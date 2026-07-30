# On-device validation

Reference rig: Retroid Pocket Flip 2 (SM8250 / Adreno 650), ROCKNIX `next`
20260701 (sway/Wayland + EmulationStation), consoles: 2x PS5. All items
operator-verified on hardware, 2026-07-29/30.

| area | verified |
|---|---|
| streaming | 720p60 and 1080p60, h264 and h265, software decode; zero decoder backlog over sustained sessions |
| pairing | `regist` via CLI and via the ETK gamepad wizard; second console paired reusing the same PSN account |
| discovery | `scan` finds LAN consoles (ready + standby); cross-subnet console reached via manual IP |
| haptics | GT7 road-surface haptics felt in-game via the PS5 haptics stream (no classic rumble events exist to fall back on) |
| input | full pad map incl. L2/R2 analog; trigger rest-drift fixed by the 10% deadzone (measured L2 resting at 12/255 after first pull) |
| chords | R1+L3 / L1+R3 toggles reconnect in place (~8-10s incl. console slot release), config persists |
| session lifecycle | auto-wakeup from rest mode; console rest-mode during stream = clean exit; stale-session self-heal after hard kills |
| rendering | fullscreen under sway incl. occlusion cases; no vsync deadlock |
| hardware decode | `h264_v4l2m2m`/`hevc_v4l2m2m` on the Venus codec: decoder initializes, accepts packets, produces NO frames (ffmpeg 6.0 + kernel 7.0.11) — documented dead end, software decode is the path |

Known cosmetics: Senkusha ping test fails on some LANs (session falls back
and works); first video packet may log one "Invalid data" push before the
first keyframe.
