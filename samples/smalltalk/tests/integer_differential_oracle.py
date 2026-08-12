#!/usr/bin/env python3

import random
import subprocess
import sys


def encode(value: int) -> tuple[str, str]:
    return ("-" if value < 0 else "+", format(abs(value), "x"))


def floor_divide(left: int, right: int) -> int:
    return left // right


def compare(left: int, right: int) -> int:
    return (left > right) - (left < right)


def expected(operation: str, left: int, right: int) -> int:
    if operation == "add":
        return left + right
    if operation == "sub":
        return left - right
    if operation == "mul":
        return left * right
    if operation == "div":
        return floor_divide(left, right)
    if operation == "mod":
        return left % right
    if operation == "cmp":
        return compare(left, right)
    if operation == "shift":
        return left << right if right >= 0 else left >> -right
    raise AssertionError(f"unknown operation {operation}")


def make_cases() -> list[tuple[str, int, int]]:
    generator = random.Random(0x4C494E540001)
    cases: list[tuple[str, int, int]] = []
    boundaries = [
        0,
        1,
        -1,
        (1 << 60) - 1,
        -(1 << 60),
        1 << 60,
        -(1 << 60) - 1,
        (1 << 127) - 1,
        -(1 << 127),
        (1 << 256) + 0x123456789ABCDEF,
    ]
    for left in boundaries:
        for right in boundaries:
            for operation in ("add", "sub", "mul", "cmp"):
                cases.append((operation, left, right))
            if right != 0:
                cases.append(("div", left, right))
                cases.append(("mod", left, right))
    for _ in range(800):
        left_bits = generator.randrange(0, 513)
        right_bits = generator.randrange(0, 513)
        left = generator.getrandbits(left_bits)
        right = generator.getrandbits(right_bits)
        if generator.randrange(2):
            left = -left
        if generator.randrange(2):
            right = -right
        operation = generator.choice(("add", "sub", "mul", "div", "mod", "cmp"))
        if operation in ("div", "mod") and right == 0:
            right = 1
        cases.append((operation, left, right))
    for _ in range(200):
        bits = generator.randrange(0, 513)
        left = generator.getrandbits(bits)
        if generator.randrange(2):
            left = -left
        cases.append(("shift", left, generator.randrange(-700, 701)))
    return cases


def integer_binary64_bits(value: int) -> int:
    sign = (1 << 63) if value < 0 else 0
    magnitude = abs(value)
    if magnitude == 0:
        return 0
    bit_length = magnitude.bit_length()
    exponent = bit_length - 1
    if bit_length <= 53:
        significand = magnitude << (53 - bit_length)
    else:
        discarded = bit_length - 53
        significand, remainder = divmod(magnitude, 1 << discarded)
        halfway = 1 << (discarded - 1)
        if remainder > halfway or (
            remainder == halfway and (significand & 1) != 0
        ):
            significand += 1
            if significand == (1 << 53):
                significand >>= 1
                exponent += 1
    if exponent > 1023:
        return sign | 0x7FF0000000000000
    return sign | ((exponent + 1023) << 52) | (significand & ((1 << 52) - 1))


def finite_binary64_ratio(bits: int) -> tuple[int, int]:
    negative = (bits >> 63) != 0
    exponent = (bits >> 52) & 0x7FF
    fraction = bits & ((1 << 52) - 1)
    if exponent == 0:
        significand = fraction
        shift = -1074
    else:
        significand = (1 << 52) | fraction
        shift = exponent - 1023 - 52
    numerator = -significand if negative else significand
    if shift >= 0:
        return numerator << shift, 1
    return numerator, 1 << -shift


def rounded_binary64_integer(operation: str, bits: int) -> int:
    numerator, denominator = finite_binary64_ratio(bits)
    if operation == "ffloor":
        return numerator // denominator
    if operation == "fceil":
        return -((-numerator) // denominator)
    negative = numerator < 0
    quotient, remainder = divmod(abs(numerator), denominator)
    if operation == "fround" and remainder * 2 >= denominator:
        quotient += 1
    return -quotient if negative else quotient


def make_conversion_cases() -> tuple[list[int], list[tuple[str, int]]]:
    generator = random.Random(0x42494E4152593634)
    integers = [
        0,
        1,
        -1,
        (1 << 53) + 1,
        (1 << 53) + 3,
        (1 << 60) - 1,
        1 << 60,
        -(1 << 60),
        1 << 1023,
        1 << 1024,
    ]
    for _ in range(500):
        magnitude = generator.getrandbits(generator.randrange(0, 2049))
        integers.append(-magnitude if generator.randrange(2) else magnitude)

    float_cases: list[tuple[str, int]] = []
    boundary_bits = [
        0,
        1,
        0x8000000000000000,
        0x8000000000000001,
        0x3FF8000000000000,
        0xBFF8000000000000,
        0x4004000000000000,
        0x43B0000000000000,
        0x7FEFFFFFFFFFFFFF,
        0xFFEFFFFFFFFFFFFF,
    ]
    finite_bits = list(boundary_bits)
    while len(finite_bits) < 510:
        bits = generator.getrandbits(64)
        if ((bits >> 52) & 0x7FF) != 0x7FF:
            finite_bits.append(bits)
    for bits in finite_bits:
        for operation in ("ftrunc", "ffloor", "fceil", "fround"):
            float_cases.append((operation, bits))
    return integers, float_cases


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    process = subprocess.Popen(
        [sys.argv[1], "--oracle"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
        encoding="ascii",
    )
    assert process.stdin is not None
    assert process.stdout is not None
    cases = make_cases()
    for operation, left, right in cases:
        left_sign, left_hex = encode(left)
        right_sign, right_hex = encode(right)
        process.stdin.write(
            f"{operation}\t{left_sign}\t{left_hex}\t"
            f"{right_sign}\t{right_hex}\n"
        )
        process.stdin.flush()
        response = process.stdout.readline().rstrip("\n").split("\t")
        if len(response) != 3 or response[0] != "ok":
            process.kill()
            raise AssertionError(
                f"malformed runtime response for {operation} {left} {right}: {response!r}"
            )
        actual = int(response[2], 16)
        if response[1] == "-":
            actual = -actual
        wanted = expected(operation, left, right)
        if actual != wanted:
            process.kill()
            raise AssertionError(
                f"{operation} mismatch: {left}, {right}: got {actual}, expected {wanted}"
            )

    integer_cases, float_cases = make_conversion_cases()
    for value in integer_cases:
        value_sign, value_hex = encode(value)
        process.stdin.write(f"asfloat\t{value_sign}\t{value_hex}\t+\t0\n")
        process.stdin.flush()
        response = process.stdout.readline().rstrip("\n").split("\t")
        wanted = integer_binary64_bits(value)
        if len(response) != 2 or response[0] != "bits":
            process.kill()
            raise AssertionError(f"malformed Integer->Float response: {response!r}")
        actual = int(response[1], 16)
        if actual != wanted:
            process.kill()
            raise AssertionError(
                f"Integer->Float mismatch: {value}: got {actual:016x}, expected {wanted:016x}"
            )
    for operation, bits in float_cases:
        process.stdin.write(f"{operation}\t+\t{bits:x}\t+\t0\n")
        process.stdin.flush()
        response = process.stdout.readline().rstrip("\n").split("\t")
        if len(response) != 3 or response[0] != "ok":
            process.kill()
            raise AssertionError(
                f"malformed Float->Integer response for {bits:016x}: {response!r}"
            )
        actual = int(response[2], 16)
        if response[1] == "-":
            actual = -actual
        wanted = rounded_binary64_integer(operation, bits)
        if actual != wanted:
            process.kill()
            raise AssertionError(
                f"{operation} mismatch for {bits:016x}: got {actual}, expected {wanted}"
            )
    process.stdin.close()
    return_code = process.wait()
    if return_code != 0:
        raise AssertionError(f"runtime oracle driver exited with {return_code}")
    print(
        "integer differential oracle: "
        f"{len(cases)} arithmetic, {len(integer_cases)} Integer->Float, "
        f"{len(float_cases)} Float->Integer cases passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
