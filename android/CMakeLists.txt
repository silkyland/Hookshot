# (android) The Android build: the same sources nx2/build.py compiles for Linux, as two shared libraries for
# arm64-v8a — libhookshot_game.so (the lifted game + the host runtime + the app entry) and libhookshot_vk.so
# (the Vulkan back end, loaded by host/rx_backend.c). Configure with the NDK toolchain:
#
#   cmake -S android -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release
#
# HOOKSHOT_GEN: the lifted code folder (lift.py --standalone --write's gen; the builder's is
#   ~/.cache/hookshot-builder/sa/gen). HOOKSHOT_RT64: the generated runtime (sa/build/rt64).
cmake_minimum_required(VERSION 3.22)
project(hookshot-android C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(HOOKSHOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(GEN_DIR "$ENV{HOME}/.cache/hookshot-builder/sa/gen" CACHE PATH "lifted code (gen/)")
set(RT64_DIR "$ENV{HOME}/.cache/hookshot-builder/sa/build/rt64" CACHE PATH "generated a64_rt.c")
set(IMAGE_JSON "$ENV{HOME}/.cache/hookshot-builder/sa/image/image.json" CACHE FILEPATH "image.json (mkw_info.c)")

if(NOT EXISTS "${GEN_DIR}/code_000.c")
    message(FATAL_ERROR "no lifted code at ${GEN_DIR} — build the game once on Linux (nx2/make_game.py) or point HOOKSHOT_GEN at a gen/ folder")
endif()
if(NOT EXISTS "${RT64_DIR}/a64_rt.c")
    message(FATAL_ERROR "no generated runtime at ${RT64_DIR} — run nx2/build.py once (it writes rt64/)")
endif()

# ---- the lifted game: gen/*.c + the generated runtime + the Eden ABI shim + mkw_info.c -------------------------
file(GLOB GEN_C "${GEN_DIR}/*.c")
list(REMOVE_ITEM GEN_C "${GEN_DIR}/a64_fpx.c")   # (header-only helpers if present; harmless either way)

add_library(hookshot_game SHARED
    ${GEN_C}
    "${RT64_DIR}/a64_rt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/mkw_info.c"
)

# ---- the host runtime (host/*.c), Android's set ---------------------------------------------------------------
# Linux compiles every host/*.c except display.c, mf_av1.c, rx_nowgpu.c, pgo_rt.c; Android additionally drops
# pad_sdl.c (SDL gamepads: android_input.c reads the AInputQueue instead).
file(GLOB HOST_C "${HOOKSHOT_ROOT}/host/*.c")
list(FILTER HOST_C EXCLUDE REGEX ".*/(display|mf_av1|rx_nowgpu|pgo_rt|pad_sdl)\\.c$")
list(APPEND HOST_C
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/android_audio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/android_display.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/android_input.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/android_touch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/android_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/ffmpeg_stub.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/android_compat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../host/android/a64_ctx.S"
)
# the Win32-on-POSIX shim the host and the menu's text rasterizer share (vk side compiles its own copy)
list(APPEND HOST_C
    "${HOOKSHOT_ROOT}/host/posix/win32_posix.c"
    "${HOOKSHOT_ROOT}/host/posix/linux_stubs.c"
    "${HOOKSHOT_ROOT}/host/posix/prof.c"
)
# opus (nn::codec's Opus decoder): the same flags and sources as nx2/build.py
file(STRINGS "${HOOKSHOT_ROOT}/third_party/opus/sources.txt" OPUS_SRC REGEX ".*\\.c$")
set(OPUS_C "")
foreach(rel ${OPUS_SRC})
    list(APPEND OPUS_C "${HOOKSHOT_ROOT}/third_party/opus/${rel}")
endforeach()
list(APPEND HOST_C ${OPUS_C})
target_sources(hookshot_game PRIVATE ${HOST_C})

# ---- the Vulkan back end ---------------------------------------------------------------------------------------
file(GLOB_RECURSE VK_CPP
    "${HOOKSHOT_ROOT}/gpu/vk/eden/*.cpp"
    "${HOOKSHOT_ROOT}/gpu/vk/sirit/src/*.cpp"
    "${HOOKSHOT_ROOT}/gpu/vk/*.cpp"
)
list(APPEND VK_CPP
    "${HOOKSHOT_ROOT}/gpu/third_party/suyu/video_core/textures/decoders.cpp"
    "${HOOKSHOT_ROOT}/gpu/third_party/suyu/video_core/textures/astc.cpp"
)
# sirit's sources may include .c (the SPIRV-Tools C parts); sirit/src is C++ but check both
file(GLOB_RECURSE VK_C "${HOOKSHOT_ROOT}/gpu/vk/sirit/src/*.c")
file(GLOB_RECURSE VK_COMPAT_CPP "${HOOKSHOT_ROOT}/gpu/vk/compat/*.cpp")

add_library(hookshot_vk SHARED ${VK_CPP} ${VK_C} ${VK_COMPAT_CPP}
    "${HOOKSHOT_ROOT}/host/posix/win32_posix.c"
    "${HOOKSHOT_ROOT}/host/posix/gdi_posix.c"
)
set_source_files_properties("${HOOKSHOT_ROOT}/host/posix/win32_posix.c"
    PROPERTIES COMPILE_OPTIONS "-x;c" SKIP_LINTING TRUE)

# ---- include dirs (nx2/build.py's -I set, minus the x86 sysroot) -------------------------------------------------
set(FFMPEG_INCLUDE "${HOOKSHOT_ROOT}/build-linux/deps/ffmpeg/include" CACHE PATH "FFmpeg headers (compile-only; ffmpeg_stub.c provides the symbols)")
if(NOT EXISTS "${FFMPEG_INCLUDE}/libavcodec/avcodec.h")
    set(FFMPEG_INCLUDE "/usr/include" CACHE PATH "FFmpeg headers (compile-only; ffmpeg_stub.c provides the symbols)")
endif()

# find the CPM caches the Linux build already fetched (build-linux/deps/cpm/<pkg>/<ver>/include)
set(_CPM_ROOTS
    "${HOOKSHOT_ROOT}/build-linux/deps/cpm"
    "$ENV{HOME}/.cache/hookshot-builder/work/eg/.cache/cpm"
    "${HOOKSHOT_ROOT}/work/eg/.cache/cpm"
    "${HOOKSHOT_ROOT}/../work/eg/.cache/cpm")
foreach(pkg vulkanheaders spirv-headers vulkanmemoryallocator)
    set(${pkg}_INCLUDE "" CACHE PATH "")
    foreach(cand ${_CPM_ROOTS})
        file(GLOB _vers "${cand}/${pkg}/*/include")
        if(_vers)
            list(GET _vers 0 ${pkg}_INCLUDE)
            break()
        endif()
        if(EXISTS "${cand}/${pkg}/include")
            set(${pkg}_INCLUDE "${cand}/${pkg}/include")
            break()
        endif()
    endforeach()
endforeach()
set(VK_HEADERS "${vulkanheaders_INCLUDE}" CACHE PATH "Vulkan-Headers include dir")
set(SPIRV_HEADERS "${spirv-headers_INCLUDE}" CACHE PATH "SPIRV-Headers include dir")
set(VMA_HEADERS "${vulkanmemoryallocator_INCLUDE}" CACHE PATH "VulkanMemoryAllocator include dir (optional)")
message(STATUS "vk headers: ${VK_HEADERS}")
message(STATUS "spirv headers: ${SPIRV_HEADERS}")
message(STATUS "vma headers: ${VMA_HEADERS}")

# ---- flags -------------------------------------------------------------------------------------------------------
target_compile_options(hookshot_game PRIVATE
    -O1 -g0 -fno-strict-aliasing -ffunction-sections -fdata-sections
    -DA64_POLLING -DA64_WRITE_TRACK
    # Android ARM64 caps process VA at 39 bits (512 GB): the desktop's 512 GB window at 16 TB
    # can never map, so the guest window moves to 256 GB and shrinks to 64 GB — far above the
    # app's ~12 GB pool and clear of the ~470 GB region the kernel already uses.
    -DA64_HOST_BASE=0x4000000000ULL -DHOS_GUEST_SPACE=0x1000000000ULL
    -Wno-everything
)
target_include_directories(hookshot_game PRIVATE
    "${RT64_DIR}" "${GEN_DIR}" "${HOOKSHOT_ROOT}/third_party/a64-kit/rt" "${HOOKSHOT_ROOT}/nx2/rt"
    "${HOOKSHOT_ROOT}/host" "${HOOKSHOT_ROOT}/host/posix" "${HOOKSHOT_ROOT}/host/android"
    "${HOOKSHOT_ROOT}/third_party/opus/include" "${HOOKSHOT_ROOT}/third_party/opus/celt"
    "${HOOKSHOT_ROOT}/third_party/opus/silk" "${HOOKSHOT_ROOT}/third_party/opus/silk/float"
    "${HOOKSHOT_ROOT}/third_party/opus/src"
    "${HOOKSHOT_ROOT}/third_party/stb"
    "${FFMPEG_INCLUDE}"
)
# opus's temporary-allocation mode and export macros, as nx2/build.py's -D set
set_property(SOURCE ${OPUS_C} APPEND PROPERTY COMPILE_DEFINITIONS
    OPUS_BUILD USE_ALLOCA HAVE_LRINTF HAVE_LRINT)
# the NDK's native_app_glue headers (android_main.c, android_input.h)
if(DEFINED ANDROID_NDK)
    target_include_directories(hookshot_game PRIVATE "${ANDROID_NDK}/sources/android/native_app_glue")
endif()
# NDK's <vulkan/vulkan.h> for the vk side; the game side gets it transitively where needed
target_compile_definitions(hookshot_game PRIVATE HOOKSHOT_ANDROID=1 _GNU_SOURCE)

target_compile_options(hookshot_vk PRIVATE
    -O2 -g0 -DNDEBUG -fno-strict-aliasing -ffunction-sections -fdata-sections
    -Wno-everything
    -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2
    -DHOOKSHOT_SHADER_STAMP=0x371d8e4b39880116ull   # (nx2/build.py's translator hash; recompute if gpu/vk sources change)
)
target_include_directories(hookshot_vk PRIVATE
    "${HOOKSHOT_ROOT}/host/posix" "${HOOKSHOT_ROOT}/host" "${HOOKSHOT_ROOT}/gpu/vk"
    "${HOOKSHOT_ROOT}/gpu/vk/compat" "${HOOKSHOT_ROOT}/gpu/compat"
    "${HOOKSHOT_ROOT}/gpu/vk/eden"
    "${HOOKSHOT_ROOT}/gpu/third_party/suyu"
    "${HOOKSHOT_ROOT}/gpu/vk/sirit/include" "${HOOKSHOT_ROOT}/gpu/vk/sirit/src"
    "${VK_HEADERS}" "${SPIRV_HEADERS}" "${SPIRV_HEADERS}/spirv/unified1" "${VMA_HEADERS}"
    "${HOOKSHOT_ROOT}/third_party/stb"
)
target_compile_definitions(hookshot_vk PRIVATE HOOKSHOT_ANDROID=1 _GNU_SOURCE)
set_source_files_properties(${VK_C} ${VK_CPP} PROPERTIES LANGUAGE CXX)

# ---- link --------------------------------------------------------------------------------------------------------
target_link_libraries(hookshot_game PRIVATE log android aaudio vulkan atomic m dl)
# the vk lib reads the game's hos_posix_* state by name (win32_posix.c's dlsym(RTLD_DEFAULT, ...)): on Android the
# "executable" is libhookshot_game.so, loaded RTLD_LOCAL by NativeActivity — its symbols reach the global scope only
# as a DT_NEEDED of the vk lib, so link it (the dependency is already satisfied: the game lib is what dlopen'd it)
target_link_libraries(hookshot_vk PRIVATE log android vulkan dl m hookshot_game)
# the game's hos_posix_* symbols are read by name from the vk lib (rx_backend looks them up): export them
target_link_options(hookshot_game PRIVATE -Wl,--export-dynamic-symbol=hos_posix_\* -Wl,--gc-sections)
target_link_options(hookshot_vk PRIVATE -Wl,--gc-sections -Wl,-Bsymbolic)

# android_native_app_glue: the NativeActivity's main-thread loop (android_main.c calls into it)
set(NATIVE_APP_GLUE "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
if(EXISTS "${NATIVE_APP_GLUE}")
    target_sources(hookshot_game PRIVATE "${NATIVE_APP_GLUE}")
    set_source_files_properties("${NATIVE_APP_GLUE}" PROPERTIES LANGUAGE C)
endif()
