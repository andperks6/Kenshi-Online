# Kenshi-Online

**16-player co-op multiplayer mod for Kenshi.** Explore, fight, build, and
trade together in the open world. Loaded as an Ogre plugin — no DLL injectors
or process attach.

## Install (Players)

You don't need to build anything. Grab the prebuilt zip:

1. Download `Kenshi-Online-vX.Y.Z.zip` from the
   [Releases page](../../releases/latest).
2. Extract anywhere.
3. Run `install.bat` once — it auto-detects your Kenshi install, backs up the
   files it touches, and copies the DLL, GUI layouts, and `kenshi-online.mod`
   into place. Set `KENSHI_DIR` first to override auto-detection.

From now on, launch with **`KenshiMP.Injector.exe`** — set your player name and
server, click **PLAY**, and Kenshi starts with multiplayer enabled. Or launch
Kenshi normally and use the **MULTIPLAYER** button on the main menu.

Full controls, in-game commands, and troubleshooting:
[`dist/JOINING.md`](dist/JOINING.md).

> **Why two scripts?** The Injector doesn't yet install GUI layouts or take
> backups, so `install.bat` runs once on first install. Day-to-day use is the
> Injector only. The Injector will absorb the installer over time.

## Hosting a Server

Anyone can host. Three options, in order of friction:

**Docker (recommended for Linux hosts and Unraid)** — pull
`ghcr.io/andperks6/kenshi-online-server:latest`, mount a `data/` volume,
done. A Tailscale-sidecar compose recipe avoids opening UDP 27800 to the
public internet. See [`dist/server-docker/`](dist/server-docker/README.md).

**Windows binary** — copy `KenshiMP.Server.exe` (and optionally
`server.json`) from the release zip to a Windows host or VPS, run it,
forward UDP 27800 (or rely on UPnP).

**Master server** — the central server browser registry. Most people
don't need to run their own; the project hosts a default. To self-host,
run `KenshiMP.MasterServer.exe` (or use the Docker image with the
master-server entrypoint override) on UDP 27801.

`server.json` example:
```json
{
  "serverName": "My Kenshi Server",
  "port": 27800,
  "maxPlayers": 16,
  "pvpEnabled": true,
  "gameSpeed": 1.0
}
```

Server console: `status`, `players`, `kick <id>`, `say <msg>`, `save`, `stop`.

## Features

- Up to 16 players, server-authoritative combat and world state
- Full replication — characters, NPCs, combat, buildings, items, time/weather
- Zone-based interest management (3×3 zone grid around each player)
- Dedicated server + master server browser (auto-discovery)
- Uninstall path — `uninstall.bat` restores vanilla from the install backups

## Building from Source (Developers)

> Only needed if you're hacking on the mod. End users should use the prebuilt
> zip from the [Releases page](../../releases/latest).

```bash
git clone --recursive https://github.com/yourname/Kenshi-Online.git
cd Kenshi-Online
build.bat
```

Requires **Visual Studio 2022** (or 2019) with the *Desktop development with
C++* workload and **CMake 3.20+**. All dependencies are bundled as git
submodules — no vcpkg. `build.bat` configures, builds Release, and runs the
unit tests. Output lands in `build/bin/Release/`.

To open in VS: `File > Open > CMake...` → pick `CMakeLists.txt`, select the
`x64-release` preset, build.

## Project Structure

```
KenshiMP.Injector/    Win32 launcher GUI; edits Plugins_x64.cfg, launches Kenshi
KenshiMP.Core/        Ogre plugin DLL — hooks, sync, ENet client, MyGUI bridge
KenshiMP.Server/      Dedicated server (game state + networking)
KenshiMP.MasterServer/ Server browser registry (port 27801)
KenshiMP.Common/      Shared types, message protocol, serialization
KenshiMP.Scanner/     Pattern scanner + MinHook wrapper

lib/                  Bundled deps: ENet, MinHook, nlohmann/json, spdlog, ImGui
dist/                 Player-facing assets shipped in the release zip
```

## Credits

Built on community reverse engineering work:
- [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) — Ogre plugin injection approach
- [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) — game structure definitions
- [Kenshi Online](https://github.com/The404Studios/Kenshi-Online) — memory addresses reference
- [OpenConstructionSet](https://github.com/lmaydev/OpenConstructionSet) — game data SDK

## License

MIT
