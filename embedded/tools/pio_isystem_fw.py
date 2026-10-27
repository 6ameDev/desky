# Treat ESP32-Arduino framework headers as system headers.
# Moves every PlatformIO CPPPATH entry under a framework-arduino* package dir
# from -I to -isystem so -Wconversion/-Wdouble-promotion in framework headers
# (e.g. WString.h) stay silent while our src/ keeps strict -Werror.
# Must run as `post:`: a `pre:` script sees an empty CPPPATH because the
# platform appends framework paths during its own setup, which finishes before
# post scripts run but after pre scripts. Package dirs resolve via
# PioPlatform().get_package_dir — no hardcoded ~/.platformio version paths.
# Applied to both `env` (framework/lib objects) and `projenv` (our src/
# objects): they are separate SCons environments, patching only one leaves
# the other noisy. SCons CCFLAGS covers both C and C++ compiles, so one
# Append per env suffices.
Import("env")
try:
    Import("projenv")
    _envs = (env, projenv)
except Exception:
    _envs = (env,)


def _is_framework(path):
    return "framework-arduino" in str(path)


for _e in _envs:
    _cpppath = _e.get("CPPPATH", [])
    _moved = [p for p in _cpppath if _is_framework(p)]
    if _moved:
        _e.Replace(CPPPATH=[p for p in _cpppath if not _is_framework(p)])
        _flags = []
        for _p in _moved:
            _flags += ["-isystem", str(_p)]
        _e.Append(CCFLAGS=_flags)
