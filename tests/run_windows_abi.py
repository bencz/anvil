#!/usr/bin/env python3
"""Check native C ABI interoperability in both MCC/Clang call directions."""
import argparse
import json
import os
import sys
from run_windows import ROOT, OUT, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--family", choices=["aggregate", "variadic"], default="aggregate")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This runner executes native Windows binaries")

    folder = OUT / (args.family + "-abi-interop")
    folder.mkdir(parents=True, exist_ok=True)
    sources = ROOT / "tests/windows_gaps"
    results = []
    native = {}
    for role in ("caller", "callee"):
        source = sources / (args.family + "_abi_" + role + ".c")
        native[role] = folder / (role + "_native.obj")
        code = run([args.cc, "-std=c99", "-O2", "-c", source, "-o", native[role]], folder / (role + "_native.log"))
        if code:
            return code

    for opt in ("0", "g", "1", "2", "3"):
        generated = {}
        for role in ("caller", "callee"):
            name = role + "_O" + opt
            source = sources / (args.family + "_abi_" + role + ".c")
            assembly = folder / (name + ".s")
            generated[role] = folder / (name + ".obj")
            code = run([OUT / "mcc.exe", "-arch=x86_64_windows", "-std=c99", "-O" + opt,
                        "-Isamples/mcc/includes", source, "-o", assembly], folder / (name + "_codegen.log"))
            if code == 0:
                code = run([args.cc, "-c", assembly, "-o", generated[role]], folder / (name + "_assemble.log"))
            results.append(dict(name=name, stage="compile", exit=code))
            if code:
                generated[role] = None

        for direction in ("mcc-caller", "mcc-callee", "mcc-both"):
            caller = native["caller"] if direction == "mcc-callee" else generated["caller"]
            callee = native["callee"] if direction == "mcc-caller" else generated["callee"]
            if caller is None or callee is None:
                continue

            name = direction + "_O" + opt
            binary = folder / (name + ".exe")
            code = run([args.cc, caller, callee, "-o", binary], folder / (name + "_link.log"))
            stage = "link"
            if code == 0:
                stage = "execute"
                code = run([binary], folder / (name + "_run.log"))
            results.append(dict(name=name, stage=stage, exit=code))
            print(name, stage, code, flush=True)

    (folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    failed = sum(result["exit"] != 0 for result in results)
    print("%d ABI stages, %d failures" % (len(results), failed))
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
