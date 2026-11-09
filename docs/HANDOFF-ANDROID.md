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

## 2025-11-09 continuation — mirror fallback implemented, frames render, device fault ~8000 submits

DONE:
- Guest window: runtime ladder in hos_main.c reserve_guest — picks 80 GB at A64_HOST_BASE 0x4000000000 (256 GB budget).
  GUEST_LO/HI in win32_posix.c now follow A64_HOST_BASE + runtime size via hos_posix_guest_hi (dlsym-resolved).
  hos_mem.c regions clamp to window top; mkw_glue.c commit_on_touch uses hos_guest_size(). Commit failure + read fault GONE.
- All-pools-mirrored implemented in vk_memory.cpp gated on runtime `mirrors_only` (= !dev.external_memory_host,
  or RX_VK_MIRRORS_ONLY=1 to force on desktop): import_pool creates device-local mirror via create_device_buffer,
  mstate=MIRROR_ON, synced_epoch=0; first mirror_sync fills ALL pages (fill=synced_epoch==0) guest→guest_stage→mirror
  + mirror_inherit for retired pools (extracted helper). gpu_buffer_bind/gpu_buffer return p->mirror when no p->buffer;
  gpu_buffer syncs mirror when stale. Write-backs: !p.buffer pools record mirror→WbPending staging copies; drain on
  mem_completed via wb_drain_locked → memcpy to guest; memory_writebacks_pending/drain exported (vk_internal.h);
  hooks in vk_replay.cpp wait_value/recording_wait; wait_writebacks_reading implemented. mirror_free/mirrors_give_back
  never free in mirrors_only (mirrors are the only GPU residence + views/descs cache on them). bg_drain destroys
  redundant mirrors via destroy_buffer. guest_stage() exported from vk_texture.cpp (uses staging() arenas).
- guest_pages table window A widened 8GB→64GB (vk_memory.cpp windows[] + spirv_emit_context.cpp kGuestPagesBytes)
  + HOOKSHOT_SHADER_STAMP bumped. Covers heap+VAMM on 80-128GB windows.
- gen/a64_stubs.c in ~/.cache/hookshot-builder/sa/gen patched: all `imp_` stubs marked weak (stale gen vs new host HLE impls).
- RESULT: game runs ~50 fps, presents ~1200+ frames, renders real (but corrupted/mosaic) frames — title art visible.
- hookshot_env.txt on device at files/hookshot/hookshot_env.txt: NAME=VALUE lines, env for both libs.

OPEN ISSUE — deterministic VK_ERROR_DEVICE_LOST at ~submit 8000-9000 (~2.5-3 min, same content point every run,
preceded by ~300ms "slow submit" draw spike). Ruled OUT via A/B env tests (still crashed): pointer-follow (RX_VK_NO_FOLLOW),
stale mirror data (RX_VK_FILL_ALL re-uploads all pages every sync), retired-mirror reuse (mirrors never freed now),
4 suspect programs (RX_VK_SKIP_PROG), upscale stub path (RX_VK_NO_UPSCALE_STUB), OOM (2.6GB RSS), vkCmdUpdateBuffer
(RX_VK_TABLE_NOW — also runs are already ≤64KB chunks), parallel replay (RX_VK_PARALLEL=0), GPU unswizzle+B2T
(RX_VK_B2T_CPU + RX_VK_GPU_UNSWIZZLE=0). NOT yet ruled out: specific remaining programs, barrier patterns,
texel-buffer views, vkCmdWriteTimestamp/queries, nvn semaphore/event paths, sparse textures, driver bug on
something else in submissions. No VK_EXT_device_fault on Adreno; no root (dmesg empty).
NEXT: force mirrors_only on desktop via RX_VK_MIRRORS_ONLY=1 (build-linux exists, AppImage built) to reproduce
with real fault tooling (VK_EXT_device_fault works on desktop). Also: bisect all "in play" programs via
RX_VK_SKIP_PROG prefix list; check RX_VK_SHOTS/dump envs to see faulting frame; inspect mosaic (likely separate
issue — ASTC/BC decode or unswizzle params on Adreno).
Build commands unchanged (handoff above). libhookshot_vk.so ALSO goes to jniLibs arm64-v8a (strip it too).

## 2026-11-09 session 2 — desktop repro NEGATIVE; program fault ruled OUT; it's the fat submit

DONE (evidence):
- Desktop RTX 5070 Ti + `RX_VK_MIRRORS_ONLY=1` + `HOS_HEADLESS=1` + `RX_VK_SHOTS`: same mirrors-only path runs
  CLEAN — 50k+ presents through title→intro-movie→Kokiri flyover and beyond, zero device lost, frames perfect
  (no mosaic). Mirrors code itself is sound; the bug is Adreno-specific. Desktop build needs BOTH binaries
  fresh: vk .so pulls `rx_nvn_pushed`/`hos_posix_guest_hi` by dlsym — the exe must export them (it does via
  `--export-dynamic-symbol` since b7616b4/9a52925; an old exe + new .so fails `undefined symbol: rx_nvn_pushed`).
  Linux game run: `cd ~/Games/Hookshot && ./hookshot` with hookshot_env.txt beside it; headless shots work;
  `HOS_INPUT_LIVE=/tmp/x.txt` steers (append `0:A`); `HOS_INPUT_SCRIPT=25:A,...` mashes through menus.
- Tablet crash is UNATTENDED-reproducible: boot→title→attract movie, lost ~submit 8200-8400 every run
  (8233/8262/8324/8359/8366/8164 seen) — count drifts with content changes, so it's a content point, not a counter.
  env lives in files/hookshot/hookshot_env.txt (run-as to edit; no rebuild needed for env A/B).
- KILLED the program hypothesis: `RX_VK_SKIP_PROG=<all 23 "in play" programs + 53509044401d78d2>` still lost at
  ~8164 — collect "in play:" keys from the log (12 graphics + 11 compute this run). NOT a draw/dispatch fault:
  the bad work is in the submit's non-program commands (copies, barriers, uploads, timestamps, queue ops).
- The ~265-290ms "slow submit: draw ... commands ~60-74" spike precedes EVERY crash, incl. the all-skipped run
  → "draw" bucket ≠ vkCmdDraw; it's the whole draw() scope (program lookup, descriptors, gpu_buffer_bind→
  mirror_sync, vkCmd*). One fat submission = the attract scene's assets landing (big ASTC images + pool fills):
  NVN 0x87 → vk 157 = ASTC_4x4, sampled NATIVELY on Adreno (NVIDIA re-encodes big ASTC→BC3; different GPU work).
- Write-backs inert in this content: "0 pages the GPU wrote copied over" every report. Mesh draws already null
  on Adreno (no VK_EXT_mesh_shader → mesh_program_locked returns nullptr). Zero sparse binds in log. Robust ON.
- Desktop exe/vk build commands used (incremental objcache in ~/.cache/hookshot-builder + build-linux/vk-work):
    cd Hookshot
    EMSDK=$PWD/build-linux/deps/emsdk RX_CPM=$PWD/build-linux/deps/cpm \
      OOT_WORK=$PWD/build-linux/vk-work RX_VK_ONLY=1 RX_RELEASE=1 \
      build-linux/deps/python/bin/python3 -I nx2/build.py --standalone --jobs 24     # vk .so
    EMSDK=$PWD/build-linux/deps/emsdk RX_CPM=$PWD/build-linux/deps/cpm \
      HOOKSHOT_FFMPEG=$PWD/build-linux/AppDir/usr/lib/hookshot/ffmpeg \
      HOOKSHOT_SYSROOT=$PWD/build-linux/AppDir/usr/lib/hookshot/sysroot \
      HOOKSHOT_SDL2=$PWD/build-linux/AppDir/usr/lib/hookshot/libSDL2-2.0.so.0 \
      HOOKSHOT_VK_PREBUILT=$PWD/build-linux/vk-work/sa/build/hookshot_vk.so \
      OOT_WORK=$HOME/.cache/hookshot-builder \
      build-linux/deps/python/bin/python3 -I nx2/build.py --standalone --jobs 24     # exe (~9min: full chunks)
  then cp build result over ~/Games/Hookshot/hookshot + hookshot_vk.so (old ones saved as *.pre-mirrors).
- Desktop saves backed up: ~/.local/share/hookshot/save.bak-hk. A "New Game" was started on desktop (save dir
  may now contain a fresh file — the .bak has the originals).

OPEN — ranked suspects for the fat submit on Adreno:
1. mirror_sync staging→mirror per-page vkCmdCopyBuffer + image layout transitions landing in one submit
   (~120 pages/submit + big ASTC uploads): A/B RX_VK_MIRROR_DEFER, RX_VK_IMPORT_CHUNK_MB smaller, split upload cmd.
2. Per-submit timestamps (vkCmdWriteTimestamp2 ×2 + vkCmdResetQueryPool every submit; no env gate — add
   RX_VK_NO_TS in gpu_time_begin/end, rebuild libhookshot_vk.so for Android via build-android `make hookshot_vk`).
3. Big ASTC texture upload copies/extents on Adreno (same path as the mosaic).
4. sync_order machinery: RX_VK_SYNC_WAITS=0 (untested).
5. vkCmdUpdateBuffer guest-pages table (RX_VK_TABLE_NOW already tested — still crashed).
- To pin the exact failing command without checkpoint ext: RX_VK_DUMP=<present>+N at ~present 1100 (crash
  frame), or add a CPU-side submit-content ring logged at lost; or gate gpu_time_begin/end + mirror copies.
- A-mash run (HOS_INPUT_SCRIPT) pending at handoff: does the crash follow different content?
  RESULT: crashed ~submit 8513 anyway, same attract-sequence "in play" programs — the A presses either didn't
  leave the attract path or the intro cutscene carries the same fault. Content-point confirmed again.

IN-PROGRESS AT HANDOFF: tablet app left force-stopped (post-lost); hookshot_env.txt cleared on device. Desktop
soak stopped cleanly at ~50k+ presents, zero device lost. ~/Games/Hookshot hookshot_env.txt REMOVED (the game
folder is back to normal desktop mode; *.pre-mirrors backups still beside the binaries). User asked to power
off the PC after tablet runs — machine was shut down after this commit.
