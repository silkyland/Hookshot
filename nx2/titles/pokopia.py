"""Pokémon Pokopia (Switch 2, Koei Tecmo KTGL, SDK 20.5.25) — title 0400A3A01EE4C000.

Dump: v655360 (Ver 2.0.1), main build 76a489f3bd3642fa5e18bd83d32c98ba,
subsdk0 9646a69c..., sdk cc7da00d... (nxnative dm/games/0400A3A01EE4C000/game.tsv).

Memory (nxnative): Switch 2 gives games 9 GB of 12 GB — 256 MB front + 256 MB
alias + 256 MB stack + 9 GB heap; the NPDM declares no system resource, so no
virtual address memory. pad_layout=full (Nmouse matters).
"""
NAME = "pokopia"
BUILD_ID = "76a489f3"
PATCH_MODULE = None                                # no generated-C patches yet
SEED_SUBDIR = "pokopia"                            # <repo>/work/pokopia
PIPELINES_SEED = None                              # no recorded pipeline list yet
# host/ HLEs this title must NOT get: the real lifted SDK code has to run.
# nn::oe::Initialize is a no-op stub for OoT/MKW, but SDK 20's version sets up
# the nn::am applet interfaces (nn::oe::InitializeApplet -> nn::am::Initialize-
# ApplicationInterfaces); without it nn::am::GetApplicationFunctions returns
# null and nn::fs::GetSaveDataSize aborts (svcBreak on TaskMgr).
UNHLE = {"_ZN2nn2oe10InitializeEv"}
LANGUAGES = [("en-US", "English (US)"), ("en-GB", "English (UK)"), ("fr", "French (France)"),
             ("fr-CA", "French (Canada)"), ("de", "German"), ("it", "Italian"),
             ("es", "Spanish (Spain)"), ("es-419", "Spanish (Latin America)"), ("pt-BR", "Portuguese (Brazil)"),
             ("nl", "Dutch"), ("pl", "Polish"), ("ru", "Russian"), ("ja", "Japanese"), ("ko", "Korean"),
             ("zh-Hans", "Chinese (Simplified)"), ("zh-Hant", "Chinese (Traditional)")]
