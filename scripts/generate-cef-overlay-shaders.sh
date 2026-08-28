#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
shader_dir="${repo_dir}/src/Cafe/HW/Latte/Renderer/Vulkan/shaders"
header="${repo_dir}/src/Cafe/HW/Latte/Renderer/Vulkan/VulkanOverlayShaders.h"
compiler=${GLSLANG_VALIDATOR:-glslangValidator}
command -v "${compiler}" >/dev/null || {
	echo "glslangValidator is required to generate ${header}" >&2
	exit 1
}

temp_dir=$(mktemp -d)
trap 'rm -rf -- "${temp_dir}"' EXIT
"${compiler}" -V "${shader_dir}/cef_overlay.vert" -o "${temp_dir}/vertex.spv"
"${compiler}" -V "${shader_dir}/cef_overlay.frag" -o "${temp_dir}/fragment.spv"

generated="${temp_dir}/VulkanOverlayShaders.h"
{
	echo '#pragma once'
	echo '#include <cstdint>'
	echo 'namespace CefOverlayShaders {'
	xxd -i -n vertex "${temp_dir}/vertex.spv" | sed 's/^unsigned char /alignas(4) inline constexpr std::uint8_t /; s/^unsigned int /inline constexpr std::uint32_t /'
	xxd -i -n fragment "${temp_dir}/fragment.spv" | sed 's/^unsigned char /alignas(4) inline constexpr std::uint8_t /; s/^unsigned int /inline constexpr std::uint32_t /'
	echo '} // namespace CefOverlayShaders'
} > "${generated}"
install -m644 "${generated}" "${header}"
