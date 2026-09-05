#!/usr/bin/env python3
"""Execute scalar variadic ABI interoperability on the native Windows/Linux host."""
import argparse
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def run(command, log):
    try:
        result = subprocess.run([str(part) for part in command], cwd=ROOT, capture_output=True, timeout=60)
        log.write_bytes(result.stdout + result.stderr)
        return result.returncode
    except subprocess.TimeoutExpired:
        log.write_text("TIMEOUT\n", encoding="utf-8")
        return 124


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--mcc", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    target = "x86_64_windows" if os.name == "nt" else "x86_64"
    link_flags = ["-Xlinker", "legacy_stdio_definitions.lib"] if os.name == "nt" else ["-no-pie"]
    sources = ROOT / "tests/abi"
    native = {}
    results = []
    for role in ("caller", "callee"):
        native[role] = args.out / (role + "_native.o")
        code = run([args.cc, "-std=c99", "-O2", "-c", sources / ("scalar_variadic_" + role + ".c"), "-o", native[role]], args.out / (role + "_native.log"))
        if code:
            return code

    for opt in ("0", "g", "1", "2", "3"):
        generated = {}
        for role in ("caller", "callee"):
            name = role + "_O" + opt
            assembly = args.out / (name + ".s")
            generated[role] = args.out / (name + ".o")
            code = run([args.mcc.resolve(), "-arch=" + target, "-std=c99", "-O" + opt, "-Isamples/mcc/includes",
                        sources / ("scalar_variadic_" + role + ".c"), "-o", assembly], args.out / (name + "_codegen.log"))
            if not code:
                code = run([args.cc, "-c", assembly, "-o", generated[role]], args.out / (name + "_assemble.log"))
            results.append(dict(name=name, stage="compile", exit=code))
            if code:
                generated[role] = None

        for direction in ("mcc-caller", "mcc-callee", "mcc-both"):
            caller = native["caller"] if direction == "mcc-callee" else generated["caller"]
            callee = native["callee"] if direction == "mcc-caller" else generated["callee"]
            name = direction + "_O" + opt
            binary = args.out / (name + (".exe" if os.name == "nt" else ".out"))
            code = 1
            if caller is not None and callee is not None:
                code = run([args.cc, *link_flags, caller, callee, "-o", binary], args.out / (name + "_link.log"))
                if not code:
                    code = run([binary.resolve()], args.out / (name + "_run.log"))
            results.append(dict(name=name, stage="execute", exit=code))
            print(name, code, flush=True)

    (args.out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    failed = sum(result["exit"] != 0 for result in results)
    print("%d scalar variadic ABI stages, %d failures" % (len(results), failed))
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
