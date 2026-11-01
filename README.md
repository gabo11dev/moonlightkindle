# Moonlight for Kindle

An experimental GTK2 Moonlight client for jailbroken Kindle e-readers. Stream a PC running [Sunshine](https://github.com/LizardByte/Sunshine) to an e-ink display, with touch mouse input and a basic on-screen keyboard.

## Made with AI

This project was created with AI assistance using OpenAI Codex. AI generated and modified project code, documentation, and build scripts. Third-party Moonlight and FBInk code remains the work of its respective authors; see [UPSTREAM.md](UPSTREAM.md). This is an unofficial, experimental project, not an official Moonlight, Sunshine, or Amazon product.

## Features and limitations

- GTK2 interface in English, landscape orientation, and full-screen Kindle window metadata.
- Software H.264 decoding with FFmpeg and grayscale display updates through FBInk.
- Pairing, application selection, streaming, touch mouse input, and a basic keyboard.
- Persistent client identity outside the extension folder, so replacing the application does not normally erase pairing keys.
- Video controls hide during streaming. Tap the top-right corner to show them, then use **Back to video** to hide them again.

The current build targets ARMv7 with NEON and the hard-float ABI, using the modern Kindle toolchain. A jailbreak, KUAL, and a working GTK2/X11 environment are required. Other hardware and firmware combinations are unverified. E-ink refresh and software decoding severely limit motion quality: begin with 640 × 360, 5 FPS, and a low bitrate. Screen presentation is capped at approximately 10 updates per second even if a higher stream rate is requested. Audio plays on the host only. Hardware decoding, HEVC, AV1, HDR, gamepads, and automatic PC discovery are not implemented.

## Install a complete package

A source checkout alone is not an installable KUAL package. Build and package the application as described below, including its private runtime.

1. Copy the complete `moonlight` directory into `/mnt/us/extensions/`, without adding an extra enclosing folder.
2. Safely eject the Kindle, open KUAL, and select **Moonlight for Kindle**.
3. Enter the PC's local network address and choose **Pair**. Enter the displayed PIN in Sunshine on the PC.
4. Choose **Load apps**, select an application, then choose **Start**.

Keep `/mnt/us/moonlight-data`: it contains the private pairing identity and settings. Do not publish this folder. Legacy keys from the extension's `data` folder are migrated when the persistent location has no identity yet.

If Sunshine reports `Certificate verification failed` after keys were replaced, remove the obsolete client entry in Sunshine and pair again with the current installation. Do not repeatedly delete the Kindle's identity folder. The exact cause of an individual authentication failure still requires the host and client logs.

If KUAL immediately returns to its menu, inspect `/mnt/us/extensions/moonlight/data/moonlight.log`. A missing `runtime/ld-linux-armhf.so.3` means the installation is incomplete: restore the entire packaged runtime.

## Get the source

```sh
git clone https://github.com/gabo11dev/moonlightkindle.git
cd moonlightkindle
```

CMake downloads the pinned Moonlight common C revision (including its ENet submodule) on the first configure. Git and network access are required for that first build. For offline builds, prepare `vendor/moonlight-common-c` at the revision listed in UPSTREAM.md, including ENet, beforehand.

## Build on a Linux desktop

On Debian or Ubuntu, install a C/C++ compiler, CMake, pkg-config, and the development packages for GTK2, FFmpeg (avcodec, avutil, swscale), libcurl, OpenSSL, Expat, and libuuid. For example:

```sh
sudo apt install build-essential cmake pkg-config libgtk2.0-dev libavcodec-dev libavutil-dev libswscale-dev libcurl4-openssl-dev libssl-dev libexpat1-dev uuid-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/moonlight-kindle --preview-kindle
```

The desktop preview does not rotate a physical display or use Kindle-specific screen updates.

## Cross-compile and package for Kindle

Supply an `arm-kindlehf-linux-gnueabihf` toolchain and a compatible ARM hard-float sysroot containing the same development dependencies. These are not bundled in this source repository.

```sh
export KINDLE_TOOLCHAIN=/absolute/path/to/arm-kindlehf-linux-gnueabihf
export KINDLE_SYSROOT=/absolute/path/to/arm-sysroot
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/kindlehf.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm -j2
```

Clone [FBInk](https://github.com/NiLuJe/FBInk) and check out revision `886f25f13368859ad8a899b88d04c26e19cda32e`, including its submodules. Build a shared FBInk library for the same target (see its upstream build instructions). Package that library and the executable with all runtime dependencies:

```sh
python3 tools/package.py --sysroot "$KINDLE_SYSROOT" --binary build-arm/moonlight-kindle --fbink /absolute/path/to/libfbink.so.1.0.0 --output package/extensions/moonlight
```

The packaging script expects a Debian-style ARM multiarch sysroot, including the dynamic loader and DejaVu Sans font. It follows ELF dependencies and creates a private runtime rather than replacing Kindle system libraries. The checked-in runtime manifests describe the previous experimental package's library and source provenance; they do not install those dependencies. Preserve applicable library notices and provide corresponding source when redistributing binaries.

## Validation

See [VALIDATION.md](VALIDATION.md) for available checks and hardware limitations. The protocol fixtures are local test hosts, not a replacement for testing against Sunshine and a real Kindle.

## License

GNU GPL version 3 or later. See [LICENSE](LICENSE) and [UPSTREAM.md](UPSTREAM.md) for third-party licenses and pinned revisions.
