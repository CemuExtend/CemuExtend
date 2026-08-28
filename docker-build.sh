#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_platform="${1:-linux}"

case "${build_platform}" in
	linux)
		image_name="${CEMU_DOCKER_IMAGE:-cemu-extend:build}"
		docker_target="build"
		artifact_suffix=""
		;;
	win)
		image_name="${CEMU_DOCKER_IMAGE:-cemu-extend:build-win}"
		docker_target="build-windows-artifact"
		container_artifact="/Cemu_release.exe"
		artifact_suffix=".exe"
		;;
	*)
		printf 'Usage: %s [linux|win]\n' "${0##*/}" >&2
		exit 2
		;;
esac

required_submodule_files=(
	"dependencies/Vulkan-Headers/include/vulkan/vulkan.h"
	"dependencies/ZArchive/CMakeLists.txt"
	"dependencies/cubeb/CMakeLists.txt"
	"dependencies/imgui/imgui.cpp"
	"dependencies/libcemuextend/CMakeLists.txt"
	"dependencies/vcpkg/bootstrap-vcpkg.sh"
)
for required_file in "${required_submodule_files[@]}"; do
	if [[ ! -f "${project_dir}/${required_file}" ]]; then
		printf 'Required submodules are not initialized. Run:\n' >&2
		printf '  git submodule update --init --recursive\n' >&2
		exit 1
	fi
done

frontend="${CEMU_FRONTEND:-cef}"
overlay_backend="${CEMU_OVERLAY_BACKEND:-}"
clean_build="${CEMU_CLEAN_BUILD:-0}"
case "${frontend}" in
	cef|webview|wx|headless) ;;
	*)
		printf 'Unsupported CEMU_FRONTEND: %s (expected cef, wx, or headless)\n' "${frontend}" >&2
		exit 2
		;;
esac
case "${overlay_backend}" in
	""|webview|cef|imgui) ;;
	*)
		printf 'Unsupported CEMU_OVERLAY_BACKEND: %s (expected cef or imgui)\n' "${overlay_backend}" >&2
		exit 2
		;;
esac
case "${clean_build}" in
	0|1) ;;
	*)
		printf 'Unsupported CEMU_CLEAN_BUILD: %s (expected 0 or 1)\n' "${clean_build}" >&2
		exit 2
		;;
esac
artifact_dir="${project_dir}/result/bin"
temporary_name=".Cemu_release.$$.tmp${artifact_suffix}"
artifact_name="Cemu_release${artifact_suffix}"
if [[ "${build_platform}" == "linux" && "${frontend}" == "headless" ]]; then
	artifact_name="Cemu_headless"
fi
git_hash="$(git -C "${project_dir}" log --format=%h -1 2>/dev/null || printf unknown)"
commit_hash="$(git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown)"
source_fingerprint="$({
	git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown
	git -C "${project_dir}" diff --binary --no-ext-diff HEAD 2>/dev/null || true
	git -C "${project_dir}" ls-files --others --exclude-standard -z 2>/dev/null \
		| LC_ALL=C sort -z \
		| xargs -0 -r sha256sum
} | sha256sum | cut -d' ' -f1)"

# The build stage reads the source through a BuildKit bind mount. Bind-mounted
# contents are not part of Docker's layer cache key, so include an explicit
# content fingerprint. An unchanged tree reuses the complete build layer;
# source edits rerun compilation while preserving the vcpkg/CMake cache mounts.
docker build --progress=plain --target "${docker_target}" \
	--build-arg "GIT_HASH=${git_hash}" \
	--build-arg "CEMU_EXTEND_COMMIT_HASH=${commit_hash}" \
	--build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
	--build-arg "CEMU_FRONTEND=${frontend}" \
	--build-arg "CEMU_OVERLAY_BACKEND=${overlay_backend}" \
	--build-arg "CLEAN_BUILD=${clean_build}" \
	--build-arg "ENABLE_WXWIDGETS=${CEMU_ENABLE_WXWIDGETS:-ON}" \
	-t "${image_name}" "${project_dir}"

mkdir -p "${artifact_dir}"
if [[ "${build_platform}" == "win" ]]; then
	container_id="$(docker create "${image_name}")"
	trap 'docker rm -f "${container_id}" >/dev/null' EXIT
	docker cp "${container_id}:${container_artifact}" "${artifact_dir}/${temporary_name}"
	mv -f "${artifact_dir}/${temporary_name}" "${artifact_dir}/${artifact_name}"
else
	# Extraction is additive, so remove obsolete runtime copies that older
	# bundles may have left behind.
	rm -f "${artifact_dir}/.cemu-runtime/lib/libcef.so"
	rm -f "${artifact_dir}/libvulkan.so.1"
	container_id="$(docker create "${image_name}")"
	trap 'docker rm -f "${container_id}" >/dev/null' EXIT
	docker cp "${container_id}:/Cemu_release.bundle/." "${artifact_dir}"
	if [[ "${artifact_name}" != "Cemu_release" ]]; then
		mv -f "${artifact_dir}/Cemu_release" "${artifact_dir}/${artifact_name}"
	fi
fi

printf 'Docker %s release build: %s\n' "${build_platform}" "${artifact_dir}/${artifact_name}"
sha256sum "${artifact_dir}/${artifact_name}"
