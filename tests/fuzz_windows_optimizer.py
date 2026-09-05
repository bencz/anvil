#!/usr/bin/env python3
"""Deterministic MCC/Clang differential corpus with operation-list reduction."""
import argparse
import json
import os
import random
import shutil
import sys

from run_windows import ROOT, OUT, run
from run_windows_mcc import execute


def make_recipe(seed):
    randomizer = random.Random(seed)
    operations = []
    for _ in range(18):
        left, right = randomizer.sample(["a", "b", "c"], 2)
        shift = randomizer.randrange(1, 32)
        constant = randomizer.randrange(1, 65536)
        operations.append(randomizer.choice([
            f"{left} += {right} ^ {constant}u;",
            f"{left} = ({left} << {shift}) | ({left} >> {32 - shift});",
            f"{left} ^= ({right} & {constant}u) + index;",
            f"{left} = ({left} + {right}) / {constant}u;",
            f"{left} *= {right} | 1u;",
            f"{left} = ({left} >> {shift}) + ({right} << {shift});",
            f"if (({left} ^ index) & 1u)\n            {left} += {right};\n        else\n            {left} ^= {constant}u;",
        ]))

    return dict(seed=seed, rounds=randomizer.randrange(2, 15), operations=operations)


def render(recipe, operations):
    body = "\n        ".join(operations)
    seed = recipe["seed"] & 0xFFFFFFFF
    return f"""#include <stdio.h>

static unsigned kernel(unsigned seed)
{{
    unsigned a = seed;
    unsigned b = {seed ^ 0x91F273AB}u;
    unsigned c = 17u;
    unsigned data[3] = {{ 0u, 1u, 2u }};
    volatile unsigned observed = seed;

    for (unsigned index = 0; index < {recipe['rounds']}u; index++)
    {{
        {body}
        data[0] = a;
        data[2] = b;
        c ^= data[0] + data[2];
        observed = c;
        a += observed;
    }}

    return (a ^ b) + c;
}}

int main(void)
{{
    unsigned seed = {seed}u;
    for (unsigned index = 0; index < 8u; index++)
    {{
        printf("%u\\n", kernel(seed));
        seed = seed * 1664525u + 1013904223u;
    }}

    return 0;
}}
"""


def reference(compiler, source, prefix):
    binary = prefix.with_suffix(".native.exe")
    status = run([compiler, "-std=c99", "-O2", source, "-o", binary], prefix.with_suffix(".native.log"))
    if status:
        return None

    result = execute(binary)
    prefix.with_suffix(".native.out").write_bytes(result[1])
    return result if result[0] == 0 else None


def compare(compiler, source, prefix, opt, expected):
    assembly = prefix.with_suffix(".s")
    binary = prefix.with_suffix(".exe")
    status = run([OUT / "mcc.exe", "-arch=x86_64_windows", "-std=c99", "-O" + opt,
                  "-Isamples/mcc/includes", "-o", assembly, source], prefix.with_suffix(".codegen.log"))
    if status:
        return "codegen"

    status = run([compiler, assembly, "-Xlinker", "legacy_stdio_definitions.lib", "-o", binary], prefix.with_suffix(".link.log"))
    if status:
        return "assemble-link"

    actual = execute(binary)
    prefix.with_suffix(".out").write_bytes(actual[1])
    prefix.with_suffix(".err").write_bytes(actual[2])
    return "execute" if actual != expected else None


def reduce_failure(compiler, folder, recipe, opt, failure, budget):
    operations = list(recipe["operations"])
    granularity = 2
    attempts = 0
    while operations and attempts < budget:
        chunk = max(1, (len(operations) + granularity - 1) // granularity)
        reduced = False
        for start in range(0, len(operations), chunk):
            if attempts == budget:
                break

            candidate = operations[:start] + operations[start + chunk:]
            prefix = folder / ("reduce_%08x_%s_%03d" % (recipe["seed"], opt, attempts))
            source = prefix.with_suffix(".c")
            source.write_text(render(recipe, candidate), encoding="utf-8")
            attempts += 1
            expected = reference(compiler, source, prefix)
            if expected is not None and compare(compiler, source, prefix, opt, expected) == failure:
                operations = candidate
                granularity = max(2, granularity - 1)
                reduced = True
                break

        if reduced:
            continue
        if granularity >= len(operations):
            break

        granularity = min(len(operations), granularity * 2)

    output = folder / ("failure_%08x_O%s_reduced.c" % (recipe["seed"], opt))
    output.write_text(render(recipe, operations), encoding="utf-8")
    return dict(source=str(output.relative_to(ROOT)), operations=len(operations), attempts=attempts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0xA471)
    parser.add_argument("--cases", type=int, default=32)
    parser.add_argument("--opts", nargs="+", choices=["0", "g", "1", "2", "3"], default=["0", "1", "2", "3"])
    parser.add_argument("--reduce-budget", type=int, default=60)
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if os.name != "nt" or not compiler or not (OUT / "mcc.exe").is_file():
        parser.error("Build native MCC with tests/run_windows.py and provide an installed Clang")
    if args.cases < 1 or args.reduce_budget < 0:
        parser.error("--cases must be positive and --reduce-budget nonnegative")

    folder = OUT / "optimizer-fuzz"
    folder.mkdir(parents=True, exist_ok=True)
    results = []
    for case in range(args.cases):
        seed = (args.seed + case) & 0xFFFFFFFF
        recipe = make_recipe(seed)
        prefix = folder / ("case_%08x" % seed)
        source = prefix.with_suffix(".c")
        source.write_text(render(recipe, recipe["operations"]), encoding="utf-8")
        prefix.with_suffix(".json").write_text(json.dumps(recipe, indent=2), encoding="utf-8")
        expected = reference(compiler, source, prefix)
        if expected is None:
            results.append(dict(seed=seed, failure="reference"))
            (folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print("%08x reference FAIL" % seed, flush=True)
            continue

        for opt in args.opts:
            variant = folder / ("case_%08x_O%s" % (seed, opt))
            failure = compare(compiler, source, variant, opt, expected)
            result = dict(seed=seed, opt=opt, failure=failure)
            print("%08x O%s %s" % (seed, opt, failure or "PASS"), flush=True)
            if failure and args.reduce_budget:
                result["reduction"] = reduce_failure(compiler, folder, recipe, opt, failure, args.reduce_budget)
                print("reduced:", result["reduction"]["source"], flush=True)

            results.append(result)
            (folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")

    failures = sum(result["failure"] is not None for result in results)
    print("%d differential executions, %d failures" % (len(results), failures))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
