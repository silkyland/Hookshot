# Android investigation: 2026-11-11

These are bounded observations from the connected Xiaomi Pad 7 Pro, not a claim that the stutter is fixed.
Reproduce observations before building a new theory on them. Earlier notes contain stale assumptions.

## Target and source

- Device: Xiaomi Pad 7 Pro, model `24091RPADG`, Qualcomm `SM8635` (Snapdragon 8s Gen 3), Android 16 / API 36.
- Firmware: `OS3.0.303.0.WOYMIXM` (HyperOS 3).
- USB ADB serial during this investigation: `ac9c8ddc`; rediscover with `adb devices -l`.
- Game package: `dev.hookshot.oot`, installed version `0.1.0-android`.
- Source base: `multi-title`, commit `fdf5875645306814845b4602c41fbbc8af8b6d72`.
- Checkpoint branch: `codex/android-cpu-topology`. It also contains the controller overlay WIP.
- The installed game APK was not replaced. Source fixes and the new controller UI have NOT been installed in it.

## What was observed

### Logging and rendering

App logcat capture of the main/system buffers contained approximately 160–166 lines per 45 seconds.
The game file log grew approximately 2.2–2.3 KB/s. The native entry redirects stderr to
`files/hookshot/user/hookshot.log`; it is important to distinguish this file from logcat.

A controlled launch with the file log temporarily redirected to `/dev/null` still stuttered. This tests
file-sink I/O, not formatting, stdio locks, flushing, or every Android log buffer. There is no evidence
of a massive app logcat flood in the sampled configuration, or that file writes dominate the stutter.

The actual game renderer reported `PurpleVK-public Adreno (TM) 735`, Vulkan 1.4. It is a hardware Vulkan
renderer. AV1 software decoding is a separate stage. GPU timestamp reports were often 3–7 ms/frame,
but those values do not account for every CPU/driver/queue wait.

The stock system Vulkan driver was also tried. It reported Adreno 735 / Vulkan 1.3, then produced
`device lost` during the scene. PurpleVK was restored. Do not silently switch the device back to stock.

### AV1 capability and actual movie files

An independent app enumerated MediaCodec and tried vendor codec names directly:

- Hardware AVC control: `c2.qti.avc.decoder`, `hardware=true`, 120/120 frames decoded.
- AV1 codecs exposed: `c2.android.av1-dav1d.decoder` and `c2.android.av1.decoder`, both software.
- `c2.qti.av1.decoder`, `c2.qti.av1.decoder.low_latency`, and `OMX.qcom.video.decoder.av1`
  each failed creation with `NAME_NOT_FOUND`.

This establishes current public MediaCodec availability on this firmware, not that the silicon lacks AV1.
Qualcomm's product brief advertises hardware AV1 for this SoC:
https://docs.qualcomm.com/doc/87-73942-1/87-73942-1_REV_A_Snapdragon_8s_Gen_3_Mobile_Platform_Product_Brief.pdf

The standalone app was then given actual game movie content, not just a generic sample:

| Content | Encoded video | Paced result | Unpaced decode/release throughput |
| --- | --- | --- | --- |
| `Dm_Title_0075.webm` | AV1, 1920x1080, 60 fps, 30 s | 1800/1800 output frames, about 59.9 fps steady | about 284 fps including startup |
| First 30 s of `Dm_Title_0005.webm` | AV1, 1920x1080, 60 fps; remuxed with `-c copy`, no re-encoding | 1800/1800 output frames, about 59.9 fps steady | about 141 fps including startup |

Both tests used Android's software dav1d decoder and a Surface. No outputs were submitted more than one
frame period late in the paced runs. Unpaced throughput is NOT panel FPS. Render callbacks are observed
snapshots, not proof of every distinct panel presentation. The standalone workload and platform decoder
are different from the game's bundled decoder, copies, locks, rendering and other concurrent work.

These measurements disprove the simple assumption that this device can never decode the movie in software
at 60 fps. They do not prove that software decoding has negligible cost inside the game.

### Confirmed CPU topology defect, fixed in source

`GetLogicalProcessorInformation` used `(physical_package_id, core_id)` to group logical CPUs. On this device,
all package IDs are zero, but core IDs repeat across clusters:

| CPU | Cluster | Core ID | Kernel thread siblings |
| --- | --- | --- | --- |
| 0 | 0 | 0 | 0 |
| 1 | 0 | 1 | 1 |
| 2 | 0 | 2 | 2 |
| 3 | 1 | 0 | 3 |
| 4 | 1 | 1 | 4 |
| 5 | 1 | 2 | 5 |
| 6 | 1 | 3 | 6 |
| 7 | 2 | 0 | 7 |

The old reader invented physical-core masks `0x89, 0x12, 0x24, 0x40` and falsely advertised SMT.
`hos_reserved_cpus()` then reserved the largest mask, `0x89`: three separate CPUs (0,3,7).
Actual `/proc/<pid>/task/*/status` confirmed guest and dav1d threads restricted to `1-2,4-6` (`0x76`).

The source fix reads `core_cpus_list`, falling back to its compatibility name `thread_siblings_list`.
It deduplicates kernel sibling masks and rejects missing, invalid or contradictory maps. Correct masks are
eight singletons; the existing reservation policy then reserves only CPU7 (`0x80`) and allows guest CPUs
0–6 (`0x7f`). Linux defines these maps at https://www.kernel.org/doc/html/latest/admin-guide/cputopology.html.

The real reader is compiled against fixture sysfs in `tests/test_cpu_topology.py`. The original code failed
the Xiaomi fixture (four groups instead of eight); the fix passes 13 cases, including SMT, ranges, bit63,
missing data and malformed maps. Independent review found no critical or important issue.

This is a correctness fix, NOT a demonstrated complete stutter fix. `HOS_RESERVE_CORE=0` allowed all eight
CPUs and did not cure stutter. `HOS_PIN=*=7f` reproduced corrected guest affinity, also without a reliable cure.
These experiments are not equivalent to installing the topology fix in both game and Vulkan libraries.

### Other probes and their limits

- An eight-second CPU-cycle sample attributed about 69.6% to `dav1d-worker` and 15.7% to
  `ModuleSystemWor`. These are CPU-cycle shares, not frame-time shares; some callchains failed.
- Worker stacks included `SleepThread -> sched_yield` for short waits. `HOS_SHORT_SLEEP_TIMER=1`
  reduced aggregate worker CPU in the measured interval, but did not establish a stable FPS cure.
- Disabling virtual cores, reducing escape delays, disabling catch-up, disabling screenshots, and removing
  verbose video logs did not establish a reliable cure. Do not keep these as magic performance settings.
- Frame-limiter instrumentation reported a target of 60. A simple 30-fps cap was not demonstrated.
- The first baseline read about 420 MiB from storage; later runs were often fully cached. Process restarts
  are NOT cold-storage tests. Baseline variability was large, and thermal/clocks were not controlled.
- A Perfetto trace had setup/interning errors; no quantitative critical-path conclusion is based on it.

## Known issues in the existing MediaCodec alternative

These were found in `host/android/android_media_av1.c` at the source base and are NOT fixed by this checkpoint:

1. `av1_codec_open(hw=1)` first creates `c2.android.av1.decoder`, a software component. It can prevent a
   vendor attempt and then incorrectly label the result hardware.
2. The MediaCodec branch obtains its input buffer but queues a length without copying `data` into `in`.
   Its logs print source bytes, not the queued bytes. Historical failures from that path do not establish
   decoder incapability.

The default bundled dav1d branch is separate and does copy its input. Do not enable `HOS_AV1_MC=1` and
assume its failure is evidence about hardware or the platform decoder's throughput.

## Device state and raw evidence

All experimental environment changes were restored. The original game was reopened using PurpleVK;
its package, payload and save data were preserved. The file log is a regular file again, not a symlink.

Original environment at the end of the investigation:

```text
RX_VK_ADRENO_DIR=/data/user/0/dev.hookshot.oot/files/hookshot/drivers
RX_VK_SHOTS=/data/user/0/dev.hookshot.oot/files/hookshot/shots
RX_VK_SHOT_EVERY=300
HOS_INPUT_BACKGROUND=1
HOS_INPUT_LIVE=/data/user/0/dev.hookshot.oot/files/hookshot/input_live.txt
HOS_VIDEO_LOG=1
```

Raw evidence remains locally on the Mac (not portable repo dependencies):

- `/Users/dex/Downloads/android-game-debug-20261110/`: trial scripts, samples, profiles, trace, `findings.json`, screenshots.
- `/Users/dex/Downloads/android-av1-probe-20261110/`: standalone codec probe source/APK, JSON results and private clips.
- Installed diagnostic app: `dev.codex.av1probe`; `run-as` exposes `files/inventory.json` and `files/last.json`.

Do not commit game payloads, movie files, APKs, signing keys or raw captures into this repository.

## Still unresolved

The dominant cause of the long game frames is not established. Next work should time the actual critical
path through decoding, copies, guest scheduling, GPU submission and presentation, using the same movie and
comparable warm-cache/thermal conditions. Keep observations, hypotheses and unverified conclusions separate.
