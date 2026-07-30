# chiaki-rocknix

**PS4/PS5 Remote Play for ROCKNIX handhelds** — a fork of [chiaki](https://git.sr.ht/~thestr4ng3r/chiaki)
by Florian Märkl, adding a controller-first SDL2 frontend built for embedded Linux
gaming devices (developed and validated on the Retroid Pocket Flip 2 / SM8250,
tuned as the Remote Play lane of the [ETK](https://github.com/mercurious/etk)).

This branch (`rocknix`) carries the fork; `master` is untouched upstream.

**Disclaimer:** This project is not endorsed or certified by Sony Interactive
Entertainment LLC.

## What the fork adds

- **`rocknix/` frontend** — one small binary (`chiaki`) with subcommands:
  `stream` (SDL2 Wayland client), `regist` (console pairing), `scan`
  (broadcast console discovery, parseable output), `discover`, `wakeup`, `list`.
- **PS5 haptics-to-rumble** — backported from
  [chiaki-ng](https://github.com/streetpea/chiaki-ng): the DualSense haptics
  audio stream is negotiated, decoded and converted to controller rumble
  (PS5 titles send no classic rumble events at all).
- **In-stream setting chords** — hold R1+L3 to toggle 1080p/720p, L1+R3 to
  toggle h265/h264. The Remote Play protocol pins the profile at session
  negotiation, so the client persists the config and reconnects in place.
- **Quality-of-life for handhelds** — auto-wakeup from rest mode, self-healing
  reconnect when the console still holds the previous session, clean
  SIGINT/SIGTERM session shutdown, trigger deadzone with rescale (rest-drift
  filter), notification hook (`CHIAKI_NOTIFY_CMD`) for on-device toasts,
  honest exit codes, Wayland-occlusion-safe rendering.
- **ffmpeg decoder patch** — full decoder names (e.g. `h264_v4l2m2m`) accepted
  in addition to hwaccel types; slice-threaded low-delay software decode.

## Controls (during a stream)

| Input | Action |
|---|---|
| Guide/Home tap | PS button |
| Guide/Home hold, or Select+Start hold | quit stream |
| R1+L3 hold | toggle resolution 1080p/720p |
| L1+R3 hold | toggle codec h265/h264 |
| Select | touchpad click |

## Config

Flat key=value file (default `/storage/.config/chiaki/chiaki.conf`, or
`--config PATH`; written by `regist`):

```
video_resolution = 1080p   # 360p/540p/720p/1080p
video_fps = 60             # 30/60
codec = h265               # h264/h265 (PS5 only)
decoder = software         # or an ffmpeg hwaccel/decoder name
audio_boost = 1.00
haptics = normal           # off/weak/normal/strong
trigger_deadzone = 0.10    # 0..0.4
```

## Installing

- **With the ETK** (recommended on ROCKNIX): `install.sh` deploys the binary,
  an EmulationStation Tools entry, and an on-device menu with a gamepad
  pairing wizard. See the [ETK repo](https://github.com/mercurious/etk).
- **Standalone, no ETK**: build it yourself and install into the ROCKNIX
  Ports system — see [STANDALONE.md](STANDALONE.md).

Build instructions: [BUILDING.md](BUILDING.md). Fork changelog:
[PATCHES.md](PATCHES.md). On-device validation: [VALIDATION.md](VALIDATION.md).

## License

AGPL-3.0-only (with OpenSSL exception), same as upstream — see
[COPYING](COPYING) and [LICENSES/](LICENSES/). Credit to Florian Märkl for
chiaki and to the chiaki-ng project for the haptics protocol work this fork
ports.
