#!/usr/bin/env python3
"""Build and exercise native Windows binaries with Clang and the Windows SDK.

Run from any directory: py -3 tests/run_windows.py --cc C:/llvm/bin/clang.exe
Logs and a machine-readable result manifest are retained in build/windows.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/windows"


def run(cmd, log, timeout=120):
    try:
        p = subprocess.run([str(x) for x in cmd], cwd=ROOT, capture_output=True,
                           timeout=timeout)
        log.write_bytes(p.stdout + p.stderr)
        return p.returncode
    except subprocess.TimeoutExpired as e:
        log.write_bytes((e.stdout or b"") + (e.stderr or b"") + b"\nTIMEOUT\n")
        return 124
    except OSError as error:
        log.write_text(str(error) + "\n", encoding="utf-8")
        return 127


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--scope", choices=["all", "compiler"], default="all",
                        help="all audits Smalltalk as well; compiler checks ANVIL, MCC and examples")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This runner executes native Windows binaries")
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error("Clang was not found: " + args.cc)
    args.cc = compiler
    os.environ["ANVIL_TEST_CLANG"] = compiler
    flags = [args.cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra",
             "-D_CRT_SECURE_NO_WARNINGS", "-Wno-deprecated-declarations",
             "-Iinclude", "-Isamples/mcc/include", "-Isamples/smalltalk/include"]
    results = []

    def compile_source(src):
        name = str(src.relative_to(ROOT)).replace("\\", "_").replace("/", "_")
        obj = OUT / (name + ".obj")
        code = run(flags + ["-c", src, "-o", obj], OUT / (name + ".log"))
        return src, obj, code

    sources = sorted((ROOT / "src").rglob("*.c"))
    sources += sorted((ROOT / "samples/mcc/src").rglob("*.c"))
    if args.scope == "all":
        sources += sorted((ROOT / "samples/smalltalk/src").rglob("*.c"))
    sources = [src for src in sources if "platform/posix/" not in src.as_posix()]
    objects = {"anvil": [], "mcc": [], "smalltalk": []}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for src, obj, code in pool.map(compile_source, sources):
            relative = src.relative_to(ROOT).as_posix()
            group = "mcc" if relative.startswith("samples/mcc/") else (
                "smalltalk" if relative.startswith("samples/smalltalk/") else "anvil")
            results.append(dict(kind="compile", name=relative, exit=code))
            if code == 0:
                objects[group].append(obj)
            else:
                print("COMPILE FAIL", relative, flush=True)
    librarian = Path(args.cc).with_name("llvm-ar.exe")
    if not librarian.is_file():
        librarian = "llvm-ar"
    libs = {}
    for group in ("anvil", "smalltalk"):
        if group == "smalltalk" and args.scope != "all":
            continue

        libs[group] = OUT / (group + ".lib")
        # Recreate archives to avoid stale members after source removal.
        if libs[group].exists():
            libs[group].unlink()
        code = run([librarian, "rcs", libs[group]] + objects[group],
                   OUT / (group + "_archive.log"))
        if code:
            raise RuntimeError("archive failed: " + group)
    mcc = OUT / "mcc.exe"
    code = run(flags + objects["mcc"] + [libs["anvil"], "-o", mcc], OUT / "mcc_link.log")
    results.append(dict(kind="link", name="mcc", exit=code))
    print("MCC link", code, flush=True)
    for src in sorted((ROOT / "samples/mcc/tests/unit").glob("*.c")):
        name = "mcc_unit_" + src.stem
        binary = OUT / (name + ".exe")
        unit_objects = [obj for obj in objects["mcc"] if obj.name != "samples_mcc_src_main.c.obj"]
        code = run(flags + [src] + unit_objects + [libs["anvil"], "-o", binary], OUT / (name + "_build.log"))
        if code == 0:
            code = run([binary], OUT / (name + "_run.log"))
        results.append(dict(kind="test", name=name, exit=code))
        print(name, code, flush=True)

    for directory, dialect in [("c89", "c89"), ("c99", "c99"), ("c11", "c11"), ("c23", "c23"), ("gnu", "gnu99"), ("", "c99"), ("negative", "c89")]:
        for src in sorted((ROOT / "samples/mcc/tests" / directory).glob("*.c")):
            standard = dialect
            if directory == "negative":
                for prefix in ("c89", "c99", "c11", "c23"):
                    if src.stem.startswith(prefix):
                        standard = prefix
                if src.stem.startswith("ambiguous_"):
                    standard = "c11"
                if src.stem.startswith("gnu_") or src.stem in ("include_next", "pragma_operator"):
                    standard = "gnu99"
            name = "mcc_syntax_" + directory + "_" + src.stem
            code = run([mcc, "-arch=x86_64_windows", "-std=" + standard, "-Isamples/mcc/includes", "-fsyntax-only", src], OUT / (name + ".log"))
            # A crash/timeout is never a successful negative test.
            passed = code == 1 if directory == "negative" else code == 0
            results.append(dict(kind="syntax", name=name, exit=0 if passed else (code or 1)))
            if not passed:
                print(name, "FAIL", code, flush=True)

    tests = sorted((ROOT / "tests").glob("*.c"))
    if args.scope == "all":
        tests += sorted((ROOT / "samples/smalltalk/tests").glob("*_test.c"))
    for src in tests:
        if src.stem.endswith("_shim") or src.stem.endswith("_codegen"):
            continue
        name = ("smalltalk_" if "smalltalk" in src.parts else "") + src.stem
        binary = OUT / (name + ".exe")
        test_libraries = [libs["smalltalk"]] if "smalltalk" in libs else []
        test_libraries.append(libs["anvil"])
        code = run(flags + [src, "tests/platform/windows/host.c"] + test_libraries + ["-o", binary],
                   OUT / (name + "_build.log"))
        kind = "test-build"
        if code == 0:
            kind = "test"
            code = run([binary], OUT / (name + "_run.log"), 60)
        results.append(dict(kind=kind, name=name, exit=code))
        print(kind, name, code, flush=True)
    generator = OUT / "win64_abi_codegen.exe"
    assembly = OUT / "win64_abi.s"
    binary = OUT / "win64_abi.exe"
    commands = [flags + ["tests/win64_abi_codegen.c", libs["anvil"], "-o", generator],
                [generator, assembly],
                flags + ["tests/win64_abi_shim.c", assembly, "-o", binary], [binary]]
    for i, cmd in enumerate(commands):
        code = run(cmd, OUT / ("win64_abi_%d.log" % i))
        if code:
            break
    results.append(dict(kind="abi", name="win64", exit=code))
    print("Win64 ABI", code, flush=True)

    generator = OUT / "atomic_runtime_codegen.exe"
    code = run(flags + ["tests/atomic_runtime_codegen.c", libs["anvil"], "-o", generator], OUT / "atomic_generator_build.log")
    for opt in ("0", "3"):
        folder = OUT / ("atomics_O" + opt)
        folder.mkdir(parents=True, exist_ok=True)
        assembly = folder / "atomics.s"
        binary = folder / "atomics.exe"
        commands = [[generator, assembly, folder / "atomic_cases.h", "win64", opt],
                    flags + ["-I" + str(folder), "tests/atomic_runtime_shim.c", "tests/platform/windows/threads.c", assembly, "-o", binary],
                    [binary]]
        if code == 0:
            for index, command in enumerate(commands):
                code = run(command, folder / ("stage_%d.log" % index))
                if code:
                    break
        results.append(dict(kind="runtime", name="atomics_O" + opt, exit=code))
        print("Atomics O" + opt, code, flush=True)

    generator = OUT / "vector_runtime_codegen.exe"
    code = run(flags + ["tests/vector_runtime_codegen.c", libs["anvil"], "-o", generator], OUT / "vector_generator_build.log")
    for opt in ("0", "3"):
        folder = OUT / ("vectors_O" + opt)
        folder.mkdir(parents=True, exist_ok=True)
        assembly = folder / "vectors.s"
        binary = folder / "vectors.exe"
        commands = [[generator, assembly, "win64", opt],
                    flags + ["tests/vector_runtime_shim.c", assembly, "-o", binary], [binary]]
        if code == 0:
            for index, command in enumerate(commands):
                code = run(command, folder / ("stage_%d.log" % index))
                if code:
                    break
        results.append(dict(kind="runtime", name="vectors_O" + opt, exit=code))
        print("Vectors O" + opt, code, flush=True)

    code = run([sys.executable, ROOT / "tests/run_windows_abi.py", "--cc", args.cc], OUT / "aggregate_abi.log")
    results.append(dict(kind="abi", name="aggregate-interop", exit=code))
    print("Aggregate ABI", code, flush=True)

    code = run([sys.executable, ROOT / "tests/run_windows_abi.py", "--cc", args.cc, "--family", "variadic"], OUT / "variadic_abi.log")
    results.append(dict(kind="abi", name="variadic-interop", exit=code))
    print("Variadic ABI", code, flush=True)

    code = run([sys.executable, ROOT / "tests/run_scalar_variadic_abi.py", "--cc", args.cc, "--mcc", OUT / "mcc.exe",
                "--out", OUT / "scalar-variadic-abi"], OUT / "scalar_variadic_abi.log")
    results.append(dict(kind="abi", name="scalar-variadic-interop", exit=code))
    print("Scalar variadic ABI", code, flush=True)

    generator = OUT / "fcmp_i1_runtime_codegen.exe"
    assembly = OUT / "fcmp_i1.s"
    binary = OUT / "fcmp_i1.exe"
    commands = [flags + ["tests/fcmp_i1_runtime_codegen.c", libs["anvil"], "-o", generator],
                [generator, assembly, "win64"], flags + ["tests/fcmp_i1_runtime_shim.c", assembly, "-o", binary], [binary]]
    for i, cmd in enumerate(commands):
        code = run(cmd, OUT / ("fcmp_i1_%d.log" % i))
        if code:
            break
    results.append(dict(kind="abi", name="fcmp_i1", exit=code))
    print("FCMP/i1", code, flush=True)

    for directory, suffix in [("basic_runtime", "basic_runtime"), ("fp_math_lib", "math"), ("dynamic_array", "dynarray"), ("base64_lib", "base64")]:
        name = "example_" + directory
        generator = OUT / (name + "_generator.exe")
        assembly = OUT / (name + ".s")
        binary = OUT / (name + ".exe")
        source_dir = ROOT / "examples" / directory
        code = run(flags + [source_dir / ("generate_" + suffix + ".c"), libs["anvil"], "-o", generator], OUT / (name + "_build.log"))
        if code == 0:
            with assembly.open("wb") as output, (OUT / (name + "_generate.log")).open("wb") as errors:
                code = subprocess.run([str(generator), "x86_64_windows"], cwd=ROOT, stdout=output, stderr=errors, timeout=60).returncode
        if code == 0:
            code = run(flags + [source_dir / ("test_" + suffix + ".c"), assembly, "-o", binary], OUT / (name + "_link.log"))
        if code == 0:
            code = run([binary], OUT / (name + "_run.log"))
        results.append(dict(kind="example", name=name, exit=code))
        print(name, code, flush=True)
    manifest = "results.json" if args.scope == "all" else "results-compiler.json"
    (OUT / manifest).write_text(json.dumps(results, indent=2), encoding="utf-8")
    failed = sum(r["exit"] != 0 for r in results)
    print("%d results, %d failures; logs: %s" % (len(results), failed, OUT))
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
