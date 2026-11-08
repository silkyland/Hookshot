# Local patches on top of upstream

Pin: `4d7d2f1` (see UPSTREAM.md). Branch: `oot-linux`.

| File | Change | Why |
|---|---|---|
| `third_party/a64-kit/rt/a64_rt.c` | `a64_unimplemented` also zeroes `vreg[0]` | Generated stubs return `x0=0` only; imports returning `float`/`double` (e.g. `nn::audio::GetAudioDeviceOutputVolume`) left `s0`/`d0` holding a stale guest value |
