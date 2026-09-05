#!/usr/bin/env python3
"""Execute AArch64 atomics and scalar variadic C ABI interoperability under QEMU.

Requires a host ANVIL archive/MCC, Clang with LLD, a Linux AArch64 sysroot and
qemu-aarch64. Tools may live in the workspace; no binfmt registration is needed.
"""
import argparse
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--host-cc", default="cc")
    parser.add_argument("--lib", type=Path, required=True)
    parser.add_argument("--mcc", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "build/cross/arm64")
    parser.add_argument("--corpus", action="store_true", help="Also compare the complete MCC execution corpus against Clang under QEMU")
    args = parser.parse_args()
    for name in ("lib", "mcc", "sysroot", "qemu", "out"):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    results = []

    def run(name, stage, command):
        try:
            result = subprocess.run([str(part) for part in command], cwd=ROOT, capture_output=True, timeout=120)
            code = result.returncode
            output = result.stdout + result.stderr
        except subprocess.TimeoutExpired as error:
            code = 124
            output = (error.stdout or b"") + (error.stderr or b"") + b"\nTIMEOUT\n"
        except OSError as error:
            code = 127
            output = str(error).encode("utf-8")
        (args.out / (name + "_" + stage + ".log")).write_bytes(output)
        results.append(dict(name=name, stage=stage, exit=code))
        print(name, stage, code, flush=True)
        return code == 0

    compiler = [args.cc, "--target=aarch64-linux-gnu", "--sysroot=" + str(args.sysroot)]
    runtime = args.sysroot / "lib"

    def link(name, objects, binary):
        # Explicit CRT objects also support extracted Debian cross sysroots,
        # whose libc linker script refers to the original installation paths.
        command = compiler + ["-fuse-ld=lld", "-nostdlib", "-no-pie", "-Wl,-dynamic-linker,/lib/ld-linux-aarch64.so.1",
                              runtime / "crt1.o", runtime / "crti.o", *objects, runtime / "libc.so.6",
                              runtime / "libc_nonshared.a", runtime / "crtn.o", "-o", binary]
        return run(name, "link", command)

    def execute(name, binary):
        return run(name, "execute", [args.qemu, "-L", args.sysroot, binary])

    generator = args.out / "atomic_codegen"
    if run("atomic_generator", "build", [args.host_cc, "-Iinclude", "tests/atomic_runtime_codegen.c", args.lib, "-o", generator]):
        for opt in ("0", "3"):
            name = "atomics_O" + opt
            folder = args.out / name
            folder.mkdir(exist_ok=True)
            assembly = folder / "generated.s"
            binary = folder / "runtime"
            if not run(name, "codegen", [generator, assembly, folder / "atomic_cases.h", "arm64", opt]):
                continue
            objects = ["-I" + str(folder), "tests/atomic_runtime_shim.c", "tests/platform/posix/threads.c", assembly]
            if link(name, objects, binary):
                execute(name, binary)

    sources = ROOT / "tests/abi"
    native = {}
    for role in ("caller", "callee"):
        native[role] = args.out / (role + "_native.o")
        source = sources / ("scalar_variadic_" + role + ".c")
        if not run(role + "_native", "build", compiler + ["-O2", "-c", source, "-o", native[role]]):
            native[role] = None

    for opt in ("0", "g", "1", "2", "3"):
        generated = {}
        for role in ("caller", "callee"):
            name = role + "_O" + opt
            source = sources / ("scalar_variadic_" + role + ".c")
            assembly = args.out / (name + ".s")
            generated[role] = args.out / (name + ".o")
            command = [args.mcc, "-arch=arm64", "-std=c99", "-O" + opt, "-Isamples/mcc/includes", source, "-o", assembly]
            if not run(name, "codegen", command) or not run(name, "assemble", compiler + ["-c", assembly, "-o", generated[role]]):
                generated[role] = None

        for direction in ("mcc-caller", "mcc-callee", "mcc-both"):
            caller = native["caller"] if direction == "mcc-callee" else generated["caller"]
            callee = native["callee"] if direction == "mcc-caller" else generated["callee"]
            if caller is None or callee is None:
                continue
            name = direction + "_O" + opt
            binary = args.out / name
            if link(name, [caller, callee], binary):
                execute(name, binary)

    if args.corpus:
        for source in sorted((ROOT / "samples/mcc/tests/exec").glob("*.c")):
            standard = next((value for value in ("c89", "c11", "c23") if "_" + value in source.stem), "c99")
            if "_gnu" in source.stem:
                standard = "gnu99"
            name = "corpus_" + source.stem
            reference = args.out / (name + "_reference")
            if not link(name + "_reference", ["-std=" + standard, source], reference):
                continue
            try:
                expected = subprocess.run([str(args.qemu), "-L", str(args.sysroot), str(reference)], capture_output=True, timeout=30)
            except subprocess.TimeoutExpired:
                results.append(dict(name=name, stage="reference-timeout", exit=124))
                continue

            for opt in ("0", "g", "1", "2", "3"):
                test = name + "_O" + opt
                assembly = args.out / (test + ".s")
                binary = args.out / test
                command = [args.mcc, "-arch=arm64", "-std=" + standard, "-O" + opt, "-Isamples/mcc/includes", source, "-o", assembly]
                if not run(test, "codegen", command) or not link(test, [assembly], binary):
                    continue
                try:
                    observed = subprocess.run([str(args.qemu), "-L", str(args.sysroot), str(binary)], capture_output=True, timeout=30)
                    equal = (observed.returncode, observed.stdout, observed.stderr) == (expected.returncode, expected.stdout, expected.stderr)
                    output = observed.stdout + observed.stderr
                except subprocess.TimeoutExpired:
                    equal = False
                    output = b"TIMEOUT\n"
                (args.out / (test + "_compare.log")).write_bytes(output)
                (args.out / (name + "_reference.log")).write_bytes(expected.stdout + expected.stderr)
                results.append(dict(name=test, stage="compare", exit=0 if equal else 1))
                print(test, "compare", "PASS" if equal else "FAIL", flush=True)

    (args.out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    failures = sum(result["exit"] != 0 for result in results)
    print("%d AArch64 stages, %d failures" % (len(results), failures))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
