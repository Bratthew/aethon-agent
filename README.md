# Aethon GPU Agent

The Aethon GPU Agent is the hypervisor that runs on supplier machines in the
[Aethon GPU marketplace](https://aethon-website.vercel.app/). It monitors NVIDIA GPUs,
manages Ollama inference workloads, and communicates with the Aethon dispatch
server.

This source is published so suppliers can audit exactly what runs on their
machine.

## What it does

- Detects NVIDIA GPUs using NVML (dynamically loaded — no hard dependency)
- Monitors GPU temperature, VRAM, and utilization
- Detects running games and auto-pauses work (Gamer Mode)
- Receives and executes inference jobs via Ollama
- Reports heartbeats and job status to the dispatch server

## Building from source

### Prerequisites

- Windows 10/11
- Visual Studio 2022 with the "Desktop development with C++" workload
- CMake 3.20 or newer
- (Optional) NVIDIA GPU with drivers installed

### Build
```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

The binary will be at `build/Release/agent.exe`.

The agent dynamically loads `nvml.dll` at runtime. On machines without an
NVIDIA GPU, it emits an error but does not crash.

## License

Business Source License 1.1 — see [LICENSE](./LICENSE) for details.

You may read, audit, and build this code for personal use. Commercial use
in a competing GPU compute marketplace is restricted until the Change Date
(2030-03-01), after which it converts to MIT.
```

### Step 6: Create a .gitignore
```
build/
*.exe
*.obj
*.pdb
.vs/
CMakeFiles/
CMakeCache.txt
cmake_install.cmake
aethon_config.json
