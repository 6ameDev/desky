#!/usr/bin/env python3
"""Kover delta coverage: line coverage of lines this change adds/modifies.

Whole-codebase Kover percentages punish legacy code and invite gaming, so
this report scopes coverage to added/modified lines only. The printed floor
(informational floor 80% for new code -- not enforced) is a nudge, not a
gate: this script always exits 0. Gating waits for triaged delta data.

Diff range semantics:
  CI:  GITHUB_BASE_REF is set (pull_request context), so the range is
       origin/<base>...HEAD (three-dot: changes since merge-base). The
       script best-effort runs `git fetch origin <base>` first because the
       default checkout is shallow (depth 1); the workflow also sets
       fetch-depth 0 on the job that runs this script. If the fetch fails
       (offline), the local range is attempted before giving up with N/A.
  Local: `git diff HEAD` (staged + unstaged vs HEAD). When the tree is
       clean that diff is empty, so it falls back to `git diff HEAD~1 HEAD`
       (last commit). The chosen mode is printed in the report header.
  Override: --range '<git diff args>' (e.g. --range 'HEAD~3 HEAD') skips
       detection; useful for spot-checking the math by hand.

Mapping: each added line (+ hunks, -U0) in *.kt under shared/src, excluding
build/ output and generated `*Impl` / `*_Impl` Room/KSP files, is looked up
in the :shared:koverXmlReport XML (JaCoCo format: package > sourcefile >
line; a line counts covered when missed-instructions == 0). Added lines
with no XML entry (blanks, comments, imports) are non-executable and left
out of the denominator; they are not "uncovered".

Tooling: python3 stdlib only (xml.etree + subprocess). No installs.
"""
import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

INFO_FLOOR_PCT = 80.0
UNCOVERED_CAP = 30

CLIENT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_XML = os.path.join(
    CLIENT_DIR, "shared", "build", "reports", "kover", "report.xml")
DEFAULT_OUT = os.path.join(CLIENT_DIR, "build", "kover-delta", "delta.txt")


def run_git(args):
    p = subprocess.run(
        ["git"] + args, cwd=CLIENT_DIR,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout, p.stderr


def resolve_range(override):
    if override:
        return override.split(), "explicit --range '%s'" % override
    base = os.environ.get("GITHUB_BASE_REF", "").strip()
    if base:
        ref = "origin/%s" % base
        run_git(["fetch", "origin", base, "--depth=50"])
        code, out, _ = run_git(
            ["merge-base", ref, "HEAD"])
        if code == 0 and out.strip():
            return ([out.strip(), "HEAD"],
                    "CI range since merge-base with %s" % ref)
        return (["%s...HEAD" % ref],
                "CI range %s...HEAD (merge-base lookup failed)" % ref)
    code, out, _ = run_git(["diff", "HEAD", "--name-only", "--", "*.kt"])
    if code == 0 and out.strip():
        return (["HEAD"],
                "local uncommitted diff (staged + unstaged vs HEAD)")
    return (["HEAD~1", "HEAD"],
            "local fallback last-commit diff (tree clean vs HEAD)")


def is_tracked_source(path):
    # git prints repo-root-relative paths (client/shared/...) even when run
    # from client/; normalize to client-relative first.
    if path.startswith("client/"):
        path = path[len("client/"):]
    elif "shared/src/" in path:
        path = path[path.index("shared/src/"):]
    if not path.endswith(".kt"):
        return False, path
    if not path.startswith("shared/src/"):
        return False, path
    if "/build/" in path or path.startswith("shared/build/"):
        return False, path
    stem = path.rsplit("/", 1)[-1][:-len(".kt")]
    if stem.endswith("Impl") or stem.endswith("_Impl"):
        return False, path
    return True, path


def added_lines(diff_text):
    current = None
    new_line = 0
    per_file = {}
    for raw in diff_text.splitlines():
        if raw.startswith("+++ "):
            path = raw[4:].strip()
            if path.startswith("b/"):
                path = path[2:]
            ok, path = is_tracked_source(path)
            current = path if ok else None
            if current is not None:
                per_file.setdefault(current, [])
            continue
        if current is None:
            continue
        if raw.startswith("@@ "):
            m = re.search(r"\+(\d+)(?:,(\d+))?", raw)
            new_line = int(m.group(1)) if m else 0
            continue
        if raw.startswith("+") and not raw.startswith("+++"):
            per_file[current].append(new_line)
            new_line += 1
        elif raw.startswith("-") and not raw.startswith("---"):
            pass
        else:
            new_line += 1
    return {k: v for k, v in per_file.items() if v}


def package_key(path):
    idx = path.find("kotlin/")
    if idx >= 0:
        rel = path[idx + len("kotlin/"):]
        parts = rel.split("/")
        return ("/".join(parts[:-1]), parts[-1])
    return (None, path.rsplit("/", 1)[-1])


def load_kover(xml_path):
    tree = ET.parse(xml_path)
    index = {}
    for pkg in tree.getroot().iter("package"):
        pname = pkg.get("name")
        for src in pkg.iter("sourcefile"):
            lines = {}
            for ln in src.iter("line"):
                try:
                    nr = int(ln.get("nr"))
                except (TypeError, ValueError):
                    continue
                mi = int(ln.get("mi", "0") or 0)
                lines[nr] = mi == 0
            index[(pname, src.get("name"))] = lines
    return index


def main():
    ap = argparse.ArgumentParser(description="Kover delta coverage (advisory)")
    ap.add_argument("--xml", default=DEFAULT_XML)
    ap.add_argument("--output", default=DEFAULT_OUT)
    ap.add_argument("--range", default=None,
                    help="explicit git diff endpoint args, e.g. 'HEAD~3 HEAD'")
    args = ap.parse_args()

    lines_out = []
    emit = lambda s="": (lines_out.append(s), print(s))

    endpoints, mode = resolve_range(args.range)
    emit("kover-delta: %s" % mode)
    try:
        index = load_kover(args.xml)
    except (IOError, ET.ParseError) as e:
        emit("kover-delta: N/A -- cannot read Kover XML %s (%s)" % (
            args.xml, e))
        emit("run :shared:koverXmlReport first; advisory only, exit 0")
        finalize(args.output, lines_out)
        return 0
    emit("kover-delta: kover xml %s" % os.path.relpath(args.xml, CLIENT_DIR))

    code, diff_text, err = run_git(
        ["diff", "-U0", "--no-color"] + endpoints + ["--", "*.kt"])
    if code != 0:
        emit("kover-delta: N/A -- git diff failed (%s)" % err.strip())
        finalize(args.output, lines_out)
        return 0

    changed = added_lines(diff_text)
    if not changed:
        emit("kover-delta: N/A -- no added/modified Kotlin lines under "
             "shared/src in range (docs-only or clean-tree edge; nothing to "
             "score, not a failure)")
        emit("informational floor %.0f%% for new code -- not enforced" %
             INFO_FLOOR_PCT)
        finalize(args.output, lines_out)
        return 0

    total_cov = total_exe = 0
    uncovered = []
    rows = []
    for path in sorted(changed):
        pkg, base = package_key(path)
        cov = index.get((pkg, base))
        if cov is None and pkg is not None:
            same = [(p, n) for (p, n) in index if n == base]
            cov = index[same[0]] if len(same) == 1 else None
        missing = cov is None
        c = x = 0
        for nr in changed[path]:
            if cov is None or nr not in cov:
                continue
            x += 1
            if cov[nr]:
                c += 1
            else:
                uncovered.append("%s:%d" % (path, nr))
        rows.append((path, c, x, len(changed[path]), missing))
        total_cov += c
        total_exe += x

    pct = 100.0 * total_cov / total_exe if total_exe else None
    if pct is None:
        emit("kover-delta: N/A -- added lines are all non-executable "
             "(imports/comments/blanks score nothing)")
    else:
        emit("kover-delta: changed-lines %.1f%% (%d/%d executable added "
             "lines covered)" % (pct, total_cov, total_exe))
    emit("per-file (covered/executable/added):")
    for path, c, x, n, missing in rows:
        cell = ("%.1f%%" % (100.0 * c / x)) if x else "n/a (non-executable)"
        if missing:
            cell += " [not in kover report: test source or uninstrumented]"
        emit("  %s %d/%d/%d %s" % (cell, c, x, n, path))
    if uncovered:
        emit("uncovered added lines (first %d of %d):" % (
            min(len(uncovered), UNCOVERED_CAP), len(uncovered)))
        for u in uncovered[:UNCOVERED_CAP]:
            emit("  %s" % u)
    else:
        emit("uncovered added lines: none")
    emit("informational floor %.0f%% for new code -- not enforced "
         "(report-only; gating waits for triaged delta data)" % INFO_FLOOR_PCT)
    finalize(args.output, lines_out)
    return 0


def finalize(out_path, lines):
    try:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")
        print("kover-delta: report written to %s" % os.path.relpath(
            out_path, CLIENT_DIR))
    except OSError as e:
        print("kover-delta: could not write %s (%s)" % (out_path, e))


if __name__ == "__main__":
    sys.exit(main())
