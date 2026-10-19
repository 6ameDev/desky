"""gen_fw_manifest.py — build-time firmware sidecar (TRACKED build input).

Runs as a PlatformIO post-action on $BUILD_DIR/firmware.bin (firmware envs
only, never native-test). Reads HEAD_FW_VERSION from include/config.h (the
single source — the number is never duplicated here), hashes + sizes the
just-built firmware.bin, and writes firmware.manifest next to it in the exact
manifest wire format the core staging server serves:

    version=<u32>
    size=<u32>
    sha256=<64 lowercase hex>

Untracked scripts/ota_head_upload.py reads this sidecar to POST version-locked
uploads, then asserts the served manifest back — a staged version can never
differ from the built firmware. Fail LOUD (non-zero exit fails the build)
on: unparseable macro, missing firmware.bin, empty image, or size beyond the
OTA slot ceiling.

Doubles as its own test: `python3 gen_fw_manifest.py --selftest` exercises
the macro parse + manifest format on synthetic inputs (no hardware, no deps).
"""

import hashlib
import os
import re
import sys

MACRO_RE = re.compile(r"^\s*#\s*define\s+HEAD_FW_VERSION\s+(\d+)\s*$", re.MULTILINE)
MAX_U32 = 0xFFFFFFFF

# Must stay in sync with the OTA slot the head boots from (see
# partitions_ota_nospiffs.csv: 0x1F0000 per slot). The firmware's own
# ota::fitsSlot gate enforces the same ceiling on-device; this is the
# build-time tripwire so an oversized image fails HERE, not on the head.
SLOT_CEILING = 0x1F0000


def read_fw_version(config_path):
    """Strict u32 HEAD_FW_VERSION from config.h. Raises ValueError."""
    try:
        with open(config_path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        raise ValueError("cannot read %s: %s" % (config_path, exc))
    hits = MACRO_RE.findall(text)
    if len(hits) != 1:
        raise ValueError("want exactly one HEAD_FW_VERSION define in %s (found %d)" % (config_path, len(hits)))
    try:
        value = int(hits[0], 10)
    except ValueError:
        raise ValueError("non-decimal HEAD_FW_VERSION in %s" % config_path)
    if value > MAX_U32:
        raise ValueError("HEAD_FW_VERSION overflows u32 in %s" % config_path)
    return value


def build_manifest_text(version, size, sha_hex):
    """Exact wire format (mirrors otamanifest::buildManifest)."""
    if not isinstance(version, int) or not (0 <= version <= MAX_U32):
        raise ValueError("bad version %r" % (version,))
    if not isinstance(size, int) or size <= 0:
        raise ValueError("bad size %r" % (size,))
    if not isinstance(sha_hex, str) or len(sha_hex) != 64:
        raise ValueError("bad sha256 %r" % (sha_hex,))
    if any(c not in "0123456789abcdef" for c in sha_hex):
        raise ValueError("sha256 not lowercase hex")
    return "version=%d\nsize=%d\nsha256=%s\n" % (version, size, sha_hex)


def generate_manifest(config_path, firmware_path, out_path):
    version = read_fw_version(config_path)
    try:
        with open(firmware_path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        raise ValueError("cannot read %s: %s" % (firmware_path, exc))
    if not blob:
        raise ValueError("empty firmware image %s" % firmware_path)
    if len(blob) > SLOT_CEILING:
        raise ValueError("firmware %dB exceeds OTA slot %dB" % (len(blob), SLOT_CEILING))
    digest = hashlib.sha256(blob).hexdigest()
    text = build_manifest_text(version, len(blob), digest)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return version, len(blob), digest


def _scons_entry(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    try:
        version, size, digest = generate_manifest(
            os.path.join(project_dir, "include", "config.h"),
            os.path.join(build_dir, "firmware.bin"),
            os.path.join(build_dir, "firmware.manifest"),
        )
    except ValueError as exc:
        print("gen_fw_manifest: FATAL: %s" % exc)
        env.Exit(1)
        return
    print("gen_fw_manifest: version=%d size=%d sha256=%s..." % (version, size, digest[:12]))


def _selftest():
    fails = []

    def check(name, cond, detail=""):
        print("[selftest] %-40s %s %s" % (name, "OK" if cond else "FAIL", detail))
        if not cond:
            fails.append(name)

    import tempfile

    d = tempfile.mkdtemp()
    cfg = os.path.join(d, "config.h")
    with open(cfg, "w") as fh:
        fh.write("// x\n#define HEAD_FW_VERSION 3\n")
    check("macro parse", read_fw_version(cfg) == 3)
    with open(cfg, "w") as fh:
        fh.write("#define HEAD_FW_VERSION 3\n#define HEAD_FW_VERSION 4\n")
    try:
        read_fw_version(cfg)
        check("duplicate define rejects", False)
    except ValueError:
        check("duplicate define rejects", True)
    with open(cfg, "w") as fh:
        fh.write("#define HEAD_FW_VERSION 0x3\n")
    try:
        read_fw_version(cfg)
        check("non-decimal rejects", False)
    except ValueError:
        check("non-decimal rejects", True)
    with open(cfg, "w") as fh:
        fh.write("#define HEAD_FW_VERSION 3\n")
    fw = os.path.join(d, "firmware.bin")
    with open(fw, "wb") as fh:
        fh.write(b"\xe9" + bytes(1023))
    out = os.path.join(d, "firmware.manifest")
    v, s, h = generate_manifest(cfg, fw, out)
    body = open(out).read()
    check("generate ok", (v, s) == (3, 1024) and len(h) == 64)
    check("wire format", body == "version=3\nsize=1024\nsha256=%s\n" % h)
    check("sha matches content", h == hashlib.sha256(b"\xe9" + bytes(1023)).hexdigest())
    with open(fw, "wb") as fh:
        fh.write(b"")
    try:
        generate_manifest(cfg, fw, out)
        check("empty image rejects", False)
    except ValueError:
        check("empty image rejects", True)
    try:
        build_manifest_text(1, 10, "ZZ")
        check("bad sha rejects", False)
    except ValueError:
        check("bad sha rejects", True)
    print("[selftest] %s (%d failures)" % ("ALL PASS" if not fails else "FAILURES", len(fails)))
    return 0 if not fails else 1


try:
    Import("env")
except NameError:
    # Plain python (not SCons): only --selftest is meaningful.
    if __name__ == "__main__":
        if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
            sys.exit(_selftest())
        sys.exit(2)
else:
    # SCons: register the post-action on the firmware image.
    env.AddPostAction("$BUILD_DIR/firmware.bin", _scons_entry)
