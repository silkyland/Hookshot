#!/usr/bin/env python3
# HOOKSHOT_SHADER_STAMP for the Android build: nx2/build.py's translator hash (what shader caches are keyed
# on), so it tracks the sources instead of a hand-baked value that goes stale. Usage: shader_stamp.py <gpu/vk>
import hashlib
import os
import sys

vk = sys.argv[1]
stamp = hashlib.sha1()
translator = [os.path.join(d, f) for top in ("eden", "sirit", "compat")
              for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
for f in sorted(translator) + [os.path.join(vk, n) for n in ("vk_shader.cpp", "vk_state.h", "vk_api.h")]:
    with open(f, "rb") as src:
        stamp.update(os.path.relpath(f, vk).replace("\\", "/").encode()
                     + src.read().replace(bytes((13, 10)), bytes((10,))))
print("0x" + stamp.hexdigest()[:16] + "ull")
