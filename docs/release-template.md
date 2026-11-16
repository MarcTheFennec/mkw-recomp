{summary}

## Changes

{changes}

## Tested before publishing

{tested}

## Install

- Linux x86-64, glibc 2.31 or newer (Debian 11, Ubuntu 20.04, SteamOS and later): `chmod +x` the AppImage and run it.
  The game it builds lives in `~/.local/share/mkw-recomp`. On 64-bit ARM (ARMv8.2: Snapdragon 8 Gen 1 and newer), the
  `-aarch64.AppImage`.
- Windows 10/11 x64: unpack the zip into a folder and run `mkw.exe`.
- Either: a Vulkan 1.3 GPU and about 30 GB free. Open **Game files**, point it at your own dump of Mario Kart World
  1.0.0 or 1.8.0 (its `exefs/` and `romfs/`, or one `.nup` or `.nx` file) and press **Set up**. Nothing of the game ships here: the setup builds it on your
  machine and downloads NVIDIA's `nvdisasm` and `ptxas` (pinned by sha256; 84 MB on Linux, 126 MB on Windows) to
  describe the shader encoding.

## Updating

From 0.1.5 on, the start screen offers a newer release (**Update to vX**) and installs it: it downloads it, checks it,
replaces the program and sets up again only what changed. From an older version, once by hand: Linux, run the new
AppImage; Windows, unpack the new zip over the same folder; then press **Set up** again (the steps already done are
skipped).

## Known issues

{known}

## Reporting a problem

Open an issue with the bug form and attach the logs: `logs/mkw.log` (the game and the start screen) and
`logs/setup.log` (the setup's tools), in `~/.local/share/mkw-recomp` on Linux and next to `mkw.exe` on Windows.
