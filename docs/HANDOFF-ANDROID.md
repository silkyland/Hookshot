# Android bring-up — session handoff (2026-11-08)

## Where we are

The game **boots on the tablet** (Xiaomi Pad 7 Pro, Snapdragon 8 Gen 2 / Adreno 735,
Android 16, ~12 GB RAM). APK installs, 25 GB payload sideloaded, guest memory reserved,
Vulkan + swapchain initialize. **Two remaining blockers:**

1. **Renderer**: `VK_EXT_external_memory_host` absent on Adreno → `memory_init()` fails →
   black screen. The fix = "all pools mirrored" mode in `gpu/vk/vk_memory.cpp` (detailed
   plan below — scout research is complete).
2. **Guest heap crash** (independent, smaller): `commit of 0x1aee00000 at 0x1000000000 failed`
   → read fault. Root cause identified: `win32_posix.c:21-22` hardcodes `GUEST_LO/HI` to the
   desktop base `0x100000000000`, but Android maps the window at `0x4000000000`; ALSO the
   guest heap base `HOS_HEAP_BASE = 0x1000000000` (hos.h:45) lands **exactly one-past-the-end**
   of the 64 GB window `[0x4000000000, 0x5000000000)` — commits mprotect unmapped memory and
   heap reads fault. Fix shape: make GUEST_LO/HI follow `A64_HOST_BASE`/`HOS_GUEST_SPACE` in
   **both** hookshot_game and hookshot_vk (vk has its own copy of win32_posix.c; can resolve
   at runtime via dlsym like `hos_posix_keys`), and either enlarge the window up to the
   39-bit VA cap (~470 GB usable, probe loop in hos_main.c:144 exists) or re-base
   `HOS_SPACE_END`/`HOS_NX2_REGION_*`/`HOS_HEAP_BASE` window-relative. Recommend enlarging
   window to ~384-448 GB at base 0x4000000000 — heap then fits naturally.

## Build & test cycle (works end-to-end)

```bash
cd /home/dit/oot-hookshot/build-android && make -j$(nproc) hookshot_game
NDKSTRIP=/home/dit/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
$NDKSTRIP --strip-all libhookshot_game.so -o ../android-app/app/src/main/jniLibs/arm64-v8a/libhookshot_game.so
cd ../android-app && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle assembleDebug --no-daemon -q
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell "am force-stop dev.hookshot.oot" && adb shell "am start -n dev.hookshot.oot/android.app.NativeActivity"
sleep 10 && adb shell "run-as dev.hookshot.oot sh -c 'tail -50 files/hookshot/user/hookshot.log'"
```

- adb over **WiFi** (`192.168.20.178:41001`, pairing via wireless-debugging code; USB dead on this device — services hang after authorize).
- Game logs go to `files/hookshot/user/hookshot.log` via run-as (stderr→file redirect in android_main.c; stdout unreachable).
- Payload already on device at `/data/user/0/dev.hookshot.oot/files/hookshot/` (image.bin + romfs + shaders + user/) — do NOT re-transfer.
- adb serial: `adb-ac9c8ddc-XoZW5M._adb-tls-connect._tcp` (or reconnect `adb connect 192.168.20.178:41001`).

## The implementation plan — "all pools mirrored, no import" (option a, smallest change)

Design (from scout B + A + C, verified): in no-import mode, `import_pool` creates a
**device-local VMA buffer instead of an imported one; it IS the pool's only GPU residence**
(`p.mirror` semantics, `mstate = MIRROR_ON`, `p.address` = its BDA, `synced_gen = 0` so
`mirror_sync` uploads every page on first bind). Reuses existing dirty-page generations,
capture machinery, write-back lists, page table. Edit points (all `gpu/vk/vk_memory.cpp`
unless noted):

1. `memory_init` (:304-322): don't bail when `!dev.external_memory_host`; set file-scope
   `mirrors_only` flag; `align=4096`; force `mirror_on()` true in this mode.
2. `import_pool` (:103-202): no-import branch — skip ext/import/commit code; use
   `create_device_buffer` (vk_vma.cpp:126) for `p.mirror`; `p.address` via
   `vkGetBufferDeviceAddress`; `mstate=MIRROR_ON`; `synced_gen=0`; `mirror_bytes += bytes`;
   `p.buffer`/`p.memory` stay null. Do NOT use `mirror_make`'s refusal paths (no fallback).
   On Adreno/UMA heaps are unified — `VMA_MEMORY_USAGE_AUTO` acceptable if DEVICE_LOCAL fails.
3. `gpu_buffer_bind` (:1777-1821): when `p->buffer == NULL`, every non-mirror fallback return
   must yield `p->mirror` (needed for `!mirror_upload` bindings outside recordings).
4. `mirror_sync` live branch (:1571-1604): when `!p.buffer`, replace `p.buffer→p.mirror`
   copies with memcpy guest→staging + `vkCmdCopyBuffer(staging→p.mirror)` into
   `upload_command_buffer()`. Reuse capture path unchanged (cap_bufs are host VMA already).
   Need staging: export vk_texture.cpp's `staging()` arena OR make a small dedicated WB/staging
   allocator in vk_memory.cpp (safer — arena lifetime vs drain timing).
5. `memory_end_submission` write-back (:2087-2140): when `!p.buffer`, record
   `p.mirror→staging` copies + pending `{pool, runs, staging_offset, mem_rec_value}` list;
   drain when `value <= mem_completed`: `memcpy(guest_base + p.lo + dst, staging + src)`.
   Give write-back staging its OWN ring/buffers (cap_free-style, :560-593) — texture arenas
   are reused once a value completes and could be overwritten before drain.
   Drain hooks: where `mem_completed` updates (:1127-1135), `recording_wait`,
   `wait_value`/`nvn_wait` semaphore waits, `invalidate` after wait_value, and
   `wait_writebacks_reading` (vk_replay.cpp:1975 — currently no-op, implement it).
6. `gpu_buffer` (:1609-1617): return `p->mirror` when `MIRROR_ON` (needed for GPU unswizzle
   vk_texture.cpp:715 and texel_buffer_view :2527).
7. `mirror_free`/`mirrors_give_back` (:1851-1888): in no-import mode free ONLY retired pools'
   mirrors — never evict live pools (their mirror is the only GPU copy); skip
   `guest_pages_set(p.lo,p.hi,p.address)` revert (:1856) since p.address==mirror BDA.
8. `pool_lookup`/`bg_drain` (:1282-1292): redundant-pool teardown → VMA `destroy_buffer`.
9. `mirror_inherit` helper: call retired-pool inheritance when a no-import pool is created
   absorbing retired pools (pool_lookup :1390-1393) — else GPU-only page-table data is lost
   on merges (the foliage bug pattern, comments :1499-1502).
10. `memory_pool_created` (:1623) gate `!dev.external_memory_host` — optional eager device-buffer
    creation; can stay gated.

### Already import-free (no change): texture uploads (CPU→staging→image), ASTC, shader binary
reads, driver bank/UIBO staging, gpu_write CPU fallback, texture readbacks,
ReadTexels/WriteTexels, guest_pages table buffer itself, all `host->gpu_read` paths.

### Known residual gaps: GPU writes via guest-page-table (unbound pointers) never reach guest
memory (same as today, now permanent); `wait_writebacks_reading` must actually drain;
texel_buffer_view caches by (gpu,fmt,size) — ensure buffer behind view stays synced.

## Scaffold decisions (locked by scouts)

- `BufferRef.address` has no consumers — only `.buffer`/`.offset`; BDA only needed via
  guest_pages table → mirror BDA suffices.
- Everything GPU-visible funnels through `gpu_buffer_bind` → a mirror satisfies all binding
  paths (descriptors, vertex/index, indirect args, copy/fill/update).
- Capture machinery + dirty-page gens (`A64_WRITE_TRACK`, 128 MB `a64_dirty[]`) already
  compiled into the Android build — works on aarch64 (pure C atomics, no x86-only path).
- `MkwVkHost` vtable complete on Android EXCEPT `hos_gpu_ptr` bounds-checks vs
  `hos_guest_size()` (nvdrv.c:243 — GPU addrs ≥64 GB fail; fine once window ≥ heap+),
  `gpu_read/write` lack bounds (latent), `guest_alloc` lands at `HOS_SPACE_END=0x8000000000`
  outside the window (unused by vk backend — leave).

## Reference implementation

`valentderah/totk-pc-recomp` `runtime/vk_memory.c` — "shadow storage"/mirror mode is a
working MIT reference for exactly this design (vk_memory.cpp's mirror code was ported from
an older version of that file; local stale clone at `/home/dit/Sites/Labs/totk-pc-recomp`,
fetch valentderah's current file for the diff).

## Don't-break notes

- Linux desktop build must stay working: gate all changes on `dev.external_memory_host`
  runtime flag, not #ifdef, so one binary does both modes.
- Do not delete build-android/, gen/, payload, or any artifacts (user rule).
- Commits go on the user's current branch (was `multi-title`); keep game/source dumps out.
