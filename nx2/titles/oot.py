"""The Legend of Zelda: Ocarina of Time (Switch 2) — the original Hookshot target."""
NAME = "oot"
BUILD_ID = "e0f3ccfa"
PATCH_MODULE = "oot_patches"
SEED_SUBDIR = ""                                   # <repo>/work itself (the historical seed location)
PIPELINES_SEED = "hookshot_pipelines_seed.bin"     # <repo>/<file>
LANGUAGES = [("en-US", "English (US)"), ("en-GB", "English (UK)"), ("fr", "French (France)"),
             ("fr-CA", "French (Canada)"), ("de", "German"), ("it", "Italian"),
             ("es", "Spanish (Spain)"), ("es-419", "Spanish (Latin America)"), ("pt-BR", "Portuguese (Brazil)"),
             ("nl", "Dutch"), ("pl", "Polish"), ("ru", "Russian"), ("ja", "Japanese"), ("ko", "Korean"),
             ("zh-Hans", "Chinese (Simplified)"), ("zh-Hant", "Chinese (Traditional)")]
UNHLE = set()                                      # every host/ HLE applies
