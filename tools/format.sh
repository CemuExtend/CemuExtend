#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Pin the formatter image. The floating `silkeh/clang:20` tag has previously
# published an internally inconsistent clang/libclang pair, making formatting
# fail before it reads any source file. Keep updates explicit and reproducible.
clang_format_image="${CLANG_FORMAT_IMAGE:-silkeh/clang:20-bookworm@sha256:203abb1df1563b3bd19cd596d67112e0c0ad380a428810dc9a61c85e555b1323}"

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
        ! -path 'src/imgui/imgui_impl_metal.h' \
        ! -path 'src/Cafe/HW/Latte/Renderer/MetalView.h' \
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
