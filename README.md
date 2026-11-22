# Catwhale Pet

[English](README.md) | [简体中文](README_CN.md)

An animated, transparent desktop pet for Linux Wayland. Left-click the pet for an
interaction, drag it with the left mouse button to move the window, and right-click
for its menu. Transparent space around the character passes pointer input to the
desktop underneath.

Catwhale Pet uses C++20, Qt 6, and FFmpeg. See the
[desktop guide](docs/Desktop.md) for GNOME, KDE Plasma, Hyprland, and niri.

## Run

With Nix flakes enabled, run from this checkout:

```bash
nix run
```

To build an executable for desktop shortcuts or an autostart entry:

```bash
nix build
./result/bin/catwhale-pet
```

The package selects the native Wayland backend in a Wayland session. Media
reactions use `playerctl` and the player's local MPRIS interface.

## Features

- Transparent animations, mouse reactions, and day/night behavior.
- Skins and configurable animation clip loops.
- Media reactions, a local agent dock, and an on-demand hardware panel.
- Settings, tray controls where supported, and optional XDG autostart.
- Optional workspace following on niri.

Configuration details are in [docs/configuration.md](docs/configuration.md).

## Commands

Run these through desktop shortcuts or a terminal. A second invocation forwards
the command to the running instance.

| Command | Action |
| --- | --- |
| `catwhale-pet --toggle-menu` | Toggle the pet menu |
| `catwhale-pet --open-settings` | Open Settings |
| `catwhale-pet --toggle-pet` | Toggle visibility |
| `catwhale-pet --quit` | Quit |
| `catwhale-pet --version` | Print the version |

## Hardware acceleration

Rendering tries Vulkan, then OpenGL, then software. FFmpeg tries NVIDIA NVDEC,
then VA-API, then libvpx CPU decoding for VP8 and VP9. The hardware video path
decodes the WebM color and alpha streams separately and matches their frames by
timestamp.

Start a fresh instance with `--diag` to see the platform and selected backends.
Use `--decode=cpu` to compare software decoding, `--renderer=opengl` to try
OpenGL, or `--no-gpu` to request software rendering and decoding.

## Development

```bash
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/catwhale-pet
```

A non-Nix build needs CMake, a C++20 compiler, Qt 6.6 or later with Qt Quick,
Widgets, and a Wayland platform plugin, and FFmpeg development libraries
(`libavformat`, `libavcodec`, `libavutil`, and `libswscale`). The FFmpeg build
needs libvpx for transparent software decoding. NVDEC and VA-API also need
matching device drivers and corresponding FFmpeg support.

Source layout and playback details are in [docs/architecture.md](docs/architecture.md).

## License and credits

Catwhale Pet is licensed under the [MIT License](LICENSE). The character art,
animations, and original application concept come from
[timevisitor/catwhale-pet](https://github.com/timevisitor/catwhale-pet).

Qt and FFmpeg are build and runtime dependencies with their own licenses. See
[third-party notices](THIRD_PARTY_NOTICES.md) for licensing and redistribution details.
