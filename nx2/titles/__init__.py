"""Per-title configuration for the shared toolchain (nx2 + host/).

One entry per supported game build. Everything title-specific lives here or in
the title's module — never scattered as build-id checks in the pipeline.

Each title module provides:
    NAME          short id ("oot", "pokopia", ...)
    BUILD_ID      main-module NSO build-id prefix (hex) the tools accept
    PATCH_MODULE  generated-C patch module applied after --write (or None)
    SEED_SUBDIR   subdirectory of <repo>/work holding this title's lift seeds
                  ("" = <repo>/work itself, the historical OoT location)
    PIPELINES_SEED  path under <repo> of hookshot_pipelines_seed.bin, or None
    LANGUAGES     [(code, name)] for language.txt / nn::oe::GetDesiredLanguage
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))

from titles import oot, pokopia  # noqa: E402

TITLES = {t.NAME: t for t in (oot, pokopia)}


def known_builds():
    return ", ".join("%s (%s)" % (t.NAME, t.BUILD_ID) for t in TITLES.values())


def for_build_id(bid):
    """(name, module) for a main-module build id (full hex or prefix), or (None, None)."""
    for name, t in TITLES.items():
        if bid.startswith(t.BUILD_ID):
            return name, t
    return None, None


def for_exefs(exefs):
    """The title an exefs dir belongs to, from its main module's build id; fails closed."""
    main = os.path.join(exefs, "main")
    with open(main, "rb") as f:
        head = f.read(0x60)
    if head[:4] != b"NSO0":
        raise SystemExit("%s is not a game executable" % main)
    name, t = for_build_id(head[0x40:0x60].hex())
    if t is None:
        raise SystemExit("build %s is not one Hookshot supports (%s)" % (head[0x40:0x48].hex(), known_builds()))
    return name, t


def resolve(want, exefs, work=None):
    """The (name, module) to build: an explicit --title / HOOKSHOT_TITLE / <work>/title
    name wins; otherwise the exefs' build id decides. An unknown name or build fails."""
    name = want or os.environ.get("HOOKSHOT_TITLE")
    if not name and work:
        try:
            with open(os.path.join(work, "title")) as f:
                name = f.read().strip()
        except OSError:
            pass
    if name:
        t = TITLES.get(name)
        if t is None:
            raise SystemExit("unknown title %r (known: %s)" % (name, ", ".join(sorted(TITLES))))
        bid = ""
        main = os.path.join(exefs, "main")
        if os.path.exists(main):
            with open(main, "rb") as f:
                head = f.read(0x60)
            bid = head[0x40:0x60].hex() if head[:4] == b"NSO0" else ""
        if bid and not bid.startswith(t.BUILD_ID):
            raise SystemExit("--title %s but the dump is build %s (that title is %s)"
                             % (name, bid[:8], t.BUILD_ID))
        return name, t
    return for_exefs(exefs)
