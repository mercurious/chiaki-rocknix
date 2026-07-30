# Fork changelog (`master..rocknix`)

Upstream base: `420809b` (chiaki 2.1.1+, the final upstream master).
Newest first.

| commit | change | why |
|---|---|---|
| c02dc44 | trigger deadzone with rescale (default 10%, `trigger_deadzone` key) | Flip 2 triggers rest off zero after first pull (fuzz=0 flat=0 on the virtual pad) — streamed as permanent brake drag |
| 5bf34b4 | `scan` subcommand: broadcast discovery, tab-separated output | feeds the on-device pairing wizard; standby consoles answered via 1s re-sends |
| f519fb6 | PS5 haptics-to-rumble (ctrl 0x13 + controller-type BOND + haptics receiver/sink + PCM->rumble conversion, `haptics` key) | PS5 titles send zero classic rumble events; ported from chiaki-ng, gated on `enable_dualsense` so PS4 behavior is untouched |
| 0b9aaa0 | concise notify messages, console-initiated end = normal exit, rumble reserved for gameplay | gamepad-only devices have no keyboard for "press Enter" prompts; toasts are the legible surface |
| 8b212b2 | `CHIAKI_NOTIFY_CMD` hook fired on stream state changes | lets launchers surface toasts during the black reconnect gap |
| a64c74e | retry (6x2.5s) when console still holds the previous session | the console keeps the RP slot busy for seconds after a disconnect; also self-heals stale sessions left by hard kills |
| efa995e | flush stale SDL events between session restarts | the old session's teardown QUIT event instantly killed the next session |
| 0697dd6 | in-stream toggle chords (R1+L3 resolution, L1+R3 codec) with in-place session restart, SIGUSR1/2 mirrors | protocol pins the profile at negotiation; chord buttons suppressed from the console while held |
| c87003b | SIGINT/SIGTERM end the session cleanly | a hard kill leaves the console reporting "Remote Play in use" (0x80108b10) for minutes |
| 7420f7f | drop PRESENTVSYNC, coalesce frame events, Select+Start quit fallback, exit 2 on abnormal quit | vsynced Wayland presents block while occluded (deadlocked the decoder); InputPlumber may swallow Guide |
| 820d2ec | the `rocknix/` SDL2 frontend + ffmpegdecoder named-decoder/threading patch | upstream's CLI cannot stream and its GUI is Qt5-only |
