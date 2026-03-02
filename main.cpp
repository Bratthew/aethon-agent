//SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 Aethon Technologies LLC

// Dynamic-loads nvml.dll at runtime so the binary never hard-depends on NVIDIA
// drivers. On AMD / Intel-only machines, it emits an error JSON and keeps the process alive.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h> // linked via cmake
#include <tlhelp32.h> // CreateToolhelp32Snapshot for gamer mode

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>
#include <optional>
#include <sys/stat.h>
#include <functional>

#pragma comment(lib, "winhttp.lib") // belt and suspender; cmake links this as well

using nvmlReturn_t = unsigned int;
using nvmlDevice_t = void*;

constexpr nvmlReturn_t NVML_SUCCESS = 0;

enum nvmlTemperatureSensors_t : unsigned int {
    NVML_TEMPERATURE_GPU = 0
};

struct nvmlMemory_t {
    unsigned long long total; // these are all bytes
    unsigned long long free;
    unsigned long long used;
};

// Function-pointer typedefs

using PFN_nvmlInit_v2 = nvmlReturn_t (*)();
using PFN_nvmlShutdown = nvmlReturn_t (*)();
using PFN_nvmlDeviceGetCount_v2 = nvmlReturn_t (*)(unsigned int* deviceCount);
using PFN_nvmlDeviceGetHandleByIndex_v2 = nvmlReturn_t (*)(unsigned int index, nvmlDevice_t* device);
using PFN_nvmlDeviceGetName = nvmlReturn_t (*)(nvmlDevice_t device, char* name, unsigned int length);
using PFN_nvmlDeviceGetMemoryInfo = nvmlReturn_t (*)(nvmlDevice_t device, nvmlMemory_t* memory);
using PFN_nvmlDeviceGetTemperature = nvmlReturn_t (*)(nvmlDevice_t device, nvmlTemperatureSensors_t sensorType, unsigned int* temp);

// Wrapper
// holds the DLL handle + resolved pointers

struct NvmlApi {
    HMODULE dll = nullptr;
    PFN_nvmlInit_v2 Init = nullptr;
    PFN_nvmlShutdown Shutdown = nullptr;
    PFN_nvmlDeviceGetCount_v2 DeviceGetCount = nullptr;
    PFN_nvmlDeviceGetHandleByIndex_v2 DeviceGetHandle = nullptr;
    PFN_nvmlDeviceGetName DeviceGetName = nullptr;
    PFN_nvmlDeviceGetMemoryInfo DeviceGetMemory = nullptr;
    PFN_nvmlDeviceGetTemperature DeviceGetTemp = nullptr;

    // load and resolve
    [[nodiscard]] bool Load() {
        dll = ::LoadLibraryA("nvml.dll");
        if (!dll) return false;

        auto get = [&](const char* sym) -> void* {
            return reinterpret_cast<void*>(::GetProcAddress(dll, sym));
        };

        Init            = reinterpret_cast<PFN_nvmlInit_v2>(get("nvmlInit_v2"));
        Shutdown        = reinterpret_cast<PFN_nvmlShutdown>(get("nvmlShutdown"));
        DeviceGetCount  = reinterpret_cast<PFN_nvmlDeviceGetCount_v2>(get("nvmlDeviceGetCount_v2"));
        DeviceGetHandle = reinterpret_cast<PFN_nvmlDeviceGetHandleByIndex_v2>(get("nvmlDeviceGetHandleByIndex_v2"));
        DeviceGetName   = reinterpret_cast<PFN_nvmlDeviceGetName>(get("nvmlDeviceGetName"));
        DeviceGetMemory = reinterpret_cast<PFN_nvmlDeviceGetMemoryInfo>(get("nvmlDeviceGetMemoryInfo"));
        DeviceGetTemp   = reinterpret_cast<PFN_nvmlDeviceGetTemperature>(get("nvmlDeviceGetTemperature"));

        // every pointer must resolve for us to be operational
        return Init && Shutdown && DeviceGetCount && DeviceGetHandle && DeviceGetName && DeviceGetMemory && DeviceGetTemp;
    }

    // RAII cleanup
    void Unload() {
        if (dll) {
            if (Shutdown) Shutdown();
            ::FreeLibrary(dll);
            dll = nullptr;
        }
    }

    ~NvmlApi() { Unload(); }

    NvmlApi() = default;
    NvmlApi(const NvmlApi&) = delete;
    NvmlApi& operator=(const NvmlApi&) = delete;
    NvmlApi(NvmlApi&& o) noexcept { *this = std::move(o); }
    NvmlApi& operator=(NvmlApi&& o) noexcept {
        if (this != &o) { Unload(); std::memcpy(this, &o, sizeof(*this)); o.dll = nullptr; }
        return *this;
    }
};

// bytes to MiB, this matches nvidia-smi convention
static unsigned long long to_mib(unsigned long long bytes) {
    return bytes / (1024ULL * 1024ULL);
}

// trim leading/trailing whitespace and control chars
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// escape a string for safe embedding inside a JSON value
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Convert a wide (WCHAR/wchar_t) string to a narrow std::string.
// Uses WideCharToMultiByte with CP_ACP. Safe for ASCII exe names.
static std::string wchar_to_string(const wchar_t* ws) {
    if (!ws || !ws[0]) return {};
    int len = ::WideCharToMultiByte(CP_ACP, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0'); // len includes null terminator
    ::WideCharToMultiByte(CP_ACP, 0, ws, -1, &out[0], len, nullptr, nullptr);
    return out;
}

// minimal JSON value extractor
// Pulls string vlaue for a given key from a flat json object
// only handles simple {"key": "value", ...} - no nesting no arrays
// returns empty string if key not found
static std::string json_extract_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};

    // find colon after key
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};

    // find opening quote of the value
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};

    // find closing quote (handle escaped quotes)
    auto start = pos + 1;
    auto end = start;
    while (end < json.size()) {
        if (json[end] == '\\') {
            end += 2;
            continue;
        }
        if (json[end] == '"') break;
        ++end;
    }

    return json.substr(start, end - start);
}

// minimal JSON boolean extractor
// Pulls a bool value for a given key from a flat JSON object.
// Returns default_val if key not found.
static bool json_extract_bool(const std::string& json, const std::string& key, bool default_val = false) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_val;

    // skip whitespace after colon
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;

    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true") return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") return false;
    return default_val;
}

// minimal JSON integer extractor
// Pulls an integer value for a given key from a flat JSON object.
// Returns default_val if key not found.
static int json_extract_int(const std::string& json, const std::string& key, int default_val = 0) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_val;

    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;

    if (pos >= json.size()) return default_val;
    bool negative = false;
    if (json[pos] == '-') { negative = true; ++pos; }
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return negative ? -val : val;
}

// minimal JSON string array extractor
// Pulls a string array for a given key from a flat JSON object.
// Handles: "key": ["val1", "val2", ...]
// Returns empty vector if key not found or malformed.
static std::vector<std::string> json_extract_string_array(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return result;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return result;

    pos = json.find('[', pos + 1);
    if (pos == std::string::npos) return result;

    auto end_bracket = json.find(']', pos);
    if (end_bracket == std::string::npos) return result;

    std::string array_content = json.substr(pos + 1, end_bracket - pos - 1);
    size_t i = 0;
    while (i < array_content.size()) {
        auto quote_start = array_content.find('"', i);
        if (quote_start == std::string::npos) break;
        auto str_start = quote_start + 1;
        auto str_end = str_start;
        while (str_end < array_content.size()) {
            if (array_content[str_end] == '\\') { str_end += 2; continue; }
            if (array_content[str_end] == '"') break;
            ++str_end;
        }
        result.push_back(array_content.substr(str_start, str_end - str_start));
        i = str_end + 1;
    }
    return result;
}

// case-insensitive string comparison helper
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// case-insensitive substring search
static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// shell helper - run a command, capture stdout, return exit code
// runs 'cmd' in a shell via _popen. Captures stdout into `output_out`
// returns the process exit code or -1 if the pipe couldnt be opened
static int run_command(const char* cmd, std::string& output_out) {
    output_out.clear();
    FILE* pipe = ::_popen(cmd, "r");
    if (!pipe) return -1;

    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe))
        output_out += buf;
    
    int rc = ::_pclose(pipe);
    output_out = trim(output_out);
    return rc;
}

// Docker check
// this runs `docker --version` via _popen
// on success: writes the parsed version string into `version_out`, returns true
// on failure (not installed, daemon down, etc): returns false
static bool check_docker(std::string& version_out) {
    std::string output;
    int rc = run_command("docker --version 2>&1", output);
    if (rc != 0 || output.empty()) return false;

    // typical output: "Docker version xx.x.x, build xxxxx"
    // extract just the version number for a cleaner JSON field
    const std::string marker = "Docker version ";
    auto pos = output.find(marker);
    if (pos != std::string::npos) {
        auto ver_start = pos + marker.size();
        auto ver_end = output.find_first_of(", \t", ver_start);
        version_out = output.substr(ver_start, ver_end - ver_start);
    } else {
        // unexpected format - return the whole trimmed string
        version_out = output;
    }

    return true;
}

// workload spawner
// workload lifecycle states. Serialised into JSON heartbeat as a string
enum class WorkloadStatus {
    Idle,       // Not attempted / Docker unavailable
    Starting,   // 'docker run' issued, not yet confirmed
    Running,    // Container confirmed running, ready for jobs
    Pulling,    // 'ollama pull' in progress
    Failed,     // A Docker command returned an error
    Paused      // Game detected — workloads frozen/stopped
};

static const char* workload_status_str(WorkloadStatus s) {
    switch (s) {
        case WorkloadStatus::Idle:     return "idle";
        case WorkloadStatus::Starting: return "starting";
        case WorkloadStatus::Running:  return "running";
        case WorkloadStatus::Pulling:  return "pulling";
        case WorkloadStatus::Failed:   return "failed";
        case WorkloadStatus::Paused:   return "paused";
    }
    return "unknown";
}

static constexpr const char* OLLAMA_CONTAINER_NAME = "aethon_ollama";

// check whether a container with the given name exits (running OR stopped)
static bool container_exists(const char* name) {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker ps -aq -f name=^%s$ 2>&1", name);
    std::string output;
    int rc = run_command(cmd, output);
    return (rc == 0 && !output.empty());
}

// check whether a container is currently running
static bool container_running(const char* name) {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker ps -q -f name=^%s$ -f status=running 2>&1", name);
    std::string output;
    int rc = run_command(cmd, output);
    return (rc == 0 && !output.empty());
}

// check whether a container is currently paused
static bool container_paused(const char* name) {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker ps -q -f name=^%s$ -f status=paused 2>&1", name);
    std::string output;
    int rc = run_command(cmd, output);
    return (rc == 0 && !output.empty());
}

// pause a container in-place (freezes all processes, instant resume)
static bool PauseOllamaContainer() {
    if (container_paused(OLLAMA_CONTAINER_NAME)) {
        std::fprintf(stderr, "[gamer] Container '%s' already paused.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    if (!container_running(OLLAMA_CONTAINER_NAME)) {
        std::fprintf(stderr, "[gamer] Container '%s' not running — nothing to pause.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker pause %s 2>&1", OLLAMA_CONTAINER_NAME);
    std::string output;
    int rc = run_command(cmd, output);
    if (rc == 0) {
        std::fprintf(stderr, "[gamer] Container '%s' paused successfully.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    std::fprintf(stderr, "[gamer] ERROR: docker pause failed (rc=%d): %s\n", rc, output.c_str());
    return false;
}

// unpause a paused container
static bool UnpauseOllamaContainer() {
    if (!container_paused(OLLAMA_CONTAINER_NAME)) {
        std::fprintf(stderr, "[gamer] Container '%s' not paused — skipping unpause.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker unpause %s 2>&1", OLLAMA_CONTAINER_NAME);
    std::string output;
    int rc = run_command(cmd, output);
    if (rc == 0) {
        std::fprintf(stderr, "[gamer] Container '%s' unpaused successfully.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    std::fprintf(stderr, "[gamer] ERROR: docker unpause failed (rc=%d): %s\n", rc, output.c_str());
    return false;
}

// stop a container entirely (frees all resources, requires full restart)
static bool StopOllamaContainer() {
    if (!container_running(OLLAMA_CONTAINER_NAME) && !container_paused(OLLAMA_CONTAINER_NAME)) {
        std::fprintf(stderr, "[gamer] Container '%s' not running — nothing to stop.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker stop %s 2>&1", OLLAMA_CONTAINER_NAME);
    std::string output;
    int rc = run_command(cmd, output);
    if (rc == 0) {
        std::fprintf(stderr, "[gamer] Container '%s' stopped successfully.\n", OLLAMA_CONTAINER_NAME);
        return true;
    }
    std::fprintf(stderr, "[gamer] ERROR: docker stop failed (rc=%d): %s\n", rc, output.c_str());
    return false;
}

// Ensure the Ollama container is ready.
static WorkloadStatus EnsureOllamaRunning() {
    if (container_running(OLLAMA_CONTAINER_NAME)) {
        return WorkloadStatus::Running;
    }

    // crash recovery: if agent was killed while Docker was paused, unpause it
    if (container_paused(OLLAMA_CONTAINER_NAME)) {
        std::fprintf(stderr, "[ollama] Container '%s' is paused (crash recovery?) — unpausing...\n", OLLAMA_CONTAINER_NAME);
        UnpauseOllamaContainer();
        if (container_running(OLLAMA_CONTAINER_NAME)) {
            return WorkloadStatus::Running;
        }
    }

    if (container_exists(OLLAMA_CONTAINER_NAME)) {
        std::fprintf(stderr, "[ollama] Container '%s' exists but is stopped - restarting...\n", OLLAMA_CONTAINER_NAME);
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "docker start %s 2>&1", OLLAMA_CONTAINER_NAME);
        std::string output;
        int rc = run_command(cmd, output);
        if (rc == 0) {
            std::fprintf(stderr, "[ollama] Restarted '%s' successfully.\n", OLLAMA_CONTAINER_NAME);
            return WorkloadStatus::Running;
        }
        std::fprintf(stderr, "[ollama] 'docker start' failed: %s\n", output.c_str());
        return WorkloadStatus::Failed;
    }

    std::fprintf(stderr, "[ollama] Container '%s' not found - creating...\n", OLLAMA_CONTAINER_NAME);
    const char* run_cmd =
        "docker run -d --gpus all"
        " -v ollama:/root/.ollama -p 11434:11434"
        " --name aethon_ollama ollama/ollama 2>&1";
    std::string output;
    int rc = run_command(run_cmd, output);
    if (rc == 0 && !output.empty()) {
        std::fprintf(stderr, "[ollama] Container started: %.12s...\n", output.c_str());
        return WorkloadStatus::Running;
    }
    std::fprintf(stderr, "[ollama] 'docker run' failed (rc=%d): %s\n", rc, output.c_str());
    return WorkloadStatus::Failed;
}

// query Ollama container for locally cached models
static std::vector<std::string> QueryCachedModels() {
    if (!container_running(OLLAMA_CONTAINER_NAME)) {
        return {};
    }

    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "docker exec %s ollama list 2>&1", OLLAMA_CONTAINER_NAME);
    std::string output;
    int rc = run_command(cmd, output);
    if (rc != 0 || output.empty()) {
        return {};
    }

    std::vector<std::string> models;
    std::istringstream stream(output);
    std::string line;
    bool header_skipped = false;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (!header_skipped) {
            if (icontains(line, "NAME") && icontains(line, "SIZE")) {
                header_skipped = true;
                continue;
            }
            header_skipped = true;
        }

        auto name_end = line.find_first_of(" \t");
        std::string model_name;
        if (name_end != std::string::npos) {
            model_name = line.substr(0, name_end);
        } else {
            model_name = line;
        }

        model_name = trim(model_name);
        if (!model_name.empty() && model_name.size() <= 200) {
            models.push_back(model_name);
        }

        if (models.size() >= 100) break;
    }

    return models;
}

// agent config - loaded from aethon_config.json
static constexpr const char* CONFIG_FILENAME = "aethon_config.json";

struct AgentConfig {
    std::string agent_id;
    std::string api_key;
    bool loaded = false;

    static std::string GetConfigPath() {
        char exe_path[MAX_PATH] = {};
        DWORD len = ::GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (len == 0) return CONFIG_FILENAME;
        std::string dir(exe_path, len);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos)
            dir = dir.substr(0, slash + 1);
        else
            dir.clear();
        return dir + CONFIG_FILENAME;
    }

    bool LoadConfig() {
        std::string path = GetConfigPath();
        std::ifstream file(path);
        if (!file.is_open()) return false;
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();
        agent_id = json_extract_string(content, "agent_id");
        api_key = json_extract_string(content, "api_key");
        if (agent_id.empty() || api_key.empty()) {
            std::fprintf(stderr, "[agent] ERROR: Config file at '%s' is malformed.\n", path.c_str());
            return false;
        }
        loaded = true;
        std::fprintf(stderr, "[agent] Loaded config from '%s'. agent_id=%.8s...\n", path.c_str(), agent_id.c_str());
        return true;
    }

    bool SaveConfig(const std::string& new_agent_id, const std::string& new_api_key) {
        std::string path = GetConfigPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            std::fprintf(stderr, "[agent] ERROR: Could not open config file for writing at '%s'.\n", path.c_str());
            return false;
        }
        file << "{\n"
             << "  \"agent_id\": \"" << json_escape(new_agent_id) << "\",\n"
             << "  \"api_key\": \"" << json_escape(new_api_key) << "\"\n"
             << "}\n";
        if (!file) {
            std::fprintf(stderr, "[agent] ERROR: Failed to write to config file '%s'.\n", path.c_str());
            return false;
        }
        file.close();
        std::fprintf(stderr, "[agent] Config saved successfully to '%s'.\n", path.c_str());
        return true;
    }
};

static constexpr const char* GAMER_CONFIG_FILENAME = "gamer_mode_config.json";

// System processes that must NEVER be in the watchlist (prevents abuse/false triggers)
static const std::vector<std::string> BLOCKLIST = {
    "svchost.exe", "explorer.exe", "csrss.exe", "lsass.exe", "dwm.exe",
    "System", "services.exe", "smss.exe", "taskhostw.exe", "RuntimeBroker.exe",
    "cmd.exe", "powershell.exe", "conhost.exe", "agent.exe", "winlogon.exe",
    "wininit.exe", "docker.exe", "ollama.exe"
};

// Default watchlist of well-known game executables
static const std::vector<std::string> DEFAULT_WATCHLIST = {
    // Steam / Valve
    "cs2.exe", "csgo.exe", "dota2.exe", "tf2.exe", "tf_win64.exe", "hl2.exe",
    "left4dead2.exe", "portal2.exe",
    // Epic / Fortnite / Rocket League
    "FortniteClient-Win64-Shipping.exe", "RocketLeague.exe",
    "UnrealTournament.exe",
    // Riot
    "VALORANT-Win64-Shipping.exe", "LeagueofLegends.exe", "RiotClientServices.exe",
    // Blizzard
    "Overwatch.exe", "Diablo IV.exe", "wow.exe", "WowClassic.exe",
    "Hearthstone.exe", "StarCraft II.exe",
    // Rockstar
    "GTA5.exe", "RDR2.exe", "PlayGTAV.exe",
    // FromSoftware
    "eldenring.exe", "DarkSoulsIII.exe", "sekiro.exe",
    // CDPR
    "cyberpunk2077.exe", "Witcher3.exe",
    // Larian
    "bg3.exe", "bg3_dx11.exe",
    // Mojang / Minecraft (Java runs via javaw when launched through launcher)
    "Minecraft.Windows.exe", "javaw.exe",
    // Ubisoft
    "ACValhalla.exe", "RainbowSix.exe", "R6-Siege.exe",
    // EA
    "apex_legends.exe", "bf2042.exe", "FIFA24.exe",
    // Other popular titles
    "Helldivers2.exe", "Palworld-Win64-Shipping.exe",
    "Lethal Company.exe", "EscapeFromTarkov.exe",
    "PUBG-Win64-Shipping.exe", "DeadByDaylight-Win64-Shipping.exe",
    "Hogwarts Legacy.exe", "Remnant2-Win64-Shipping.exe"
};

// Known game directories — a verified game exe must reside under one of these
static const std::vector<std::string> VALID_GAME_PATHS = {
    "\\Steam\\steamapps\\common\\",
    "\\SteamLibrary\\steamapps\\common\\",
    "\\Epic Games\\",
    "\\Riot Games\\",
    "\\Battle.net\\",
    "\\GOG Galaxy\\Games\\",
    "\\Ubisoft Game Launcher\\games\\",
    "\\Ubisoft\\Ubisoft Game Launcher\\games\\",
    "\\EA Games\\",
    "\\Origin Games\\",
    "\\Program Files\\",
    "\\Program Files (x86)\\"
};

// Directories that must NOT contain a valid game exe
static const std::vector<std::string> REJECTED_PATHS = {
    "\\Windows\\", "\\System32\\", "\\SysWOW64\\",
    "\\Temp\\", "\\Downloads\\"
};

// Known game launcher parent processes
static const std::vector<std::string> KNOWN_LAUNCHERS = {
    "steam.exe", "EpicGamesLauncher.exe", "RiotClientServices.exe",
    "Battle.net.exe", "GalaxyClient.exe", "UbisoftConnect.exe",
    "Origin.exe", "EADesktop.exe"
};

struct DetectedGame {
    std::string exe_name;
    std::string exe_path;
    DWORD process_id = 0;
    DWORD parent_process_id = 0;
    bool verified = false;
};

struct GamerModeConfig {
    bool enabled = false;
    std::string pause_method = "pause"; // "pause" or "stop"
    int grace_period_ms = 30000;        // 30s cooldown between transitions
    std::vector<std::string> watchlist;        // built-in watchlist
    std::vector<std::string> custom_watchlist;  // user-added entries
    std::vector<std::string> merged_watchlist;  // watchlist + custom_watchlist (computed)
    time_t config_mtime = 0;                   // last modification time of config file

    static std::string GetConfigPath() {
        char exe_path[MAX_PATH] = {};
        DWORD len = ::GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (len == 0) return GAMER_CONFIG_FILENAME;
        std::string dir(exe_path, len);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos)
            dir = dir.substr(0, slash + 1);
        else
            dir.clear();
        return dir + GAMER_CONFIG_FILENAME;
    }

    // Validate an exe name against the blocklist (returns true if safe)
    static bool IsAllowedExe(const std::string& name) {
        for (const auto& blocked : BLOCKLIST) {
            if (iequals(name, blocked)) return false;
        }
        return true;
    }

    void MergeWatchlist() {
        merged_watchlist.clear();
        // Add built-in entries
        for (const auto& entry : watchlist) {
            if (IsAllowedExe(entry))
                merged_watchlist.push_back(entry);
        }
        // Add custom entries (validated against blocklist)
        for (const auto& entry : custom_watchlist) {
            if (IsAllowedExe(entry))
                merged_watchlist.push_back(entry);
        }
    }

    bool LoadConfig() {
        std::string path = GetConfigPath();
        std::ifstream file(path);
        if (!file.is_open()) {
            // No config file,create default
            std::fprintf(stderr, "[gamer] No config file found at '%s' — creating default.\n", path.c_str());
            CreateDefault();
            return true;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();
        file.close();

        enabled = json_extract_bool(content, "enabled", false);
        pause_method = json_extract_string(content, "pause_method");
        if (pause_method.empty()) pause_method = "pause";
        grace_period_ms = json_extract_int(content, "grace_period_ms", 30000);

        watchlist = json_extract_string_array(content, "watchlist");
        custom_watchlist = json_extract_string_array(content, "custom_watchlist");

        // If watchlist is empty (user deleted it), restore defaults
        if (watchlist.empty()) {
            watchlist = DEFAULT_WATCHLIST;
        }

        MergeWatchlist();
        UpdateMtime();

        std::fprintf(stderr, "[gamer] Config loaded: enabled=%s, method=%s, watchlist=%zu entries.\n",
                     enabled ? "true" : "false", pause_method.c_str(), merged_watchlist.size());
        return true;
    }

    void CreateDefault() {
        enabled = false;
        pause_method = "pause";
        grace_period_ms = 30000;
        watchlist = DEFAULT_WATCHLIST;
        custom_watchlist.clear();
        MergeWatchlist();
        SaveConfig();
    }

    bool SaveConfig() const {
        std::string path = GetConfigPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            std::fprintf(stderr, "[gamer] ERROR: Could not write config to '%s'.\n", path.c_str());
            return false;
        }

        file << "{\n";
        file << "  \"enabled\": " << (enabled ? "true" : "false") << ",\n";
        file << "  \"pause_method\": \"" << json_escape(pause_method) << "\",\n";
        file << "  \"grace_period_ms\": " << grace_period_ms << ",\n";

        // Write watchlist array
        file << "  \"watchlist\": [\n";
        for (size_t i = 0; i < watchlist.size(); ++i) {
            file << "    \"" << json_escape(watchlist[i]) << "\"";
            if (i + 1 < watchlist.size()) file << ",";
            file << "\n";
        }
        file << "  ],\n";

        // Write custom_watchlist array
        file << "  \"custom_watchlist\": [\n";
        for (size_t i = 0; i < custom_watchlist.size(); ++i) {
            file << "    \"" << json_escape(custom_watchlist[i]) << "\"";
            if (i + 1 < custom_watchlist.size()) file << ",";
            file << "\n";
        }
        file << "  ]\n";
        file << "}\n";

        if (!file) {
            std::fprintf(stderr, "[gamer] ERROR: Failed writing config to '%s'.\n", path.c_str());
            return false;
        }
        file.close();
        std::fprintf(stderr, "[gamer] Config saved to '%s'.\n", path.c_str());
        return true;
    }

    void UpdateMtime() {
        struct _stat st{};
        std::string path = GetConfigPath();
        if (::_stat(path.c_str(), &st) == 0) {
            config_mtime = st.st_mtime;
        }
    }

    // Returns true if the config file has been modified since last load
    bool HasFileChanged() const {
        struct _stat st{};
        std::string path = GetConfigPath();
        if (::_stat(path.c_str(), &st) == 0) {
            return st.st_mtime != config_mtime;
        }
        return false;
    }
};

// Hardware Protection Config
static constexpr const char* HWPROT_CONFIG_FILENAME = "hardware_protection_config.json";

struct HardwareProtectionConfig {
    bool enabled = true;
    std::string throttle_method = "pause"; // "pause" or "stop"
    int temp_warning = 80;
    int temp_critical = 90;
    int temp_resume = 75;
    bool vram_guard_enabled = true;
    int vram_min_free_mb = 2048;
    time_t config_mtime = 0;

    static std::string GetConfigPath() {
        char exe_path[MAX_PATH] = {};
        DWORD len = ::GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (len == 0) return HWPROT_CONFIG_FILENAME;
        std::string dir(exe_path, len);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos)
            dir = dir.substr(0, slash + 1);
        else
            dir.clear();
        return dir + HWPROT_CONFIG_FILENAME;
    }

    bool LoadConfig() {
        std::string path = GetConfigPath();
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "[thermal] No config file found, creating defaults at %s\n", path.c_str());
            CreateDefault();
            return true;
        }
        char buf[2048] = {};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        std::string json(buf);

        enabled = json_extract_string(json, "enabled") != "false";
        std::string method = json_extract_string(json, "throttle_method");
        throttle_method = (method == "stop") ? "stop" : "pause";

        int val;
        val = json_extract_int(json, "temp_warning");
        if (val > 0) temp_warning = val;
        val = json_extract_int(json, "temp_critical");
        if (val > 0) temp_critical = val;
        val = json_extract_int(json, "temp_resume");
        if (val > 0) temp_resume = val;
        val = json_extract_int(json, "vram_min_free_mb");
        if (val > 0) vram_min_free_mb = val;

        std::string vg = json_extract_string(json, "vram_guard_emabled");
        vram_guard_enabled = (vg != "false");

        UpdateMtime();
        std::fprintf(stderr, "[thermal] Config loaded: enabled=%s method=%s warn=%dC crit=%dC resume=%dC vram_guard=%s min_free=%dMB\n",
                     enabled ? "true" : "false", throttle_method.c_str(),
                     temp_warning, temp_critical, temp_resume,
                     vram_guard_enabled ? "true" : "false", vram_min_free_mb);
        return true;
    }

    void CreateDefault() {
        SaveConfig();
        UpdateMtime();
    }

    bool SaveConfig() const {
        std::string path = GetConfigPath();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "[thermal] ERROR: Cannot write config to %s\n", path.c_str());
            return false;
        }
        std::fprintf(f,
            "{\n"
            "  \"enabled\": %s,\n"
            "  \"throttle_method\": \"%s\",\n"
            "  \"temp_warning\": %d,\n"
            "  \"temp_critical\": %d,\n"
            "  \"temp_resume\": %d,\n"
            "  \"vram_guard_enabled\": %s,\n"
            "  \"vram_min_free_mb\": %d\n"
            "}\n",
            enabled ? "true" : "false",
            throttle_method.c_str(),
            temp_warning, temp_critical, temp_resume,
            vram_guard_enabled ? "true" : "false",
            vram_min_free_mb);

        std::fclose(f);
        return true;
    }

    void UpdateMtime() {
        struct _stat st{};
        if (_stat(GetConfigPath().c_str(), &st) == 0)
            config_mtime = st.st_mtime;
    }
    
    bool HasFileChanged() const {
        struct _stat st {};
        if (_stat(GetConfigPath().c_str(), &st) != 0) return false;
        return st.st_mtime != config_mtime;
    }
};

class GameDetector {
public:
    void SetWatchlist(const std::vector<std::string>& list) { watchlist_ = list; }
    void SetEnabled(bool enabled) { enabled_ = enabled; }

    // Scan running processes for any game from the watchlist.
    // Returns the first verified match, or nullopt if no games detected.
    std::optional<DetectedGame> Scan() const {
        if (!enabled_ || watchlist_.empty())
            return std::nullopt;

        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return std::nullopt;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);

        std::optional<DetectedGame> result;

        if (::Process32FirstW(snapshot, &pe)) {
            do {
                std::string exe_name = wchar_to_string(pe.szExeFile);

                // Check if this process is in our watchlist (case-insensitive)
                bool in_watchlist = false;
                for (const auto& game : watchlist_) {
                    if (iequals(exe_name, game)) {
                        in_watchlist = true;
                        break;
                    }
                }
                if (!in_watchlist) continue;

                // Found a watchlist match — resolve full path and validate
                DetectedGame game;
                game.exe_name = exe_name;
                game.process_id = pe.th32ProcessID;
                game.parent_process_id = pe.th32ParentProcessID;

                // Get full exe path
                HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (proc) {
                    char full_path[MAX_PATH] = {};
                    DWORD path_len = MAX_PATH;
                    if (::QueryFullProcessImageNameA(proc, 0, full_path, &path_len)) {
                        game.exe_path = std::string(full_path, path_len);
                    }
                    ::CloseHandle(proc);
                }

                // Security validation: path OR parent process must pass
                bool path_ok = !game.exe_path.empty() && ValidateGamePath(game.exe_path);
                bool parent_ok = ValidateParentProcess(game.parent_process_id);

                game.verified = path_ok || parent_ok;
                if (game.verified) {
                    result = game;
                    break;
                } else {
                    std::fprintf(stderr, "[gamer] WARNING: '%s' (PID %lu) matched watchlist but failed security validation. Path: '%s'\n",
                                 exe_name.c_str(), pe.th32ProcessID, game.exe_path.c_str());
                }
            } while (::Process32NextW(snapshot, &pe));
        }

        ::CloseHandle(snapshot);
        return result;
    }

private:
    std::vector<std::string> watchlist_;
    bool enabled_ = false;

    // Validate that the exe path is in a known game directory
    static bool ValidateGamePath(const std::string& path) {
        // First reject dangerous directories
        for (const auto& rejected : REJECTED_PATHS) {
            if (icontains(path, rejected))
                return false;
        }
        // Then require a known game directory
        for (const auto& valid : VALID_GAME_PATHS) {
            if (icontains(path, valid))
                return true;
        }
        return false;
    }

    // Validate that the parent process is a known game launcher
    static bool ValidateParentProcess(DWORD parent_pid) {
        if (parent_pid == 0) return false;

        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        std::string parent_name;

        if (::Process32FirstW(snapshot, &pe)) {
            do {
                if (pe.th32ProcessID == parent_pid) {
                    parent_name = wchar_to_string(pe.szExeFile);
                    break;
                }
            } while (::Process32NextW(snapshot, &pe));
        }
        ::CloseHandle(snapshot);

        if (parent_name.empty()) return false;

        // Reject cmd.exe / powershell.exe as parents (security)
        if (iequals(parent_name, "cmd.exe") || iequals(parent_name, "powershell.exe"))
            return false;

        // Accept known game launchers
        for (const auto& launcher : KNOWN_LAUNCHERS) {
            if (iequals(parent_name, launcher))
                return true;
        }

        // explorer.exe as parent is inconclusive — fall back to path check
        return false;
    }
};

class NetworkClient {
public:
    void SetBearerToken(const std::string& token) {
        bearer_token_ = token;
    }

    DWORD Post(const std::string& url, const std::string& json_payload, std::string& response_body_out) {
        response_body_out.clear();
        std::wstring wide_url(url.begin(), url.end());
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwSchemeLength = static_cast<DWORD>(-1);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);

        if (!::WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &uc)) {
            std::fprintf(stderr, "[http] WinHttpCrackUrl failed (%lu)\n", ::GetLastError());
            return 0;
        }

        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
        bool use_tls = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET hSession = ::WinHttpOpen(L"AethonAgent/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            std::fprintf(stderr, "[http] WinHttpOpen failed (%lu)\n", ::GetLastError());
            return 0;
        }

        HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), uc.nPort, 0);
        if (!hConnect) {
            std::fprintf(stderr, "[http] WinHttpConnect to %S failed (%lu)\n", host.c_str(), ::GetLastError());
            ::WinHttpCloseHandle(hSession);
            return 0;
        }

        DWORD flags = use_tls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            std::fprintf(stderr, "[http] WinHttpOpenRequest failed (%lu)\n", ::GetLastError());
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return 0;
        }

        const wchar_t* content_type = L"Content-Type: application/json";
        ::WinHttpAddRequestHeaders(hRequest, content_type, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

        if (!bearer_token_.empty()) {
            std::string auth_narrow = "Authorization: Bearer " + bearer_token_;
            std::wstring auth_header(auth_narrow.begin(), auth_narrow.end());
            ::WinHttpAddRequestHeaders(hRequest, auth_header.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = ::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(json_payload.data()), static_cast<DWORD>(json_payload.size()), static_cast<DWORD>(json_payload.size()), 0);

        DWORD status_code = 0;
        if (ok) {
            ok = ::WinHttpReceiveResponse(hRequest, nullptr);
        }
        if (ok) {
            DWORD size = sizeof(status_code);
            ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD bytes_available = 0;
            while (::WinHttpQueryDataAvailable(hRequest, &bytes_available) && bytes_available > 0) {
                std::vector<char> buffer(bytes_available + 1, 0);
                DWORD bytes_read = 0;
                if (::WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read) && bytes_read > 0) {
                    response_body_out.append(buffer.data(), bytes_read);
                }
            }
        } else {
            std::fprintf(stderr, "[http] Request failed (%lu)\n", ::GetLastError());
        }

        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return status_code;
    }

    DWORD Get(const std::string& url, std::string& response_body_out) {
        response_body_out.clear();
        std::wstring wide_url(url.begin(), url.end());
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwSchemeLength = static_cast<DWORD>(-1);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);

        if (!::WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &uc)) {
            std::fprintf(stderr, "[http] WinHttpCrackUrl failed (%lu)\n", ::GetLastError());
            return 0;
        }

        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
        bool use_tls = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET hSession = ::WinHttpOpen(L"AethonAgent/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            std::fprintf(stderr, "[http] WinHttpOpen failed (%lu)\n", ::GetLastError());
            return 0;
        }

        HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), uc.nPort, 0);
        if (!hConnect) {
            std::fprintf(stderr, "[http] WinHttpConnect to %S failed (%lu)\n", host.c_str(), ::GetLastError());
            ::WinHttpCloseHandle(hSession);
            return 0;
        }

        DWORD flags = use_tls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            std::fprintf(stderr, "[http] WinHttpOpenRequest failed (%lu)\n", ::GetLastError());
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return 0;
        }

        if (!bearer_token_.empty()) {
            std::string auth_narrow = "Authorization: Bearer " + bearer_token_;
            std::wstring auth_header(auth_narrow.begin(), auth_narrow.end());
            ::WinHttpAddRequestHeaders(hRequest, auth_header.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = ::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        DWORD status_code = 0;
        if (ok) {
            ok = ::WinHttpReceiveResponse(hRequest, nullptr);
        }
        if (ok) {
            DWORD size = sizeof(status_code);
            ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD bytes_available = 0;
            while (::WinHttpQueryDataAvailable(hRequest, &bytes_available) && bytes_available > 0) {
                std::vector<char> buffer(bytes_available + 1, 0);
                DWORD bytes_read = 0;
                if (::WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read) && bytes_read > 0) {
                    response_body_out.append(buffer.data(), bytes_read);
                }
            }
        } else {
            std::fprintf(stderr, "[http] GET request failed (%lu)\n", ::GetLastError());
        }

        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return status_code;
    }

    using StreamLineCallback = std::function<void(const std::string&)>;

    DWORD PostStreaming(const std::string& url, const std::string& json_payload, StreamLineCallback on_line, std::string& response_body_out) {
        response_body_out.clear();
        std::wstring wide_url(url.begin(), url.end());
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwSchemeLength = static_cast<DWORD>(-1);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);

        if (!::WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &uc)) {
            std::fprintf(stderr, "[http] WinHttpCrackUrl failed (%lu)\n", ::GetLastError());
            return 0;
        }

        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
        bool use_tls = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET hSession = ::WinHttpOpen(L"AethonAgent/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

        if (!hSession) {
            std::fprintf(stderr, "[http] WinHttpOpen failed (%lu)\n", ::GetLastError());
            return 0;
        }

        HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), uc.nPort, 0);
        if (!hConnect) {
            std::fprintf(stderr, "[http] WinHttpConnect to %S failed (%lu)\n", host.c_str(), ::GetLastError());
            ::WinHttpCloseHandle(hSession);
            return 0;
        }

        DWORD flags = use_tls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

        if (!hRequest) {
            std::fprintf(stderr, "[http] WinHttpOpenRequest failed (%lu)\n", ::GetLastError());
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return 0;
        }

        const wchar_t* content_type = L"Content-Type: application/json";
        ::WinHttpAddRequestHeaders(hRequest, content_type, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

        if (!bearer_token_.empty()) {
            std::string auth_narrow = "Authorization: Bearer " + bearer_token_;
            std::wstring auth_header(auth_narrow.begin(), auth_narrow.end());
            ::WinHttpAddRequestHeaders(hRequest, auth_header.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = ::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(json_payload.data()), static_cast<DWORD>(json_payload.size()), static_cast<DWORD>(json_payload.size()), 0);

        DWORD status_code = 0;
        if (ok) {
            ok = ::WinHttpReceiveResponse(hRequest, nullptr);
        }
        if (ok) {
            DWORD size = sizeof(status_code);
            ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size, WINHTTP_NO_HEADER_INDEX);

            std::string line_buffer;
            DWORD bytes_available = 0;
            while (::WinHttpQueryDataAvailable(hRequest, &bytes_available) && bytes_available > 0) {
                std::vector<char> buffer(bytes_available + 1, 0);
                DWORD bytes_read = 0;
                if (::WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read) && bytes_read > 0) {
                    response_body_out.append(buffer.data(), bytes_read);
                    line_buffer.append(buffer.data(), bytes_read);

                    size_t pos;
                    while ((pos = line_buffer.find('\n')) != std::string::npos) {
                        std::string line = line_buffer.substr(0, pos);
                        line_buffer.erase(0, pos + 1);
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                            line.pop_back();
                        if (!line.empty()) {
                            on_line(line);
                        }
                    }
                }
            }
            while (!line_buffer.empty() && (line_buffer.back() == '\r' || line_buffer.back() == ' '))
                line_buffer.pop_back();
            if (!line_buffer.empty()) {
                on_line(line_buffer);
            }
        } else {
            std::fprintf(stderr, "[http] Streaming request failed (%lu)\n", ::GetLastError());
        }

        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return status_code;
    }

private:
    std::string bearer_token_;
};

struct HardwareMonitor {
    NvmlApi nvml;
    nvmlDevice_t device = nullptr;
    bool gpu_ok = false;
    char gpu_name[96] = {};
    bool docker_active = false;
    std::string docker_version;
    std::atomic<WorkloadStatus> workload = WorkloadStatus::Idle;
    static constexpr int DOCKER_CHECK_INTERVAL = 5;
    int docker_tick = 0;

    // Gamer mode fields (sent in heartbeat to server)
    bool gamer_mode = false;
    std::string paused_reason; // "gaming" or ""
    unsigned int current_temp = 0;
    unsigned long long vram_free_mib = 0;
    unsigned long long vram_total_mib = 0;
    std::string thermal_state = "normal";

    std::vector<std::string> cached_models;
    int cached_models_tick = 0;
    static constexpr int CACHED_MODELS_REFRESH_INTERVAL = 6;

    void init() {
        init_gpu();
        refresh_docker();
    }

    void tick() {
        if (++docker_tick >= DOCKER_CHECK_INTERVAL) {
            refresh_docker();
            docker_tick = 0;
        }
        refresh_gpu_stats();
        if (++cached_models_tick >= CACHED_MODELS_REFRESH_INTERVAL) {
            refresh_cached_models();
            cached_models_tick = 0;
        }
    }

    void refresh_cached_models() {
        if (docker_active) {
            cached_models = QueryCachedModels();
            std::fprintf(stderr, "[ollama] Cached models refreshed: %zu model(s) found.\n", cached_models.size());
        } else {
            cached_models.clear();
        }
    }

    [[nodiscard]] std::string build_json() const {
        std::string json = "{";
        char buf[512];

        if (gpu_ok) {
            nvmlMemory_t mem{};
            nvml_mut().DeviceGetMemory(device, &mem);
            std::snprintf(buf, sizeof(buf),
                "\"gpu_name\":\"%s\",\"vram_total\":%llu,\"vram_used\":%llu,\"temp\":%u,"
                "\"docker_active\":%s,\"docker_version\":\"%s\",\"workload_status\":\"%s\","
                "\"gamer_mode\":%s,\"paused_reason\":\"%s\",\"thermal_state\":\"%s\"",
                json_escape(gpu_name).c_str(), to_mib(mem.total), to_mib(mem.used), current_temp,
                docker_active ? "true" : "false", json_escape(docker_version).c_str(),
                workload_status_str(workload.load()),
                gamer_mode ? "true" : "false",
                json_escape(paused_reason).c_str(),
                json_escape(thermal_state).c_str());
            json += buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                "\"status\":\"error\",\"msg\":\"No NVIDIA GPU detected\",\"docker_active\":%s,"
                "\"docker_version\":\"%s\",\"workload_status\":\"%s\","
                "\"gamer_mode\":%s,\"paused_reason\":\"%s\",\"thermal_state\":\"%s\"",
                docker_active ? "true" : "false", json_escape(docker_version).c_str(),
                workload_status_str(workload.load()),
                gamer_mode ? "true" : "false",
                json_escape(paused_reason).c_str(),
                json_escape(thermal_state).c_str());
            json += buf;
        }

        json += ",\"cached_models\":[";
        for (size_t i = 0; i < cached_models.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + json_escape(cached_models[i]) + "\"";
        }
        json += "]}";

        return json; 
    }

    void emit() const {
        std::printf("%s\n", build_json().c_str());
        std::fflush(stdout);
    }

    void refresh_gpu_stats() {
        if (!gpu_ok) return;
        nvmlMemory_t mem{};
        unsigned int t = 0;
        nvml_mut().DeviceGetMemory(device, &mem);
        nvml_mut().DeviceGetTemp(device, NVML_TEMPERATURE_GPU, &t);
        current_temp = t;
        vram_free_mib = to_mib(mem.free);
    }

private:
    void init_gpu() {
        if (!nvml.Load() || nvml.Init() != NVML_SUCCESS) return;
        unsigned int count = 0;
        if (nvml.DeviceGetCount(&count) != NVML_SUCCESS || count == 0) return;
        if (nvml.DeviceGetHandle(0, &device) != NVML_SUCCESS) return;
        nvml.DeviceGetName(device, gpu_name, sizeof(gpu_name));
        gpu_ok = true;
        nvmlMemory_t mem{};
        if (nvml.DeviceGetMemory(device, &mem) == NVML_SUCCESS) {
            vram_total_mib = to_mib(mem.total);
        }
    }

    void refresh_docker() {
        std::string ver;
        docker_active = check_docker(ver);
        docker_version = docker_active ? ver : "";
    }

    NvmlApi& nvml_mut() const { return const_cast<NvmlApi&>(nvml); }
};

static constexpr const char* HEARTBEAT_URL = "http://127.0.0.1:8000/heartbeat";
static constexpr const char* REGISTER_URL = "http://127.0.0.1:8000/register";
static constexpr const char* JOB_STATUS_URL_FMT = "http://127.0.0.1:8000/jobs/%s/status";
static constexpr const char* JOB_PAYLOAD_URL_FMT = "http://127.0.0.1:8000/jobs/%s/payload";
static constexpr const char* JOB_CHUNK_URL_FMT = "http://127.0.0.1:8000/jobs/%s/chunk";
static constexpr const char* OLLAMA_CHAT_URL = "http://localhost:11434/api/chat";
static constexpr const char* OLLAMA_GENERATE_URL = "http://localhost:11434/api/generate";
static constexpr int POLL_INTERVAL_SEC = 2;
static constexpr int HEARTBEAT_INTERVAL_SEC = 10;
static constexpr unsigned long long MIN_VRAM_MIB = 10240;


static void ReportJobStatus(const std::string& job_id, const char* status_str,
                            const std::string& output, const std::string& api_key) {
    NetworkClient net;
    net.SetBearerToken(api_key);
    char status_url[256];
    std::snprintf(status_url, sizeof(status_url), JOB_STATUS_URL_FMT, job_id.c_str());
    std::string payload = "{\"status\": \"" + std::string(status_str) +
                          "\", \"output\": \"" + json_escape(output) + "\"}";
    std::string response;
    DWORD http_status = net.Post(status_url, payload.c_str(), response);
    if (http_status < 200 || http_status >= 300) {
        std::fprintf(stderr, "[job:%s] ERROR: Failed to report status '%s' to server (HTTP %lu).\n",
                     job_id.c_str(), status_str, http_status);
    }
}

static void ReportJobChunk(const std::string& job_id, const std::string& content, bool done, const std::string& api_key) {
    
    NetworkClient net;
    net.SetBearerToken(api_key);
    char chunk_url[256];
    std::snprintf(chunk_url, sizeof(chunk_url), JOB_CHUNK_URL_FMT, job_id.c_str());
    std::string payload = "{\"content\": \"" + json_escape(content) +
                          "\", \"done\": " + (done ? "true" : "false") + "}";
    std::string response;
    net.Post(chunk_url, payload, response);
}



void ExecuteJob(HardwareMonitor* monitor, std::string job_id, std::string model, std::string api_key) {
    monitor->workload = WorkloadStatus::Pulling;
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "docker exec %s ollama pull %s 2>&1", OLLAMA_CONTAINER_NAME, model.c_str());
    std::fprintf(stderr, "[job:%s] Executing: %s\n", job_id.c_str(), cmd);

    std::string output;
    int rc = run_command(cmd, output);

    const char* job_status_str = "failed";
    if (rc == 0) {
        monitor->workload = WorkloadStatus::Running;
        job_status_str = "completed";
        std::fprintf(stderr, "[job:%s] Model '%s' pulled successfully.\n", job_id.c_str(), model.c_str());
        monitor->refresh_cached_models();
    } else {
        monitor->workload = WorkloadStatus::Failed;
        std::fprintf(stderr, "[job:%s] ERROR: ollama pull failed (rc=%d): %s\n", job_id.c_str(), rc, output.c_str());
    }

    ReportJobStatus(job_id, job_status_str, output, api_key);
}

static std::string extract_ollama_chat_content(const std::string& json) {
    auto msg_pos = json.find("\"message\"");
    if (msg_pos == std::string::npos) return {};

    auto brace_pos = json.find('{', msg_pos);
    if (brace_pos == std::string::npos) return {};

    auto content_key = json.find("\"content\"", brace_pos);
    if (content_key == std::string::npos) return {};

    auto colon = json.find(':', content_key + 9);
    if (colon == std::string::npos) return {};

    auto quote_start = json.find('"', colon + 1);
    if (quote_start == std::string::npos) return {};

    auto start = quote_start + 1;
    auto end = start;
    while (end < json.size()) {
        if (json[end] == '\\') {
            end += 2;
            continue;
        }
        if (json[end] == '"') break;
        ++end;
    }
    if (end >= json.size()) return {};

    std::string raw = json.substr(start, end - start);
    std::string result;
    result.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            switch (raw[i + 1]) {
                case '"': result += '"'; ++i; break;
                case '\\': result += '\\'; ++i; break;
                case 'n': result += '\n'; ++i; break;
                case 'r': result += '\r'; ++i; break;
                case 't': result += '\t'; ++i; break;
                default: result += raw[i]; break;
            }
        } else {
            result += raw[i];
        }
    }
    return result;
}

static std::string extract_ollama_generate_content(const std::string& json) {
    return json_extract_string(json, "response");
}

void ExecuteInferenceJob(HardwareMonitor* monitor, std::string job_id, std::string model, std::string api_key) {
    monitor->workload = WorkloadStatus::Pulling;
    std::fprintf(stderr, "[job:%s] Starting inference job for model '%s'.\n", job_id.c_str(), model.c_str());

    NetworkClient server_net;
    server_net.SetBearerToken(api_key);
    char payload_url[256];
    std::snprintf(payload_url, sizeof(payload_url), JOB_PAYLOAD_URL_FMT, job_id.c_str());

    std::string payload_response;
    DWORD payload_status = server_net.Get(payload_url, payload_response);
    if (payload_status < 200 || payload_status >= 300) {
        std::fprintf(stderr, "[job:%s] ERROR: Failed to fetch inference payload (HTTP %lu)\n", job_id.c_str(), payload_status);
        ReportJobStatus(job_id, "failed", "Failed to fetch inference payload from server", api_key);
        monitor->workload = WorkloadStatus::Running;
        return;
    }

    std::string inference_payload = json_extract_string(payload_response, "inference_payload");
    if (inference_payload.empty()) {
        std::fprintf(stderr, "[job:%s] ERROR: Empty inference payload received.\n", job_id.c_str());
        ReportJobStatus(job_id, "failed", "Empty inference payload", api_key);
        monitor->workload = WorkloadStatus::Running;
        return;
    }
    std::string ollama_endpoint = json_extract_string(payload_response, "ollama_endpoint");

    char pull_cmd[512];
    std::snprintf(pull_cmd, sizeof(pull_cmd), "docker exec %s ollama pull %s 2>&1", OLLAMA_CONTAINER_NAME, model.c_str());
    std::string pull_output;
    run_command(pull_cmd, pull_output);
    std::fprintf(stderr, "[job:%s] Model pull check complete.\n", job_id.c_str());

    const char* ollama_url = OLLAMA_CHAT_URL;
    bool is_generate = (ollama_endpoint == "/api/generate");
    if (is_generate) {
        ollama_url = OLLAMA_GENERATE_URL;
    }

    bool is_streaming = json_extract_bool(inference_payload, "stream", false);

    NetworkClient ollama_net;
    const char* final_status = "failed";
    std::string output;

    if (is_streaming) {
        std::string accumulated_output;
        std::string full_response;

        auto on_line = [&](const std::string& line) {
            bool done = json_extract_bool(line, "done", false);
            std::string token;

            if (is_generate) {
                token = json_extract_string(line, "response");
            } else {
                token = extract_ollama_chat_content(line);
            }

            if (!token.empty()) {
                accumulated_output += token;
                ReportJobChunk(job_id, token, false, api_key);
            }

            if (done) {
                ReportJobChunk(job_id, "", true, api_key);
            }
        };

        DWORD ollama_status = ollama_net.PostStreaming(ollama_url, inference_payload, on_line, full_response);

        if (ollama_status >= 200 && ollama_status < 300 && !accumulated_output.empty()) {
            final_status = "completed";
            output = accumulated_output;
            std::fprintf(stderr, "[job:%s] Streaming inference completed (%zu chars).\n", job_id.c_str(), output.size());
        } else {
            output = accumulated_output.empty()
                ? "Ollama streaming inference failed (HTTP " + std::to_string(ollama_status) + ")"
                : accumulated_output;
            std::fprintf(stderr, "[job:%s] ERROR: Streaming inference failed (HTTP %lu).\n", job_id.c_str(), ollama_status);
        }
    } else {
        std::string ollama_response;
        DWORD ollama_status = ollama_net.Post(ollama_url, inference_payload, ollama_response);

        if (ollama_status >= 200 && ollama_status < 300) {
            if (is_generate) {
                output = extract_ollama_generate_content(ollama_response);
            } else {
                output = extract_ollama_chat_content(ollama_response);
            }
            if (!output.empty()) {
                final_status = "completed";
                std::fprintf(stderr, "[job:%s] Inference completed successfully (%zu chars).\n", job_id.c_str(), output.size());
            } else {
                output = "Ollama returned empty response content";
                std::fprintf(stderr, "[job:%s] ERROR: Ollama returned empty content.\n", job_id.c_str());
            }
        } else {
            output = "Ollama inference failed (HTTP " + std::to_string(ollama_status) + ")";
            std::fprintf(stderr, "[job:%s] ERROR: Ollama inference failed (HTTP %lu): %.200s\n", job_id.c_str(), ollama_status, ollama_response.c_str());
        }
    }

    ReportJobStatus(job_id, final_status, output, api_key);

    monitor->workload = WorkloadStatus::Running;
    monitor->refresh_cached_models();
}

static bool AttemptRegistration(AgentConfig& config, const std::string& payload) {
    std::fprintf(stderr, "[agent] No config found. Attempting to register with server at %s...\n", REGISTER_URL);
    NetworkClient net;
    std::string response_body;
    DWORD status = net.Post(REGISTER_URL, payload, response_body);
    if (status < 200 || status >= 300) {
        std::fprintf(stderr, "[agent] ERROR: Registration failed. Server returned HTTP %lu.\n[agent] Response: %s\n", status, response_body.c_str());
        return false;
    }
    std::fprintf(stderr, "[agent] Registration successful. Parsing credentials...\n");
    std::string agent_id = json_extract_string(response_body, "agent_id");
    std::string api_key = json_extract_string(response_body, "api_key");
    if (agent_id.empty() || api_key.empty()) {
        std::fprintf(stderr, "[agent] ERROR: Could not parse 'agent_id' or 'api_key' from server response.\n[agent] Response: %s\n", response_body.c_str());
        return false;
    }
    return config.SaveConfig(agent_id, api_key);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    HardwareMonitor monitor;
    monitor.init();

    if (monitor.gpu_ok && monitor.vram_total_mib < MIN_VRAM_MIB) {
        std::printf("{\"status\":\"error\",\"msg\":\"Your GPU needs at least 10 GB VRAM to supply on Aethon. "
                    "Detected %llu MiB (%s).\"}\n",
                    monitor.vram_total_mib, json_escape(monitor.gpu_name).c_str());
        std::fflush(stdout);
        std::fprintf(stderr, "[agent] FATAL: GPU VRAM (%llu MiB) below minimum (%llu MiB). Cannot register.\n",
                     monitor.vram_total_mib, MIN_VRAM_MIB);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    }

    AgentConfig config;
    if (!config.LoadConfig()) {
        std::string payload = monitor.build_json();
        if (AttemptRegistration(config, payload)) {
            config.LoadConfig();
        }
    }
    if (!config.loaded) {
        std::fprintf(stderr, "\n[agent] FATAL: Auto-registration failed and no valid config file found.\n[agent] Heartbeats will NOT be sent.\n\n");
    }

    if (monitor.docker_active) {
        monitor.workload = WorkloadStatus::Starting;
        monitor.emit();
        monitor.workload = EnsureOllamaRunning();
    } else {
        std::fprintf(stderr, "[agent] Docker not available - skipping Ollama launch.\n");
        monitor.workload = WorkloadStatus::Idle;
    }

    NetworkClient net;
    if (config.loaded) {
        net.SetBearerToken(config.api_key);
    }

    GamerModeConfig gamer_config;
    gamer_config.LoadConfig();

    GameDetector game_detector;
    game_detector.SetWatchlist(gamer_config.merged_watchlist);
    game_detector.SetEnabled(gamer_config.enabled);

    HardwareProtectionConfig hwprot_config;
    hwprot_config.LoadConfig();

    static constexpr int CONFIG_RELOAD_TICKS = 15;  // 15 ticks * 2s = 30s
    int config_reload_tick = 0;

    // Cooldown tracking: time of last pause↔available transition
    auto last_transition_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(gamer_config.grace_period_ms);

    auto last_heartbeat = std::chrono::steady_clock::now() - std::chrono::seconds(HEARTBEAT_INTERVAL_SEC);
    std::thread job_thread;

    while (true) {
        monitor.tick();

        if (++config_reload_tick >= CONFIG_RELOAD_TICKS) {
            config_reload_tick = 0;
            if (gamer_config.HasFileChanged()) {
                std::fprintf(stderr, "[gamer] Config file changed — reloading.\n");
                gamer_config.LoadConfig();
                game_detector.SetWatchlist(gamer_config.merged_watchlist);
                game_detector.SetEnabled(gamer_config.enabled);
            }
            if (hwprot_config.HasFileChanged()) {
                std::fprintf(stderr, "[thermal] Config file changed, reloading.\n");
                hwprot_config.LoadConfig();
            }
        }

        // Game detection (runs every 2s tick)
        if (gamer_config.enabled) {
            auto detected = game_detector.Scan();
            bool was_paused = (monitor.workload.load() == WorkloadStatus::Paused);

            if (detected.has_value() && !was_paused) {
                // TRANSITION: Available → Paused
                std::fprintf(stderr, "[gamer] Game detected: %s (PID %lu, verified=%s) — pausing workloads.\n",
                             detected->exe_name.c_str(), detected->process_id,
                             detected->verified ? "yes" : "no");

                if (monitor.docker_active) {
                    if (gamer_config.pause_method == "stop") {
                        StopOllamaContainer();
                    } else {
                        PauseOllamaContainer();
                    }
                }

                monitor.workload = WorkloadStatus::Paused;
                monitor.gamer_mode = true;
                monitor.paused_reason = "gaming";
                last_transition_time = std::chrono::steady_clock::now();

            } else if (!detected.has_value() && was_paused) {
                // TRANSITION: Paused → Available (with cooldown)
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_transition_time).count();

                if (elapsed >= gamer_config.grace_period_ms) {
                    std::fprintf(stderr, "[gamer] Game closed — resuming workloads.\n");

                    if (hwprot_config.enabled &&
                        monitor.current_temp >= (unsigned int)hwprot_config.temp_critical) {
                        std::fprintf(stderr, "[thermal] GPU still at %uC after gaming — entering thermal throttle.\n",
                                     monitor.current_temp);
                        monitor.gamer_mode = false;
                        monitor.paused_reason = "thermal";
                        monitor.thermal_state = "throttled";
                    } else {
                        if (monitor.docker_active) {
                            if (gamer_config.pause_method == "stop") {
                                // Full restart needed
                                monitor.workload = EnsureOllamaRunning();
                            } else {
                                UnpauseOllamaContainer();
                                monitor.workload = container_running(OLLAMA_CONTAINER_NAME)
                                    ? WorkloadStatus::Running : WorkloadStatus::Idle;
                            }
                        } else {
                            monitor.workload = WorkloadStatus::Idle;
                        }

                        monitor.gamer_mode = false;
                        monitor.paused_reason = "";
                        last_transition_time = now;
                    }
                    last_transition_time = now;
                }
            }
            // else: no state change, either still gaming or still available
        }

        if (hwprot_config.enabled && monitor.gpu_ok) {
            unsigned int temp = monitor.current_temp;

            if (temp >= (unsigned int) hwprot_config.temp_critical && monitor.workload.load() != WorkloadStatus::Paused) {
                std::fprintf(stderr, "[thermal] CRITICAL: GPU at %uC (>=%dC), pausing workloads\n", temp, hwprot_config.temp_critical);
                if (monitor.docker_active) {
                    if (hwprot_config.throttle_method == "stop") {
                        StopOllamaContainer();
                    } else {
                        PauseOllamaContainer();
                    }
                }
                monitor.workload = WorkloadStatus::Paused;
                monitor.paused_reason = "thermal";
                monitor.thermal_state = "throttled";
            }
            else if (temp >= (unsigned int)hwprot_config.temp_warning && temp < (unsigned int)hwprot_config.temp_critical && monitor.workload.load() != WorkloadStatus::Paused) {
                monitor.thermal_state = "warning";
                static bool warning_logged = false;
                if (!warning_logged) {
                    std::fprintf(stderr, "[thermal] WARNING: GPU at %uC, apporaching critical threshold (%dC).\n", temp, hwprot_config.temp_critical);
                    warning_logged = true;
                }
            }
            else if (temp <= (unsigned int)hwprot_config.temp_resume && monitor.paused_reason == "thermal") {
                std::fprintf(stderr, "[thermal] GPU cooled to %uC (<=%dC), resuming workloads.\n", temp, hwprot_config.temp_resume);
                if (monitor.docker_active) {
                    if (hwprot_config.throttle_method == "stop") {
                        monitor.workload = EnsureOllamaRunning();
                    } else {
                        UnpauseOllamaContainer();
                        monitor.workload = container_running(OLLAMA_CONTAINER_NAME) ? WorkloadStatus::Running : WorkloadStatus::Idle;
                    }
                } else {
                    monitor.workload = WorkloadStatus::Idle;
                }
                monitor.paused_reason = "";
                monitor.thermal_state = "normal";
            }
            else if (monitor.paused_reason == "thermal") {
                monitor.thermal_state = "throttled";
            }
            else if (temp < (unsigned int)hwprot_config.temp_warning) {
                monitor.thermal_state = "normal";
                static bool& wl = *[]() -> bool* { static bool b = false; return &b; }();
            }
        }

        monitor.emit();

        // Heartbeat (every 10s)
        auto now = std::chrono::steady_clock::now();
        if (config.loaded && now - last_heartbeat >= std::chrono::seconds(HEARTBEAT_INTERVAL_SEC)) {
            // Skip heartbeat job assignment while pulling, but always send heartbeat
            // (server needs to see gamer_mode status even while paused)
            if (monitor.workload.load() != WorkloadStatus::Pulling) {
                std::string json_payload = monitor.build_json();
                std::string response_body;
                DWORD status = net.Post(HEARTBEAT_URL, json_payload, response_body);

                if (status >= 200 && status < 300) {
                    // Only accept new jobs if NOT paused and NOT pulling
                    std::string command = json_extract_string(response_body, "command");
                    if (command == "start_job" &&
                        monitor.workload.load() != WorkloadStatus::Pulling &&
                        monitor.workload.load() != WorkloadStatus::Paused) {
                        
                        // vram guard: refuse job if free VRAM is too low
                        if (hwprot_config.vram_guard_enabled && monitor.vram_free_mib < (unsigned long long)hwprot_config.vram_min_free_mb) {
                            std::fprintf(stderr, "[vram-guard] Refusing job, only %llu MiB free (need %d MiB minimum).\n", monitor.vram_free_mib, hwprot_config.vram_min_free_mb);
                        } else {
                            if (job_thread.joinable()) job_thread.join();
                            std::string model = json_extract_string(response_body, "model");
                            std::string job_id = json_extract_string(response_body, "job_id");
                            std::string job_type = json_extract_string(response_body, "job_type");
                            if (job_type == "inference") {
                                job_thread = std::thread(ExecuteInferenceJob, &monitor, job_id, model, config.api_key);
                            } else {
                                job_thread = std::thread(ExecuteJob, &monitor, job_id, model, config.api_key);
                            }
                            job_thread.detach();

                        }
                    }
                }
            }
            last_heartbeat = now;
        }
        std::this_thread::sleep_for(std::chrono::seconds(POLL_INTERVAL_SEC));
    }

    if (job_thread.joinable()) job_thread.join();
    return 0;
}