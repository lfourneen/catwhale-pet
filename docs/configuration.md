# Configuration

All runtime files are under XDG paths; on a normal desktop that is
`~/.config/catwhale-pet/`.

| File | Written by | Contents |
| --- | --- | --- |
| `settings.json` | Settings / menu | `media.show` and `niri.followWorkspace` |
| `pet-state.json` | Settings / menu | pet size, speed, and ground shadow |
| `dock.json` | Agent board settings | `enabled`, `port`, `keepDoneSec`, and `muted` |
| `animation-loop.json` | Settings | fixed-clip loop spans |
| `director.json` | optional user file | director overrides |
| `skins/active.json` + `skins/<slug>/` | external skin packs | active skin |

## settings.json

```json
{
  "media": {
    "show": {
      "enabled": false,
      "pollMs": 2000,
      "startMs": 1200,
      "stopMs": 2500
    }
  },
  "niri": {
    "followWorkspace": true
  }
}
```

`media.show.enabled` starts the `playerctl` poller. `pollMs` must be at least
500. `startMs` / `stopMs` are the debounce windows used by the media page.
`niri.followWorkspace` makes the pet move to the focused workspace when running under niri.

## Skins

A skin is a directory under `~/.config/catwhale-pet/skins/<slug>/` with a
`skin.json` and video files. `active.json` selects the current slug:

```json
{ "slug": "default" }
```

The Settings panel has **Open skin folder**. Skin actions override the corresponding built-in clips. The app watches the
active skin manifest for changes. Missing actions use their built-in clips.

## Agent board

The board listens on `127.0.0.1:8770` by default. It accepts `POST /state`
with an agent event, and can also tail known session logs. Change the port in
`dock.json`; the server binds to loopback and treats events as data.

## CLI

```text
--toggle-menu       open/close the pet menu
--open-menu         open the pet menu
--open-settings     open Settings
--toggle-settings   toggle Settings
--toggle-pet        hide/show the pet
--show              show the pet
--quit              quit
--autostart=on|off  write/remove the XDG autostart entry
--version
```

A second process forwards the command to the running instance through
the session-local command socket.
