# Unofficial Stremio PS4 Port

A native Stremio client for jailbroken PlayStation 4 consoles, based on [`Sp9nky/unofficial-stremio-ps5-port`](https://github.com/Sp9nky/unofficial-stremio-ps5-port) and adapted to OpenOrbis/PacBrew.

> **Unofficial.** This project is not affiliated with, endorsed by, or supported by Stremio or Sony Interactive Entertainment. No Sony SDK, keys, firmware files, or proprietary Sony code are included.

## Current status

Tested on a physical PS4:

- installs and boots as a native `.pkg`;
- Stremio UI starts successfully;
- RmlUi and the bundled Noto fonts render correctly;
- DualShock 4 input works through `ScePad`;
- network initialization through `SceNet` / `SceNetCtl` works;
- libcurl HTTPS initializes with the bundled CA file;
- addons load at startup;
- persistent logs are written to `/data/stremio/log.txt`.

The PS4 build intentionally uses SDL's **software renderer**. OpenOrbis' `opengles2` SDL renderer produced white quads, broken glyphs and stray geometry with this RmlUi backend on real hardware; the software renderer fixed the UI.

### Still in progress

The full app currently reports `libSceVideodec2` unavailable on the tested console, so hardware video playback is still work in progress. Playback, torrent streaming, seeking, subtitles, audio tracks and the system keyboard need additional real-hardware validation before this port should be considered complete.

## Features

The application is based on the PS5 port and includes its native Stremio UI and core client functionality:

- Board, Discover, Library, Addons and Settings pages;
- Stremio account/login support;
- addon and catalog APIs;
- built-in torrent engine;
- direct HTTP/HTTPS stream support;
- FFmpeg player architecture;
- subtitles, audio track selection, seeking and resume logic;
- PS4-native DualShock 4 input;
- PS4 system keyboard integration;
- PS4 network/runtime layer;
- experimental PS4 `libSceVideodec2` H.264 backend.

## Requirements

- Jailbroken PS4 capable of installing/running homebrew PKGs.
- x86_64 Linux machine for building.
- Internet connection during the build.

The build script is tested on **CachyOS / Arch Linux**. It also contains setup support for Ubuntu/Debian.

## Install

After building, install:

```text
dist/Stremio-PS4-0.5.0.pkg
```

using the normal homebrew package installer on the PS4.

Application data and logs are stored under:

```text
/data/stremio/
```

The main runtime log is:

```text
/data/stremio/log.txt
```

## Controls

| Where | Button | Action |
|---|---|---|
| Everywhere | ✕ / ○ | select / back |
| Everywhere | △ / R3 / Options | search |
| Everywhere | L1 / R1 | previous / next page |
| Everywhere | L2 / R2 | page up / down |
| Everywhere | Touchpad | open / close menu |
| Everywhere | L3 | back to Board |
| Player | ✕ | pause / play |
| Player | D-pad left / right | seek 10 s |
| Player | L1 / R1 | seek 1 min |
| Player | □ / △ | subtitles / audio |
| Player | Options | track menu |
| Player | ○ | stop |

## Building

Builds run on Linux. From the repository root:

```bash
./build.sh
```

That's it.

The script automatically:

1. installs the host build dependencies;
2. installs OpenOrbis and the required PacBrew PS4 portlibs;
3. downloads the pinned upstream Stremio PS5 source;
4. applies the PS4-specific runtime and compatibility changes;
5. downloads/builds RmlUi with the OpenOrbis toolchain;
6. builds the PS4 ELF and FSELF;
7. packages the application as a PS4 `.pkg`;
8. writes the result to `dist/`.

Expected output:

```text
dist/Stremio-PS4-0.5.0.pkg
dist/SHA256SUMS
dist/build.log
dist/compiler.log
```

To remove generated files:

```bash
./build.sh clean
```

### Upstream revision

Builds are currently pinned to:

```text
Sp9nky/unofficial-stremio-ps5-port
89c0e6227cfb6549cc6e144a0a77fa852b8e316c
```

Pinning the source keeps the PS4 patches reproducible instead of silently applying them to an incompatible upstream revision.

## How it works

The PS4 port keeps the upstream C++17 application, RmlUi interface, FFmpeg player, networking logic and torrent engine. The build replaces/adds the platform-specific pieces needed by OpenOrbis:

- PS4 network initialization and net pool;
- DualShock 4 input through `ScePad`;
- CommonDialog / IME keyboard integration;
- PS4-safe process shutdown;
- `__cxa_thread_atexit_impl` for OpenOrbis libc++abi;
- OpenOrbis math/FFmpeg compatibility fixes;
- PS4 packaging and FSELF generation;
- experimental `libSceVideodec2` backend;
- SDL software rendering for the RmlUi interface.

A few host/toolchain compatibility fixes are handled by the build automatically, including Arch's `libxml2-legacy`, old PacBrew `libcurl.pc` packages, OpenSSL compatibility for `PkgTool.Core`, and removal of the unsupported linker `-pthread` flag while retaining `libpthread.a`.

## Project layout

```text
build.sh    one-command Linux build
src/        PS4-specific runtime/input/video source
native/     OpenOrbis build, packaging and compatibility layer
.github/    CI build workflow
```

The upstream application source is downloaded into `.build/` during compilation and is not committed as a second copy in this repository.

## Status and known limitations

- UI rendering is verified on real PS4 hardware using SDL software rendering.
- Hardware video decoding is not yet working in the full application on the tested console.
- The IME keyboard is initialized lazily because initializing CommonDialog during startup caused `CE-34878-0` on real hardware.
- This is an experimental homebrew port and should be expected to contain bugs.

## Credits and licenses

Licensed under the **GNU GPL v3.0**, matching the upstream project.

This port builds on the work in [`Sp9nky/unofficial-stremio-ps5-port`](https://github.com/Sp9nky/unofficial-stremio-ps5-port), OpenOrbis, PacBrew, SDL2, RmlUi, FFmpeg, libcurl, FreeType, FriBidi, WebP and the other components documented in [`THIRD_PARTY.md`](THIRD_PARTY.md).

See [`LICENSE`](LICENSE) for the full GPLv3 text.
