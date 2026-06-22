# Crowd Control fork (WarpWorld)

This repository is a fork of [cemu-project/Cemu](https://github.com/cemu-project/Cemu)

**Fork:** https://github.com/WarpWorld/Cemu

## Upstream base

|                 |                                                                                                                                    |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| **Upstream**    | https://github.com/cemu-project/Cemu                                                                                               |
| **Release tag** | `v2.6`                                                                                                                             |
| **Base commit** | [`a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98`](https://github.com/cemu-project/Cemu/commit/a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98) |

Crowd Control changes are applied on top of that commit on **`main`** in this fork.

## License (MPL-2.0)

Cemu is licensed under the [Mozilla Public License 2.0](LICENSE.txt). Modified and new
Cemu source files in this fork remain under MPL-2.0.

If you receive a binary built from this fork, you may obtain the corresponding source for
our Cemu changes from https://github.com/WarpWorld/Cemu.

## What changed

- **`src/Cafe/CrowdControl.{h,cpp}`** — localhost TCP server (default port 52225) for the
  Crowd Control wire protocol.
- **`src/input/emulated/{VPAD,WPAD}Controller.cpp`** — inverted controls / swapped buttons
  while a TCP client is connected.
- **`src/config/CemuConfig.{h,cpp}`** — `CrowdControlPort` setting (`Debug` section in
  `settings.xml`; `0` disables the server).
- **`src/gui/guiWrapper.cpp`** — window title shows bridge connection status.
- **`src/Cafe/CafeSystem.cpp`**, **`src/Cafe/CMakeLists.txt`** — init/shutdown wiring.
- **Renderer files** — minor hooks needed for the integration build.

## Build (Windows)

Requires Visual Studio 2022 with C++ CMake tools and Ninja.

Set `CrowdControlPort` in portable `settings.xml` if you need a non-default port. Boot a
title with the BOTW Crowd Control graphics pack enabled and connect with the `CemuTCP`
connector from the Crowd Control pack.
