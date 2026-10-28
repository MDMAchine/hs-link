<p align="center">
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/link_logo_dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="docs/assets/link_logo_light.svg">
  <img src="docs/assets/link_logo_light.png" alt="HOT-Step LINK" width="316">
</picture>
</p>

# HOT-STEP LINK

A VST3/CLAP/AU plugin and Standalone application that connects a DAW (or desktop session) to a running local [HOT-Step](https://github.com/scragnog/HOT-Step-CPP) server. You type a caption, pick a mode, and the plugin sends a generation request over HTTP. Audio generation happens entirely in HOT-Step; the plugin handles host sync, capture, take playback, and drag-out.

**Status: v0.5.14 beta, pre-release.** Zips for Windows x64 and macOS arm64 are on the [Releases page](https://github.com/MDMAchine/hs-link/releases). Windows passes pluginval strictness 5; the Mac AU passes auval. Ableton Live was tested by hand; Reaper, Bitwig and Samplitude were not.

---

## Beta install

LINK makes no audio by itself: HOT-Step must be running and reachable (default `http://127.0.0.1:3001`). Get the zip for your platform from [Releases](https://github.com/MDMAchine/hs-link/releases) and check its SHA-256 against the value in the release notes or `SHA256SUMS` (Windows: `certutil -hashfile <zip> SHA256`; macOS: `shasum -a 256 <zip>`).

Download the zip, unzip, copy the plugin into your plugin folder (Windows: `C:\Program Files\Common Files\VST3\`; Mac: `~/Library/Audio/Plug-Ins/Components/`).

### Windows x64

1. Install the Microsoft Visual C++ 2015-2022 x64 runtime if needed: https://aka.ms/vs/17/release/vc_redist.x64.exe
2. Copy the `HOT-STEP LINK.vst3` folder to `C:\Program Files\Common Files\VST3\`.
3. Copy `HOT-STEP LINK.clap` to `C:\Program Files\Common Files\CLAP\`.
4. Optional: run `HOT-STEP LINK.exe` (Standalone) from anywhere.
5. Restart your host and rescan plugins. If LINK does not appear, check that the host scans `C:\Program Files\Common Files\VST3` (and `...\CLAP` for CLAP hosts).

If SmartScreen shows "Windows protected your PC", click **More info**, then **Run anyway**. If the zip is blocked, right-click it, **Properties**, tick **Unblock**, then unzip.

### macOS (Apple Silicon)

1. Copy `AU/HOT-STEP LINK.component` to `~/Library/Audio/Plug-Ins/Components/`, `VST3/HOT-STEP LINK.vst3` to `~/Library/Audio/Plug-Ins/VST3/`, `CLAP/HOT-STEP LINK.clap` to `~/Library/Audio/Plug-Ins/CLAP/`, and `HOT-STEP LINK.app` to `/Applications/`.
2. The Mac build is not notarized yet, so macOS may block it the first time; right-click > Open. If a plugin still says it "cannot be opened" or "is damaged", clear the quarantine flag in Terminal (the AU is the one Logic and GarageBand use):

   ```
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"HOT-STEP LINK.component"
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"HOT-STEP LINK.vst3"
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/"HOT-STEP LINK.clap"
   xattr -dr com.apple.quarantine /Applications/"HOT-STEP LINK.app"
   ```

   For the Standalone you can instead right-click `HOT-STEP LINK.app` in Finder, choose **Open**, then **Open** again in the dialog. Plugins cannot be opened that way, so use `xattr` for the `.component`, `.vst3` and `.clap`.

3. Quit and restart your DAW so it picks up the cleared files, then rescan. Logic Pro: Settings > Plug-in Manager, select LINK, **Reset & Rescan Selection**. Ableton Live: Settings > Plug-Ins, enable the system folders, **Rescan**.

### DAW setup

Insert LINK on an audio or instrument track, make sure the server field points at your HOT-Step server (default `http://127.0.0.1:3001`), and press Connect. See [Quick start](#quick-start) and [docs/MANUAL.md](docs/MANUAL.md).

### Reporting bugs

Open an [issue](https://github.com/MDMAchine/hs-link/issues) with: OS and version, host name and version, plugin format, the HOT-Step backend you used, steps to reproduce, and the exact error or status line. Screenshots help. For crashes, attach the crash dump or host log if one was written.

## Build from source

If you prefer, build it yourself with Visual Studio 2022, CMake 3.22+ and JUCE 8.0.12. See `docs/DEVELOPING.md` and the Build section below.

---

## Requirements

- HOT-Step running and reachable at `http://127.0.0.1:3001` (or any URL you configure).
- HOT-Step must have the `/api/v1/daw` facade enabled (present since the facade was added to the server; check `/api/v1/daw/status` returns `{ "api": 1 }` to confirm).
- For `lego` and `repaint` modes, HOT-Step must have a non-turbo base model available (`non_turbo_available` in the status response). Turbo-only setups can only use `text2music`.
- A C++ build toolchain: Visual Studio 2022 (MSVC) on Windows, or Clang/GCC on Linux and macOS.
- [JUCE 8.0.12](https://github.com/juce-framework/JUCE/releases/tag/8.0.12).
- CMake 3.22 or later.

---

## Build

### Windows (Visual Studio 2022)

```bat
git clone https://github.com/MDMAchine/HOTSTEP_LINK.git
cd HOTSTEP_LINK
build.bat
```

`build.bat` runs `cmake -B build -G "Visual Studio 17 2022" -A x64` then builds the Release configuration. The VST3 and Standalone artefacts land in `build\HOTSTEP_LINK_artefacts\Release\`.

`JUCE_DIR` defaults to the `JUCE_DIR` environment variable, else a `JUCE` folder beside this repo (`../JUCE`); if JUCE is elsewhere, pass `-DJUCE_DIR=...`. If no JUCE checkout is found at `JUCE_DIR`, CMake falls back to FetchContent and downloads JUCE 8.0.12 from GitHub (needs network on the first configure):

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=C:/path/to/JUCE
cmake --build build --config Release
```

### Linux / macOS

```sh
cmake -B build -DJUCE_DIR=/path/to/JUCE
cmake --build build --config Release
```

### Running the tests

```bat
build.bat test
```

Or after a manual build:

```
build\HSLINK_Tests_artefacts\Release\HSLINK_Tests.exe
```

The test binary covers host sync, capture ring, DawWorker offline detection, audition level-match and resampling, and stable take naming. See [docs/DEVELOPING.md](docs/DEVELOPING.md) for the teeth flags.

---

## Install

Windows x64 only in this section; Mac steps are in [Beta install](#beta-install). Download the zip, unzip, copy the plugin into your plugin folder (Windows: `C:\Program Files\Common Files\VST3\`; Mac: `~/Library/Audio/Plug-Ins/Components/`).

Download: **[release download link: TBD]** (`HOTSTEP_LINK_<version>_Windows_x64.zip`, with a `.sha256`)

Before you start, you need:

- HOT-Step v1.4.2 or newer, running on this machine or on your LAN. v1.4.2 is the first release with the `/api/v1/daw` API this plugin talks to. The plugin does not generate audio itself; it sends requests to HOT-Step.
- At least one HOT-Step backend set up: ACE-Step, SA3 (Stable Audio 3) or YuE2. The plugin offers whichever backends the server reports.
- HOT-Step with the DAW facade enabled: `http://127.0.0.1:3001/api/v1/daw/status` should return `{ "api": 1 }`.

### Manual install from the zip

Windows SmartScreen may show a blue "Windows protected your PC" box when you run the Standalone. To continue, click **More info**, then **Run anyway**.

The zip (`HOTSTEP_LINK_<version>_Windows_x64.zip`) holds the three built files, plus `INSTALL.txt`, `LICENSE` and `THIRD_PARTY_NOTICES.md`:

- `VST3\HOT-STEP LINK.vst3` (a folder; copy the whole folder, not just what is inside it)
- `CLAP\HOT-STEP LINK.clap`
- `Standalone\HOT-STEP LINK.exe`

Copy them like this (the two Common Files folders need administrator rights):

| File | Copy to |
|---|---|
| `HOT-STEP LINK.vst3` | `C:\Program Files\Common Files\VST3\` |
| `HOT-STEP LINK.clap` | `C:\Program Files\Common Files\CLAP\` (create the folder if it is missing) |
| `HOT-STEP LINK.exe` | anywhere you like; it is a single executable |

Windows may block files that came from a downloaded zip. If the host does not see the plugin, right-click the zip before extracting, open Properties, tick **Unblock**, then extract again.

### Rescan in your host

Close and reopen the host if it was open during the install, then rescan:

- Ableton Live: Settings, Plug-Ins, turn on "Use VST3 Plug-in System Folders", then Rescan.
- Reaper: Options, Preferences, Plug-ins, VST (or CLAP), then "Re-scan".
- Bitwig: Settings, Locations, Plug-ins; the system VST3 and CLAP folders are scanned by default, then rescan.
- Other hosts: add `C:\Program Files\Common Files\VST3` (or `...\CLAP`) to the scan paths if it is not there already, and rescan.

### Connect to HOT-Step

1. Start HOT-Step and wait until its web UI loads.
2. Load HOT-STEP LINK on a track, or run the Standalone.
3. The server URL defaults to `http://127.0.0.1:3001`. When the plugin can reach the server, the status bar turns green within a few seconds.
4. If HOT-Step runs on another machine, enter that machine's address (for example `http://192.168.1.20:3001`) and set a shared key; see [Network and security](#network-and-security).

### From source (developers)

The build does not copy the plugin anywhere (`COPY_PLUGIN_AFTER_BUILD` is `FALSE`). Artefacts land in `build\HOTSTEP_LINK_artefacts\Release\` under `VST3\`, `CLAP\` and `Standalone\`. `tools\deploy.ps1` copies them to a plugin folder you own (`-Dest "E:\MyPlugins"`, `-DryRun` to preview); it refuses Program Files on purpose and never deletes.

On Linux and macOS, build from source and copy the `HOT-STEP LINK.vst3` bundle to `~/.vst3/` (Linux) or `~/Library/Audio/Plug-Ins/VST3/` (macOS), then rescan. These platforms have not been tested by hand.

---

## Quick start

1. Launch HOT-Step and confirm it is ready (the web UI loads).
2. Load the HOT-STEP LINK plugin on any track (or launch the Standalone).
3. The server URL defaults to `http://127.0.0.1:3001`. If HOT-Step is running, the status bar turns green within a few seconds.
4. Type a caption describing what you want.
5. Select a mode: `text2music`, `repaint`, or `lego`.
6. Set the backend and duration, then click Generate.
7. Takes appear in the grid as they complete. Click a take to audition it, then drag it to your timeline or click Export WAV.

---

## Network and security

HOT-Step binds on `0.0.0.0` with CORS wide open. The DAW facade accepts any origin by default. On a typical setup the plugin and server are on the same machine, so only loopback traffic is involved.

If you run HOT-Step on a different machine on your LAN, set a shared secret in HOT-Step's `config.yaml` (`HOTSTEP_DAW_KEY`) and enter the same value in the plugin's Key field. The plugin sends the key as `X-DAW-Key` on every request. Requests arriving from a non-loopback address without the correct key are rejected.

The key is stored per user in your application data folder (`HOT-Step/LINK.settings`), not in the project or preset file. This means the key does not travel with the session when you share or back up project files.

---

## License

AGPLv3. See [LICENSE](LICENSE).

The plugin communicates with HOT-Step over HTTP only. No HOT-Step source or binary is linked into the plugin. HOT-Step itself is GPL v3; the two licenses coexist because the only coupling is a network API call.

---

## Known issues

- See the SmartScreen and Mac first-run steps above. No Linux binaries.
- Mac: AU passes auval but has not been run in a DAW; audition playback on Mac is untested.
- Some sandboxed hosts (Logic Pro, some Bitwig setups) refuse file drops from a plugin window. Use Export WAV and import from disk.
- Multi-take jobs show only the requested seed, not a seed per take.
- Backend switch is untested against a live server. If you see "Could not switch to", press Generate again.
- Recovered takes may appear twice when switching between pre-0.5.14 and 0.5.14 builds on the same takes folder.
- ACE renders can come out slightly shorter than requested (a 10 s request gave 9.44 s). This is engine-side, not the plugin.

The full list for testers is in the [v0.5.14 release notes](https://github.com/MDMAchine/hs-link/releases/tag/v0.5.14).

---

## Unverified

The following items have not been verified in a real DAW:

- **Reaper and Bitwig have not been tested by hand.** Ableton Live was tested by hand on 2026-10-07. Basic host operations in Reaper and Bitwig (scan and load, state save and restore, host transport sync, drag to the timeline) are untested.
- Drag to timeline in hosts other than the mock (R6 uses `performExternalDragDropOfFiles`; not tested in Reaper, Live, or Bitwig).
- Sandboxed hosts (Logic Pro entitlement requirements, Bitwig sandbox). Network calls from inside a sandbox may be blocked.
- The level-match audition on macOS (WinINet timeout behavior is Windows-specific; the macOS NSURLSession path differs but has not been tested).
- YuE2 in batch mode: in the bench run the batch-4 path produced an OOM when SA3 was resident at the same time. Not reproduced under normal plugin startup conditions but possible with a warm keep-loaded server.
- **Backend switch, live server.** The backend switch (Generate with Backend set to a specific value) has not been checked against a live HOT-Step server. The switch timeout (10 s against a silent server) and the cancel of a switch in flight are not covered by tests.

Tested a host? Open an issue with what worked and what did not.
