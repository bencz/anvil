#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sample_dir=$(cd -- "$script_dir/.." && pwd)
reference_dir=${ST_REFERENCE_DIR:-"$sample_dir/../smalltalk-jit"}
parser_bin=${ST_PARSE_BIN:-"$sample_dir/build/st-parse"}

if [[ ! -x "$parser_bin" ]]; then
    echo "reference corpus: parser executable not found: $parser_bin" >&2
    exit 2
fi
if [[ ! -d "$reference_dir/packages" ]]; then
    echo "reference corpus: packages directory not found: $reference_dir/packages" >&2
    exit 2
fi

count=0
failures=0
corpus_roots=()
for relative in packages tests samples benchmarks python_proto; do
    if [[ -d "$reference_dir/$relative" ]]; then
        corpus_roots+=("$reference_dir/$relative")
    fi
done
if (( ${#corpus_roots[@]} == 0 )); then
    echo "reference corpus: no corpus directories found" >&2
    exit 2
fi
while IFS= read -r source; do
    count=$((count + 1))
    if ! "$parser_bin" "$source" >/dev/null; then
        failures=$((failures + 1))
    fi
done < <(find "${corpus_roots[@]}" -type f -name '*.st' -print | sort)

if (( count == 0 )); then
    echo "reference corpus: no package sources found" >&2
    exit 2
fi
if (( failures != 0 )); then
    echo "reference corpus: $failures/$count file(s) failed" >&2
    exit 1
fi
echo "reference corpus: PASS ($count files)"
