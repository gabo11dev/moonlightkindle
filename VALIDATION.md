# Validation

## Automated checks

After building with `BUILD_TESTING=ON` (the default):

```sh
ctest --test-dir build --output-on-failure
python3 tests/mock_host.py build/protocol-test
python3 tests/pair_host.py build/pair-test
```

The pairing fixture requires Python `cryptography` and `pyOpenSSL`. It exercises PIN pairing with RSA/AES operations, client certificate authentication, reconnection, replacement identities, and preservation of HTTP 409/503 errors rather than stale authentication errors.

The video test also accepts an H.264 elementary stream to exercise decoding and reconnect cleanup:

```sh
ffmpeg -f lavfi -i testsrc2=size=640x360:rate=5 -frames:v 20 -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -f h264 test.h264
./build/video-test test.h264
```

With an X server available, `./build/moonlight-kindle --smoke-test` creates and closes the interface. Use `--preview-kindle` for a desktop preview. The ARM executable and protocol tests can also run under qemu-arm with the matching private loader and library path.

## Scope

Earlier application versions were opened on a user's jailbroken Kindle. Pairing fixes were exercised with local HTTP/TLS fixtures, including an ARM build under emulation. Such checks do not establish compatibility with every Sunshine release, firmware, or Kindle model. Real-device streaming performance, orientation, suspend/resume, and the current pairing changes require testing on the target device. A full PC-to-Kindle session is not covered by these automated fixtures.
