#!/usr/bin/env python3
"""Compare MCC-generated Windows executables with native Clang at all opt levels."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from run_windows import ROOT, OUT, run


def execute(binary):
    try:
        p = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, timeout=10)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, b"", b"TIMEOUT"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--opts", nargs="+", default=["0", "g", "1", "2", "3"])
    parser.add_argument("--filter", default="*")
    parser.add_argument("--fp-vectorize", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This runner executes native Windows binaries")
    folder = OUT / ("mcc-exec-vectorize" if args.fp_vectorize else "mcc-exec")
    folder.mkdir(parents=True, exist_ok=True)
    sources = sorted((ROOT / "samples/mcc/tests/exec").glob(args.filter + ".c"))
    if not sources:
        parser.error("No execution test matches --filter " + args.filter)
    results = []
    for src in sources:
        standard = next((s for s in ("c89", "c11", "c23") if "_" + s in src.stem), "c99")
        if "_gnu" in src.stem:
            standard = "gnu99"
        native = folder / (src.stem + "_native.exe")
        code = run([args.cc, "-std=" + ("c2x" if standard == "c23" else standard),
                    "-D_CRT_SECURE_NO_WARNINGS", src, "-o", native],
                   folder / (src.stem + "_native.log"))
        if code:
            results.append(dict(name=src.stem, stage="reference-build", exit=code))
            print(src.stem, "reference-build FAIL", flush=True)
            continue
        reference = execute(native)
        (folder / (src.stem + "_native.out")).write_bytes(reference[1])
        for opt in args.opts:
            name = src.stem + "_O" + opt
            asm = folder / (name + ".s")
            binary = folder / (name + ".exe")
            stage = "codegen"
            extra = ["-ffp-vectorize"] if args.fp_vectorize else []
            code = run([OUT / "mcc.exe", "-arch=x86_64_windows", "-std=" + standard,
                        "-O" + opt, *extra, "-Isamples/mcc/includes", "-o", asm, src],
                       folder / (name + "_codegen.log"))
            if code == 0:
                stage = "assemble-link"
                code = run([args.cc, asm, "-Xlinker", "legacy_stdio_definitions.lib", "-o", binary],
                           folder / (name + "_link.log"))
            if code == 0:
                stage = "execute"
                observed = execute(binary)
                (folder / (name + ".out")).write_bytes(observed[1])
                (folder / (name + ".err")).write_bytes(observed[2])
                code = 0 if observed == reference and observed[0] != 124 else 1
            results.append(dict(name=name, stage=stage, exit=code))
            print(name, stage, "PASS" if code == 0 else "FAIL", flush=True)
    (folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    failed = sum(r["exit"] != 0 for r in results)
    print("%d comparisons, %d failed" % (len(results), failed))
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
