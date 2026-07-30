# Standalone install on stock ROCKNIX (no ETK)

You can run chiaki-rocknix on a stock ROCKNIX device using the **Ports**
system — no ETK required. Ports live on persistent storage, so unlike the
ETK's Tools-menu integration this needs no boot services at all.

What you get standalone: the streaming client, launched from EmulationStation
-> Ports, with all in-stream features (chords, haptics, quit gestures).
Pairing is done once over ssh. (The on-device menu/pairing wizard is part of
the ETK integration, not this repo.)

## 1. Build

Follow [BUILDING.md](BUILDING.md) — you end up with `out/chiaki`.

## 2. Install onto the device

```sh
ssh root@<device> 'mkdir -p /storage/chiaki-rocknix /storage/roms/ports'
scp out/chiaki root@<device>:/storage/chiaki-rocknix/chiaki
ssh root@<device> 'chmod +x /storage/chiaki-rocknix/chiaki'
```

Create the port launcher `/storage/roms/ports/Chiaki.sh` on the device:

```sh
#!/bin/sh
# chiaki-rocknix standalone port launcher
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
export SDL_VIDEODRIVER=wayland
export SDL_VIDEO_WAYLAND_WMCLASS=chiaki
SWAYSOCK="$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -n 1)"
export SWAYSOCK

# coexistence: if the ETK is installed, its launcher owns chiaki - bail out
[ -x /storage/games-internal/roms/etk/tools/chiaki ] && exit 0

# tell etk-style daemons (if any) that a stream owns the R1+L3/L1+R3 chords
mkdir -p /dev/shm/etk_shm; touch /dev/shm/etk_shm/chiaki_active
trap 'rm -f /dev/shm/etk_shm/chiaki_active' EXIT INT TERM

# keep the stream fullscreen for its whole lifetime (sway tiles otherwise)
(
    while [ -f /dev/shm/etk_shm/chiaki_active ]; do
        swaymsg '[app_id="chiaki"] fullscreen enable' >/dev/null 2>&1
        sleep 2
    done
) &

/storage/chiaki-rocknix/chiaki stream > /storage/chiaki-rocknix/last.log 2>&1
exit 0
```

`chmod +x /storage/roms/ports/Chiaki.sh`, then refresh gamelists (or reboot).
The entry appears under Ports.

## 3. Pair (one-time, over ssh)

On the console: Settings > System > Remote Play > Pair Device (shows an
8-digit PIN). Your PSN account id in base64 comes from a lookup service such
as psntools.com/psn/checker (it is derived from your PSN name), or run
`scripts/psn-account-id.py` from this repo.

```sh
ssh root@<device> '/storage/chiaki-rocknix/chiaki regist \
    --host <console-ip> --pin <8-digit-pin> --account-id <base64-id>'
```

Find the console's IP with `/storage/chiaki-rocknix/chiaki scan -t 4` (run on
the device) or from the console's network settings.

That writes `/storage/.config/chiaki/chiaki.conf`; launch the port and it
streams. Settings (resolution/codec/haptics/deadzone) are keys in that file —
see [README.md](README.md). If you later install the ETK, it takes over
seamlessly: the pairing config is shared, and this port steps aside.

## Uninstall

```sh
ssh root@<device> 'rm -rf /storage/chiaki-rocknix /storage/roms/ports/Chiaki.sh'
# pairing config (keep if reinstalling): rm -rf /storage/.config/chiaki
```
