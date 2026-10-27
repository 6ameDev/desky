# Phase 1 coverage helper (advisory only, used by [env:native-cov]).
# gcov-style --coverage instruments at compile time via build_flags, but the
# native link line does not inherit it, so restate it here for the link.
# Without this the link fails with undefined _llvm_gcda_emit_arcs (2026-10-27).
Import("env")
env.Append(LINKFLAGS=["--coverage"])
