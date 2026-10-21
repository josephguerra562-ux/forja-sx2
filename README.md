# Forja SX2

PlayStation 2 emulator for jailbroken PS4 consoles (GoldHEN). It is based on [PS4SX2](https://github.com/Phoenixx1202/PS4SX2),
which comes from [PCSX2](https://github.com/PCSX2/pcsx2), and adds performance work, per-game profiles, save states, frame
generation, PS2 DVD copying and an in-app updater. It is still in testing: every game behaves differently, and there can be
stutters, audio dropouts or crashes.

- **Download:** the recommended version is the one marked **Latest** in
  [Releases](https://github.com/josephguerra562-ux/forja-sx2/releases/latest), for example `ForjaSX2-Emulador-v1.03.pkg`.
  It has been tested on a PS4 Pro (stable channel). The "Pre-release" builds (such as "Forja SX2 v1.03 (prueba 58)") are
  published automatically with every change and may break (testing channel). You can download whichever you like.
- **Install and use:** the full guide is in Spanish, [LEEME_FORJA_SX2.md](LEEME_FORJA_SX2.md) (installation, folders,
  controls, per-game settings and troubleshooting). The app's menus are in Spanish.
- **Community:** join the [Forja SX2 Discord](https://discord.gg/YVCQF5XFFd) (in Spanish) for news, help with installing, game reports and bug
  reports (the **reportar-errores** forum).
- **No BIOS or games are included.** You need your own PS2 BIOS, dumped from your console, and your own copies of your
  games. Please do not ask for or share those files here.

This is an independent project, not affiliated with Sony or the PCSX2 team. Credits are [at the end](#credits).

## Compatibility

Measured on a PS4 Pro with Boost Mode on, at 1x internal resolution unless stated otherwise. The recommended settings ship
in each game's factory profile (`resources/perfiles/`); whatever you change in the L3+R3 menu overrides them.

| Status | Game | Region | How it runs | Factory settings |
|---|---|---|---|---|
| Playable | Crash Nitro Kart | PAL (SLES-51511) | Full speed at 27 % PS2 CPU, also with two players in split screen (97-99 %). The game runs at 25 FPS in races; "Fluid" frame generation fills the 60 Hz output. | PS2 CPU at 27 %, "Fluid" frame generation. Experimental "50 FPS" patch: 50 FPS in races with the PS2 CPU at 36 % (factory setting since build 47). |
| In testing | Dragon Ball Z Budokai Tenkaichi 4 (fan mod) | NTSC-U (SLUS-21978) | GS-bound: ~2,100 draws a frame in fights, 25 µs each on the PS4's CPU (31 ms a vsync, 40-55 % speed). The "character shading" fingerprint brings fights to 80-100 %, and since build 73 the GS frame skip ("Saltar cuadros") on Auto, with its guard against flicker, keeps them at full speed and 30 FPS. | Budokai Tenkaichi 3's GS fixes, the "Menos efectos: sombreado de personajes" fingerprint, frame generation off, GS frame skip on Auto (since build 75). |
| Playable | God of War II | PAL (SCES-54206) | With the PS2 CPU at 27 %: 98 % average speed, full speed 83 % of the time. At 100 % CPU the heaviest fights (Colossus of Rhodes, many enemies) run at 60-85 %, limited by the VU1. | vuFlagHack, cores "Plan B", skip cutscenes, PS2 CPU at 36 % with automatic CPU down to 27 % in the heaviest fights (since build 46: ~41 FPS at the Colossus instead of ~30), "Fluid" frame generation. "Menos efectos 1 y 2 (Sony)" (fewer effects: 97 % at the Colossus, even at 3x), on by default since build 44. |
| Playable | Tales of the Abyss | USA (SLUS-21386) | Gameplay 93-100 %. Cutscenes ~78 % (23 FPS) without video mode. | PS2 CPU at 100 %, normal cores. |
| Playable | Mega Man X Command Mission | USA (SLUS-20903) | 30 FPS version: locked 30 FPS, even at 4x. The intro runs at 80-90 % with video mode. | Normal VU1 clamping. |

Tried another game? Tell us in **#juegos-probados** on the [Discord](https://discord.gg/YVCQF5XFFd) or open an issue with the game, its region and serial, how it ran (FPS and speed from the overlay) and that
session's `logs/sesiones` folder.

## Reporting a problem

Open an [issue](https://github.com/josephguerra562-ux/forja-sx2/issues/new/choose), or a post in the **reportar-errores**
forum of the [Discord](https://discord.gg/YVCQF5XFFd), and include:

1. **Console model** (CUH-xxxx; PS4 Fat, Slim or Pro) and **firmware** (for example 9.00 or 11.00), and your GoldHEN version.
2. **Forja SX2 version** (bottom left of the library, for example "Versión 1.03 (57)").
3. **What happens:** the app doesn't open, the game closes, black screen, slow... and any error code the PS4 shows (CE-xxxxx-x).
4. **The logs:** the `/data/ForjaSX2/logs` folder (via FTP). If the app closes before the library appears, that folder still
   shows how far it got. Since 1.06, `logs/resumen.txt` sums up the whole session on one page.

> The technical documentation below comes from PS4SX2. In Forja SX2 the folders are under `/data/ForjaSX2/`, and the up to
> date user guide (in Spanish) is [LEEME_FORJA_SX2.md](LEEME_FORJA_SX2.md).

## Features

- Game library with a scrolling 4 × 2 cover grid, Recent and Favorites views.
- ISO, BIN and CHD image identification and loading, with compatibility limitations.
- Local covers and automatic downloads for recognized PS1 and PS2 titles.
- Experimental physical PS2 DVD detection and reading.
- DualShock 4 navigation and emulated PlayStation controls.
- Vulkan graphics through the PS4 Mesa/RADV backend and native AudioOut output.
- In-game internal resolution, aspect ratio and performance overlay settings.
- Memory cards, shader caching, session logs and release notifications.
- Live PC telemetry with a browser dashboard and FTP tools.

Forja SX2's menus are in Spanish. BIOS files, games and user saves are not included. The PS4's drive reads DVDs only, so PS2 CD games can't be played or copied from disc, and disc swapping isn't supported. Save states are available (L3+R3); complete desktop PCSX2 feature parity is not.

## Installation and console setup

1. Install the PKG on a PS4 with a homebrew-capable environment that can run OpenOrbis applications.
2. Launch the application once to initialize its data directories.
3. Copy your BIOS and game images to the appropriate folders, using FTP or your preferred file transfer method.
4. Open **Library** and press **Triangle** to refresh the catalog and request missing covers.

```text
/data/PS4SX2/
├── bios/             Your PS2 BIOS files
├── roms/             Game images
├── covers/           Local and downloaded cover images
├── memcards/         Memory card data
├── cache/            Runtime caches
├── logs/             Emulator logs
└── telemetry.conf    Optional PC telemetry destination
```

For a local cover, use the same basename as the game: `roms/My Game.chd` → `covers/My Game.jpg`. PNG, JPG and JPEG images are accepted. Automatic downloads depend on identifying the game and finding a matching image online; not every title has a cover.

For a physical PS2 DVD, open **Disc**, wait for detection, and press **Cross** to start. **Triangle** refreshes the disc information. Disc detection and a downloaded cover do not guarantee that the game will run correctly.

## Controls

| Context | Control | Action |
|---|---|---|
| Library | D-pad / left stick | Navigate |
| Library | Cross | Start selected game |
| Library | Square | Add/remove favorite |
| Library | Triangle | Refresh library and missing covers |
| Library | Circle | Open exit confirmation |
| In game | OPTIONS | Emulated Start |
| In game | Touchpad click | Emulated Select |
| In game | L3 + R3 | Open emulator menu |
| Emulator menu | D-pad / left stick | Navigate and adjust settings |
| Emulator menu | Cross | Confirm |
| Emulator menu | Circle | Resume game |

The emulator menu includes **Resume game**, **Internal resolution**, **Aspect ratio**, **Performance overlay**, **Return to main menu**, and **Exit application**. Returning to the library closes the current game session.

Internal resolution ranges from **1× to 4×**. **3× and above are experimental and intended for PS4 Pro.** A higher rendering resolution does not improve emulation speed; start at 1× when diagnosing performance.

## Building

### Supported build workflow

The current workflow uses **Windows PowerShell + WSL2 Ubuntu-24.04**, with compilation in WSL and PKG creation on Windows. The active CMake target is `ps4/vm/CMakeLists.txt`; this source subset does not provide the desktop Qt build.

| Requirement | Current default / purpose |
|---|---|
| OpenOrbis PS4 Toolchain, Windows | `C:\OpenOrbis\PS4Toolchain`; PKG tools and packaging resources |
| OpenOrbis PS4 Toolchain, WSL | `/opt/pacbrew/ps4/openorbis`; PS4 headers, libraries and FSELF tools |
| Clang / Clang++ 21 | WSL compilation |
| CMake, Ninja, rsync, Python 3 | WSL build tools |
| PS4 libc++21 / libc++abi and ABI overlay | `/root/dolphinps4-build/sysroot` |
| PS4 Mesa/RADV static libraries | `/root/dolphinps4-build/build/mesa` |
| Windows CMake and Clang | Building the local dependency prefix |
| Internet connection | Fetching pinned build dependencies |

**A fresh clone is not a complete SDK installation.** OpenOrbis, the PS4 libc++ sysroot and PS4 Mesa/RADV artifacts must already be prepared. The two `dolphinps4-build` paths refer to compiled SDK dependencies in the current development environment. The selected runtime and toolchain integration are included in `ps4/runtime` and `ps4/toolchain`; the build does not require the DolphinPS4 source checkout. Desktop Mesa libraries are not substitutes for the PS4 build.

The WSL environment variables `OPENORBIS`, `PS4_SYSROOT` and `PS4_MESA_BUILD` can override these SDK paths. SDKs and compiled dependency prefixes are not committed to this repository.

### 1. Prepare local dependencies

Open PowerShell in the repository folder:

```powershell
Set-Location C:\Port\PS4SX2
.\ps4\deps\build-prereqs.ps1 -ToolchainRoot 'C:\OpenOrbis\PS4Toolchain' -Jobs 4
```

This downloads hash-pinned sources and builds the local dependency prefix in `ps4/deps/prefix`. It covers zlib, libpng, libjpeg-turbo, Zstandard, LZ4 and WebP. Zstandard is built with `ZSTD_TRACE=0` to avoid unsupported weak imports during FSELF generation. If this prefix is already prepared on the development machine, it can be reused.

### 2. Compile and package

```powershell
.\ps4\vm\build.ps1 -Toolchain 'C:\OpenOrbis\PS4Toolchain' -Distro Ubuntu-24.04 -Jobs 4 -Jit ON -Fastmem ON -MTVU ON
```

The script stages the sources in `/root/ps4sx2-src`, builds in `/root/ps4sx2-build`, creates the executable and validates the resulting PKG. Edit the repository files, not the WSL staging copy. GLSLang, Shaderc headers and RapidYAML are fetched at pinned revisions/hashes on first configuration.

Output:

```text
ps4/vm/build/package/IV0000-PCSX20007_00-PS4SX2EMU0000001.pkg
```

- Application: **PS4SX2 Emu**
- Title ID: **PCSX20007**
- Package version: **01.00**

To save build output:

```powershell
.\ps4\vm\build.ps1 -Toolchain 'C:\OpenOrbis\PS4Toolchain' -Distro Ubuntu-24.04 -Jobs 4 -Jit ON -Fastmem ON -MTVU ON *> build-output.log
```

Ready-to-use startup images and intro resources are included. Regenerating them with `ps4/vm/prepare-startup-assets.ps1` is optional and requires the original artwork/video and FFmpeg. It is not part of a normal build.

Successful compilation and PKG validation do not replace testing on the console.

### GitHub Actions

The workflow at [`.github/workflows/build.yml`](.github/workflows/build.yml) builds on **GitHub-hosted runners**:

1. **Ubuntu 24.04:** installs Clang/LLVM 21 from [LLVM's package repository](https://apt.llvm.org/), restores the PS4 SDK snapshot and compiles the current source with JIT, Fastmem and MTVU enabled.
2. **Windows Server 2022:** downloads that executable, packages it with the existing PowerShell script and runs the package validator.
3. The run publishes **PS4SX2-PKG**, executable/debug symbols and build logs as [Actions artifacts](https://github.com/actions/upload-artifact). It does not publish a release automatically.

**The published SDK is configured by default.** The workflow downloads [PS4SX2 SDK v2](https://github.com/Phoenixx1202/PS4SX2-SDK/releases/tag/sdk-v2) and verifies its pinned SHA-256 checksum. No repository variables are required for this snapshot. Application builds run entirely on GitHub; your PC can be switched off. The PS4 Mesa/RADV, libc++ ABI overlay and OpenOrbis dependencies are prebuilt SDK inputs, not rebuilt from upstream in this workflow. A stock desktop SDK cannot replace them.

#### Export a replacement SDK snapshot (maintainers)

From PowerShell in this checkout, with the working WSL dependencies and local dependency prefix prepared:

```powershell
wsl -d Ubuntu-24.04 -- python3 /mnt/c/Port/PS4SX2/tools/ci/export-sdk.py
```

The exporter requires Python 3.11 or newer and `llvm-ar-21`. It converts thin static libraries into standalone archives containing their object files, then produces:

```text
ps4/ci/build/ps4sx2-sdk-v2.tar.gz
ps4/ci/build/ps4sx2-sdk-v2.tar.gz.sha256
```

Both files are ignored by Git. The archive includes the selected Linux OpenOrbis tools and target libraries, PS4 libc++ headers/runtime, Mesa static archives, the local dependency prefix, Windows packaging tools/resources, available license notices and a per-file SHA-256 manifest. It does not read game images, BIOS folders, saves or application logs. Preserve the component licenses and their source-distribution obligations when hosting a dependency snapshot.

SDK v2 fixes the missing Mesa object files in SDK v1. Thin archives from the original Meson build refer to files outside the archive and cannot be copied alone to another machine. The importer rejects these archives before compilation; re-export from the complete original build tree if this check fails.

For another development environment, run `python3 tools/ci/export-sdk.py --help` in WSL and set `--openorbis`, `--sysroot`, `--mesa` and `--windows-sdk` as needed. The exporter refuses to overwrite an existing archive; use `--output /path/to/ps4sx2-sdk-v2.tar.gz` for a new snapshot.

#### Override the SDK (optional)

To use a different snapshot, upload its archive to your dependency hosting location (for example, a dedicated SDK repository release asset). Prefer a separate SDK repository so the application's release checker does not report an SDK asset as an emulator update. Then open **Settings → Secrets and variables → Actions → Variables** in the PS4SX2 repository and set both overrides:

| Repository variable | Value |
|---|---|
| `PS4_SDK_URL` | Direct public HTTPS download URL of `ps4sx2-sdk-v2.tar.gz`; not a release page |
| `PS4_SDK_SHA256` | The 64-character checksum from the `.sha256` file, without the filename |

The downloader verifies the whole archive before extracting any files. An invalid URL/checksum, a missing component or a checksum mismatch stops the job. Private URLs requiring authentication are not supported by this setup. Leave both variables unset to use the published SDK v2 defaults.

#### Run the build

Commit the workflow and helper scripts, then open **Actions → Build PS4SX2 → Run workflow**. It also runs on code pushes to `main`; documentation-only and PC-monitor-only changes are skipped. When the run finishes, download **PS4SX2-PKG-<run number>** from its **Artifacts** section. The ZIP contains the installable PKG, its checksum and build provenance. Packages are retained for 30 days; executable symbols and logs for 14 days.

If compilation fails, inspect the `compile-logs` artifact and the failed step. If packaging fails, inspect `package-logs`. Dependency download/setup errors appear directly in the step output. When the PS4 SDK or prebuilt libraries change, export a new archive and update **both** repository variables.

The [first successful hosted build using SDK v2](https://github.com/Phoenixx1202/PS4SX2/actions/runs/38490473175) compiled the executable and built and validated the PKG from commit `1cfb4a6`. This confirms the CI build path; console compatibility and performance still require hardware testing.

## Libraries and components

| Component | Role |
|---|---|
| PCSX2 core | CPU, memory, graphics, audio and device emulation |
| OpenOrbis | PS4 platform interfaces and packaging tools |
| Mesa/RADV and Vulkan headers | PS4 Vulkan rendering |
| GLSLang and Shaderc headers | Runtime shader compilation interface |
| Dear ImGui, stb image/font helpers | UI support, cover decoding and fonts |
| libpng, libjpeg-turbo, WebP | Image decoding/encoding |
| libchdr, LZMA, zlib, Zstandard, LZ4 | Disc image and compressed data handling |
| SoundTouch, FreeSurround, Speex resampler | Audio processing and resampling |
| fmt, fast_float, SimpleIni, RapidYAML | Formatting and configuration/data parsing |
| Xbyak, cpuinfo | x86 code generation and CPU support |
| Chaos Compiler Collection | Core symbol/debug information support |
| rcheevos headers | Core compatibility declarations; not an advertised achievements service |
| Python standard library | PC debug server, telemetry, HTTP and FTP |

Only the source/header subset required by this PS4 target is included for some dependencies. Their presence does not mean every upstream feature is enabled. Original licenses and notices remain alongside the dependency files.

## Live debugging on your PC

<img width="1913" height="992" alt="image" src="https://github.com/user-attachments/assets/df0906d2-1eaf-4fb2-a834-ad18a7aff32f" />


The debug tool displays console logs, application/VM timing, execution stages, EE throughput and connection status at **[http://localhost:8080/](http://localhost:8080/)**. It also provides FTP upload/download and diagnostic marker controls. It is a development monitor, not a remote CPU debugger with breakpoints.

### 1. Configure the PC tool

Install **Python 3.12**. The server uses the Python standard library; no pip packages are required.

```powershell
Set-Location C:\Port\PS4SX2
Copy-Item .\tools\ps4-live-monitor\config.example.json .\tools\ps4-live-monitor\config.json
notepad .\tools\ps4-live-monitor\config.json
```

If you already have a local `config.json`, edit it instead of replacing it. Set:

- `ps4.host`: the **PS4's LAN IPv4 address**.
- `ps4.port`: the PS4 FTP port, commonly `2121` with GoldHEN.
- `ps4.user` / `ps4.pass`: the credentials used by your FTP server.
- `web_host`: `127.0.0.1` for the browser dashboard on this PC.
- `web_port`: `8080`.
- `telemetry_port`: `19423`.

Enable the PS4 FTP server if you want file browsing, transfers and marker controls. FTP is separate from live telemetry: one connection can work while the other is disconnected.

### 2. Configure the PS4 telemetry client

Find your **PC's LAN IPv4 address** with `ipconfig`. Copy `tools/ps4-live-monitor/telemetry.conf.example`, rename it to `telemetry.conf`, and edit its contents. For example, if the PC is `192.168.1.10`:

```ini
host=192.168.1.10
port=19423
```

Upload that file to **`/data/PS4SX2/telemetry.conf`** using FTP. Keep `host` and `port` on separate lines. Do not use `localhost` or `127.0.0.1` here: on the PS4 those addresses refer to the console itself. Restart PS4SX2 after changing this file; the destination is read at application startup.

The current client has a development fallback address when the file is absent. Always create the file for your own network rather than relying on that fallback.

### 3. Start the server and open the dashboard

```powershell
py -3.12 .\tools\ps4-live-monitor\debug-server.py
```

Alternatively, use `python` if it points to your Python installation. Keep the terminal open, visit **http://localhost:8080/** in the PC browser, then launch PS4SX2 on the console. Stop the server with **Ctrl+C**. The dashboard source is `tools/ps4-live-monitor/index.html`.

| Connection | Direction | Default |
|---|---|---|
| Browser dashboard | PC browser → PC server | HTTP `127.0.0.1:8080` |
| Live telemetry | PS4 → PC server | TCP `19423` |
| File tools | PC server → PS4 FTP server | TCP `2121` |

Both devices must be able to reach each other on the local network. Allow Python/inbound TCP `19423` through the PC firewall on the private network if needed. The browser dashboard binds to localhost by default and has no login; keep the debugging service on your trusted development network.

### Using the dashboard

- **Execution:** application uptime, VM time, EE MC/s, program counters and stage transitions. EE MC/s is not game FPS.
- **Live logs:** filter messages, show stages only, clear the displayed list or export JSONL.
- **Diagnostic markers:** create/remove marker files through FTP. Some changes require restarting the game or application and can affect performance.
- **FTP files:** list a directory, upload a file or download a complete file path under `/data/PS4SX2`.
- **Alerts:** highlight potentially relevant log messages; an alert alone does not prove a crash.

PC sessions are saved in `tools/ps4-live-monitor/logs/session_*.jsonl`. Console logs remain in `/data/PS4SX2/logs`. Local configuration and captured logs are ignored by Git. For large game images, use a dedicated FTP client; the dashboard upload currently buffers the entire file in memory.

If telemetry stays disconnected, check the PC IP in `telemetry.conf`, the TCP port and firewall, then restart the application. If only FTP fails, check the PS4 IP, FTP service and credentials. If port 8080 is already occupied, stop the older debug server before starting this copy, or change `web_port` and open that port in your browser.

## Repository layout

```text
pcsx2/                  Selected emulator core sources
common/                 Shared utilities
3rdparty/               Required dependency sources/headers and licenses
bin/resources/          Game database and shaders
ps4/core/               PS4 platform implementation
ps4/compat/             Compatibility adapters
ps4/runtime/            PS4 runtime sources
ps4/toolchain/          WSL toolchain integration
ps4/cmake/              Windows dependency toolchain
ps4/deps/               Dependency preparation script
ps4/vm/                 Application, frontend, assets and build scripts
tools/ps4-live-monitor/ PC telemetry server and English dashboard
MD/                     Local development notes (ignored; not in a clone)
```

The public setup instructions are maintained in this README. Internal plans, migration notes and handoff documents belong in `MD/`, which is excluded from Git. Third-party license and attribution documents remain with their libraries.

## Credits

- **[PCSX2 Team and contributors](https://github.com/PCSX2/pcsx2)** — the emulator core, hardware emulation, renderers, game database and the years of work that make this project possible.
- **[Phoenixx1202 — PS4SX2](https://github.com/Phoenixx1202/PS4SX2)** — the PS4 port Forja SX2 is built on, including its game library, Vulkan and audio backends, and the [PS4SX2 SDK](https://github.com/Phoenixx1202/PS4SX2-SDK) used by the GitHub Actions build.
- **[swordpdf — PS5SX2](https://github.com/Swordpdf/PS5SX2)** — the PS5 port whose core optimizations (wait loops, GIF buffer, texture memory, instant DMA) are in this code, the AMD FSR 3 frame generation ported from it (`ps4/core/framegen`), and the reference for per-game profiles.
- **[iHaiDeeZ — PS4SX2](https://github.com/iHaiDeeZ/PS4SX2) and [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — the PS4 adaptation of PS5SX2 used here as a reference, and the Dolphin port behind ideas used here: per-game profiles, the GPU-queued flip and the in-app updater that downloads to `/data/pkg`.
- **[Al-Azif — ps-disc-detect](https://github.com/Al-Azif/ps-disc-detect)** — the reference implementation used for PS4 optical drive discovery and disc detection. The CAM discovery and sense-decoding work in this port was adapted from that project. Its dumper and Redump database are not bundled here.
- **Spyros** — earlier console-port work from which parts of this adaptation are derived.
- **DolphinPS4 and love-ps4 contributors** — PS4 runtime and toolchain integration used by this port.
- **OpenOrbis, Mesa, Khronos and all third-party library authors** — the platform, graphics and supporting components used by the application.

Forja SX2 and PS4SX2 are independent projects and are not affiliated with Sony or endorsed by the PCSX2 Team. Please keep PS4-port-specific reports with this project rather than treating them as confirmed upstream PCSX2 bugs.

## License

See [COPYING.GPLv3](COPYING.GPLv3) and the notices in individual source files. Third-party components retain their respective licenses, including the notices in `3rdparty/` and `ps4/LICENSE`.

Do not include BIOS files, game images, user saves, keys or SDK binaries when contributing source changes.
