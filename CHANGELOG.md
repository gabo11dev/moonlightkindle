# Changelog

## 0.1

First public release. This is the latest development build, previously distributed privately as 0.3.2, renumbered to 0.1 for public releases.

- Moonlight name in KUAL and the application window.
- English GTK2 interface, landscape orientation, and full-screen video.
- H.264 streaming from Sunshine with touch mouse input and a basic keyboard.
- Stream rates from 1 to 30 FPS, saved between sessions.
- Preset and custom resolutions, with even dimensions up to 1920 × 1080.
- Refresh screen uses `eips -c` followed by an immediate redraw of the interface and last frame. FBInk remains a fallback if the command fails.
- Faster presentation checks and direct grayscale conversion.
- Persistent pairing identity outside the replaceable extension directory.

Experimental software. Native and emulated ARM tests passed; physical e-ink refresh behavior and performance still need device validation. Created with AI assistance using OpenAI Codex.
