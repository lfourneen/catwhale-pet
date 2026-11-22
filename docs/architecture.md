# Architecture

Catwhale Pet runs as one Qt application. The pet is a translucent Wayland
top-level window with a Qt Quick scene graph. Qt Widgets provide the controls
and status panels.

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Startup, renderer selection, and single-instance commands |
| `src/controller.cpp` | Animation priorities, desktop behavior, and settings |
| `src/pet_window.cpp` | Transparent surface, pointer input region, and system drag |
| `src/panels.cpp` | Menu, Settings, tray, agent dock, and hardware UI |
| `src/video.cpp` | FFmpeg decoding, timing, and frame delivery |
| `src/config.cpp` | XDG paths, atomic JSON writes, and skin metadata |
| `src/services.cpp` | MPRIS and hardware reporting |
| `src/agent_dock.cpp` | Local HTTP event listener and session log summaries |
| `src/niri.cpp` | Optional workspace following through niri IPC |
| `assets/` | Clip manifest, director defaults, artwork, and desktop entry |
| `tests/` | Native decode and application startup checks |

## Window and input

The pointer hit region comes from the displayed frame's alpha and includes any
visible agent badge. Qt sends the region to Wayland so the compositor can
route clicks on transparent areas to surfaces underneath. The pet uses the
compositor's interactive move when the left-button drag crosses the toolkit's
drag threshold. The compositor manages focus, initial placement, and stacking.

The Wayland move grab can consume the release event. In that case, pointer
re-entry or a short timeout returns the animation to its usual state. This
does not end the compositor's move operation. See [Desktop.md](Desktop.md)
for compositor configuration.

## Playback

A worker decodes one animation at a time and passes the latest frame through
a bounded mailbox to the GUI. The first frame can come from a poster.
Qt Quick uploads the image as a texture and composites it with a cached
ground shadow. Hiding the pet stops decoding.

FFmpeg prefers CUDA/NVDEC on NVIDIA and then VA-API when there is a usable
render device. VP9 WebM transparency is stored as a separate encoded stream.
The hardware path pairs color and alpha by presentation timestamp, reads back
both surfaces, and combines them for the Qt texture. Failed device, format,
decode, or transfer operations advance to the next backend. VP8 and VP9 use
libvpx in the CPU path.

Vulkan, OpenGL, and software are renderer choices independent of the decoder.
The startup log reports the renderer and the decoder used for frames.

## Local services

`playerctl` supplies media metadata only while enabled or on an explicit probe.
The agent dock listens on loopback and summarizes bounded portions of recent
Codex and Claude Code JSONL logs. The hardware panel reads Linux CPU, memory,
storage, and GPU information when opened. Workspace following connects directly
to the niri socket when available.
