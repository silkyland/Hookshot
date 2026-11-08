# Local patches on top of upstream

Pin: `4d7d2f1` (see UPSTREAM.md). Branch: `oot-linux`.

| File | Change | Why |
|---|---|---|
| `third_party/a64-kit/rt/a64_rt.c` | `a64_unimplemented` also zeroes `vreg[0]` | Generated stubs return `x0=0` only; imports returning `float`/`double` (e.g. `nn::audio::GetAudioDeviceOutputVolume`) left `s0`/`d0` holding a stale guest value |
| `host/nn_vi.c` | HLE `QueryDisplayCurrentRefreshRate` (returns a failure Result so the game uses its own 60 Hz default instead of reading unwritten out-params) and `SetDisplay120HzRefreshRateCapability` (no-op) | Both were generic stubs: the query returned success without writing `RefreshRate`/`bool` out-params (garbage reads) |
| `host/nn_hid.c` | HLE `SetGyroscopeZeroDriftMode` no-op | Generic stub; six-axis sensor is already stubbed to rest pose, drift mode is unused |
