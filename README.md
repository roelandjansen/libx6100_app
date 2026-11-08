# libx6100_app

Remote client for radios behind a wfview server, written in C with an LVGL user interface. First target: the Xiegu X6100 via wfserver.

Status: planning and early development. Nothing usable yet.

## Features (planned)

- Connect to a wfview server over UDP (hostname or IPv4, control port, username, password)
- RX audio: LPCM, µ-law, ADPCM and Opus; mono rigs played on both channels
- Spectrum, waterfall and meters
- Basic controls: frequency, step, mode, filter, VFO, AGC, preamp, attenuator, scope span, frequency lock
- TX: push-to-talk, microphone audio, ATU tune, PTT time-out
- Rig definitions taken from wfview `.rig` files, selected by the model ID the radio announces

## Targets

| Target | Platform |
|---|---|
| `native` | Linux desktop (SDL), used for development |
| `android-arm64` | Android 10 (API 29) and later |
| `x6100` | Xiegu X6100 (ARMv7, glibc) |

## Building

Requirements: GNU make, a C11 compiler, SDL2 development files (native), Android NDK and SDK build-tools (Android), zig (X6100).

```
make TARGET=native
make TARGET=x6100
make TARGET=android-arm64 apk
make TARGET=native test
make DEBUG=1 TARGET=native
```

Toolchain paths are read from the environment or a local `config.mk`. Run `tools/toolchain-check.sh` to see what is missing.

## References

- [wfview](https://github.com/wf-group/wfview): protocol, timing and rig file format
- [libx6100](https://github.com/roelandjansen/libx6100): X6100 support, `contrib/wfview/X6100.rig`

## Licence

GPL-3.0. See `LICENSE`.

Third-party code keeps its own licence and copyright notices:

- LVGL: MIT
- Opus: BSD
- adpcm-xq: BSD

