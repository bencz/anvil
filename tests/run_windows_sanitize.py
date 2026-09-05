#!/usr/bin/env python3
"""Run the ANVIL regressions with native Windows AddressSanitizer instrumentation."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import sys

from run_windows import ROOT, OUT, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if os.name != "nt" or not compiler:
        parser.error("This runner requires Windows and an installed Clang toolchain")

    folder = OUT / "asan"
    folder.mkdir(parents=True, exist_ok=True)
    os.environ["ANVIL_TEST_CLANG"] = compiler
    flags = [compiler, "-std=c11", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
             "-D_CRT_SECURE_NO_WARNINGS", "-Wno-deprecated-declarations", "-Iinclude"]
    sources = [source for source in (ROOT / "src").rglob("*.c")
               if "/platform/posix/" not in source.as_posix()]

    def compile_source(source):
        name = source.relative_to(ROOT).as_posix().replace("/", "_")
        obj = folder / (name + ".obj")
        status = run(flags + ["-c", source, "-o", obj], folder / (name + ".log"))
        if status:
            raise RuntimeError("Instrumented compilation failed: " + str(source))

        return obj

    with ThreadPoolExecutor(4) as pool:
        objects = list(pool.map(compile_source, sources))

    library = folder / "anvil.lib"
    if library.exists():
        library.unlink()

    librarian = Path(compiler).with_name("llvm-ar.exe")
    if run([librarian, "rcs", library] + objects, folder / "archive.log"):
        return 1

    results = []
    for source in sorted((ROOT / "tests").glob("*regression.c")):
        binary = folder / (source.stem + ".exe")
        status = run(flags + [source, "tests/platform/windows/host.c", library, "-o", binary],
                     folder / (source.stem + "_build.log"))
        if status == 0:
            status = run([binary], folder / (source.stem + "_run.log"), 120)

        results.append(dict(name=source.stem, exit=status))
        print(source.stem, status, flush=True)

    (folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    failed = sum(result["exit"] != 0 for result in results)
    print("%d instrumented regressions, %d failures" % (len(results), failed))
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
