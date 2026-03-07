# GhostClient

Open-source Roblox FFlag tool with dynamic offset support.

Offsets are fetched automatically from the API — no hardcoded values, always up to date.

## Requirements

- Windows 10/11
- Visual Studio 2019+ with C++ Desktop Development workload
- CMake 3.20+
- Git (for cloning with submodules)

## Build

```bash
git clone --recurse-submodules https://github.com/GetGhostClient/GhostClient.git
cd GhostClient
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule update --init
```

The compiled `ghostclient.exe` will be in `build/Release/`.

## Usage

1. Launch Roblox first
2. Run `ghostclient.exe` (as Administrator for memory access)
3. Go to **Process > Attach to Roblox** (should auto attach)
4. If this is your first run, go to **Settings > Update Offsets** to download the latest offsets
5. Use the **FFlag Browser** tab to search and read/write individual flags
6. Use the **Injector** tab to paste a list of flags or load from file
7. Use the **Presets** tab to save/load flag configurations

## Dynamic Offsets

GhostClient fetches FFlag offsets at runtime from the API instead of shipping a hardcoded list.

- On first launch, go to **Settings > Update Offsets** to download offsets
- Once downloaded, they are cached locally at `%LOCALAPPDATA%\GhostClient\fflag_cache.hpp`
- The cache is loaded automatically on every subsequent launch
- Use **Settings > Check Version** to see if a newer version is available
- Use **Settings > Update Offsets** again any time Roblox updates

Offsets sourced from [imtheo.lol/Offsets](https://imtheo.lol/Offsets).

### Injector Formats

**Text format** (one per line):
```
FlagName=value
AnotherFlag=true
SomeNumber=42
```

**JSON format**:
```json
{
    "FlagName": "value",
    "AnotherFlag": true,
    "SomeNumber": 42
}
```
