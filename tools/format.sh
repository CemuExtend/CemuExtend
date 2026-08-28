#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
clang_format_image="${CLANG_FORMAT_IMAGE:-silkeh/clang:20}"

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker was not found." >&2
    exit 127
fi

mode="format"
if [[ "${1:-}" == "--check" ]]; then
    mode="check"
elif [[ $# -ne 0 ]]; then
    echo "usage: $0 [--check]" >&2
    exit 2
fi

cd "${project_root}"

mapfile -d '' sources < <(
    find src \
        -type f \
        \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \
        -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \) \
        -print0 | sort -z
)

if [[ "${mode}" == "check" ]]; then
    format_args=(--dry-run --Werror)
else
    format_args=(-i)
fi

docker run --rm \
    --user "$(id -u):$(id -g)" \
    --volume "${project_root}:/work" \
    --workdir /work \
    "${clang_format_image}" \
    clang-format "${format_args[@]}" "${sources[@]}"
