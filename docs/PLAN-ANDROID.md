
## 2026-11-08 bring-up on Xiaomi Pad 7 Pro (Snapdragon 8 Gen 2, Adreno 735, Android 16)

What works now:
- adb over WiFi (USB adb on this device is dead: services hang after authorize; wireless debugging pairing works)
- APK installs via `adb install`; payload sideloaded 25 GB to `files/hookshot/` via `run-as` tar stream
- app finds payload (fixed: `internalDataPath` already ends in `/files`; candidates also probe `/sdcard/Android/data/...` and `/sdcard/Hookshot` for the no-adb path)
- game's stderr mirrored to `files/hookshot/user/hookshot.log` (pull with run-as — stdout never reaches logcat)
- **guest memory reserved**: kernel caps user-specified VA at 39 bits (512 GB) and has no contiguous 512 GB hole; window moved to `-DA64_HOST_BASE=0x4000000000` (256 GB) + `-DHOS_GUEST_SPACE=0x1000000000` (64 GB)
- **game boots**: nninitStartup runs, heap allocated, **Vulkan inits on Adreno 735 + swapchain created**
- ucontext fibers implemented in asm (host/android/a64_ctx.S), affinity via sched_setaffinity, backtrace stub

Remaining blocker — the renderer:
- `VK_EXT_external_memory_host` absent on Adreno (as predicted G4): pools can't be imported → black screen
- Options: (a) mirror/staging fallback in gpu/vk/vk_memory.cpp — infra exists (MIRROR_ON, submit syncs, write-back) but is designed for a subset of GPU-writable pools inside the import model; needs "all pools mirrored" mode: pool's VkBuffer = device-local mirror, dirty pages uploaded at submit (A64_WRITE_TRACK write marks exist), GPU writes copied back to guest via staging+memcpy instead of imported-buffer copy. guest_pages table buffer needs mirroring too. Turnip-in-app is NOT a shortcut: no host-memory import on KGSL either.
- guest crash seen: `commit of 0x1aee00000 at 0x1000000000 failed (error 8)` then read fault — check whether hos_commit's commit path needs Android work (window is RW-reserved already; commit may be a no-op bookkeeping call that should never fail, or mprotect-based and failing on the reserved range)

Build cycle that works: edit → `make -C build-android hookshot_game` → llvm-strip --strip-all → jniLibs → gradle assembleDebug → adb install -r → am start → run-as cat files/hookshot/user/hookshot.log
