# Linux desktop setup

Catwhale Pet runs as a native Wayland window on Linux. The desktop controls its initial position, stacking order, and workspace. To move the pet, press the left mouse button over the character and drag. Click the character for a reaction or right-click it for the pet menu. The transparent area around the character passes pointer input to the desktop underneath.

## Launch and controls

From a Wayland session, run `nix run` in the repository for a quick start. Use the installed `catwhale-pet` executable in desktop shortcuts and autostart entries. The Wayland application ID is `catwhale-pet` and the pet window title is **Catwhale Pet**. Both can be used in compositor rules.

| Command | Action |
| --- | --- |
| `catwhale-pet --show` | Show the pet |
| `catwhale-pet --toggle-pet` | Toggle visibility |
| `catwhale-pet --toggle-menu` | Toggle the pet menu |
| `catwhale-pet --open-settings` | Open settings |
| `catwhale-pet --quit` | Quit the running instance |

Run the same commands from a local terminal if your desktop has no system tray. To start the pet on login, enable **Start with the session** in Settings or use the desktop's autostart mechanism. For keyboard shortcuts and compositor configuration, use the executable's absolute path if it is outside the desktop session's `PATH`.

## GNOME

1. Sign in to a GNOME Wayland session and start the pet.
2. To keep the pet above ordinary windows, select the pet as the active window, open the GNOME window menu with **Alt+Space**, and choose **Always on Top** if available.
3. For quick access, add a custom shortcut under **Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts**. Use `catwhale-pet --open-settings` or `catwhale-pet --toggle-pet` as the command.

If you want the tray icon, GNOME Shell supports it through an AppIndicator extension. [GNOME shortcuts](https://help.gnome.org/gnome-help/keyboard-shortcuts-set.html) · [AppIndicator and KStatusNotifierItem Support](https://extensions.gnome.org/extension/615/appindicator-support/)

## KDE Plasma

1. Sign in to a Plasma Wayland session and start the pet.
2. With the pet selected, press **Alt+F3** and choose **Keep Above Others** from the window operations menu if you want it to stay in front. The tray icon can be accessed from Plasma's System Tray.
3. To remember the behavior across launches, open **System Settings → Window Management → Window Rules**. Use **Detect Window Properties** on the pet and match the **Catwhale Pet** title as well as the detected application class. Add **Keep above** and enable it, or choose a preferred output or virtual desktop. [Plasma window shortcuts](https://docs.kde.org/stable_kf6/en/khelpcenter/fundamentals/kbd.html) · [KWin rule editor](https://docs.kde.org/stable_kf6/en/kwin/kcontrol/windowspecific/kwin-rule-editor.html) · [KWin rule attributes](https://docs.kde.org/stable_kf6/en/kwin/kcontrol/windowspecific/attributes.html)

## Hyprland

On Hyprland 0.55 and later, add a Lua rule to float the pet and leave its transparent window free of desktop effects.

```lua
hl.window_rule({
    match = { class = "^catwhale-pet$", title = "^Catwhale Pet$" },
    float = true,
    no_blur = true,
    no_shadow = true,
    border_size = 0,
    rounding = 0,
})
```

Use the hyprlang form if your configuration targets Hyprland 0.54.

```ini
windowrule = match:class ^catwhale-pet$, match:title ^Catwhale Pet$, float on, no_blur on, no_shadow on, border_size 0, rounding 0
```

Use `hyprctl clients` to check the live window class and title. If you use a global window opacity or override rule, set the pet's opacity to `1.0 override 1.0 override` after it. [Current Hyprland window rules](https://wiki.hypr.land/configuring/core/rules/window-rules/) · [Hyprland 0.54 window rules](https://wiki.hypr.land/0.54.0/configuring/window-rules/)

## niri

Add this window rule after your global window appearance rules in `~/.config/niri/config.kdl`. It floats the pet at the bottom-right of its workspace and disables effects that would show through the transparent pixels.

```kdl
window-rule {
    match app-id="^catwhale-pet$" title="^Catwhale Pet$"
    open-floating true
    open-focused false
    default-floating-position x=24 y=24 relative-to="bottom-right"
    opacity 1.0
    geometry-corner-radius 0
    clip-to-geometry false
    draw-border-with-background false
    background-effect { blur false; }
    shadow { off; }
    focus-ring { off; }
    border { off; }
}
```

Use `niri validate` to check your configuration and restart the pet to apply the startup properties. On niri versions earlier than 26.04, omit the `background-effect` block. With **Follow the active workspace (niri)** enabled in Settings, the pet uses the compositor's session socket to follow the focused workspace. The pet floats above tiled windows, and another floating window can overlap it. [niri window rules](https://niri-wm.github.io/niri/Configuration%3A-Window-Rules.html) · [niri floating windows](https://niri-wm.github.io/niri/Floating-Windows.html)

Add these commands inside your existing `binds` block if you want shortcuts for the pet.

```kdl
Mod+Alt+C { spawn "catwhale-pet" "--toggle-menu"; }
Mod+Alt+S { spawn "catwhale-pet" "--open-settings"; }
Mod+Alt+H { spawn "catwhale-pet" "--toggle-pet"; }
```

## Graphics troubleshooting

- Start the pet from the same Wayland session as the compositor. The runtime environment needs the compositor's `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR`.
- If a blur, shadow, or border shows around the character, adjust the compositor's rules for the pet window. Check that the match uses the pet title and application ID.
- Rendering defaults to trying Vulkan, then OpenGL, then software. To test a particular backend, start a new instance with `--renderer=vulkan`, `--renderer=opengl`, or `--renderer=software`.
- Video decoding tries NVIDIA NVDEC first, then VA-API, then libvpx CPU decoding. Use `--decode=cpu` to test CPU decoding separately or `--no-gpu` to use software for both rendering and decoding.
- Use `catwhale-pet --diag` to enable startup diagnostics.

For NVIDIA on NixOS, the package looks for the installed driver libraries in `/run/opengl-driver/lib`. If decoding falls back to the CPU, check the reported backend, the driver, and VP9 support for your GPU. [NVIDIA video support matrix](https://developer.nvidia.com/video-encode-decode-support-matrix)
