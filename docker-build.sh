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
	# This host-only wrapper is not read by the Docker build stage. Excluding it
	# lets logging-only edits reuse the already-verified Cemu image.
	git -C "${project_dir}" diff --binary --no-ext-diff HEAD -- . \
		':(exclude)docker-build.sh' 2>/dev/null || true
	git -C "${project_dir}" ls-files --others --exclude-standard -z 2>/dev/null \
		| LC_ALL=C sort -z \
		| xargs -0 -r sha256sum
} | sha256sum | cut -d' ' -f1)"

build_log="$(mktemp "${TMPDIR:-/tmp}/cemu-docker-build.XXXXXX.log")"
container_id=""
cleanup() {
	rm -f -- "${build_log}"
	if [[ -n "${container_id}" ]]; then
		docker rm -f "${container_id}" >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

filter_build_log() {
	local log_file="$1"
	awk '
		function emit(value) {
			if (!seen[value]++)
				print value
		}
		{
			line = $0
			# Remove BuildKit step/timing prefixes while retaining compiler paths,
			# test names and diagnostics.
			sub(/^#[0-9]+([[:space:]]+[0-9.]+)?[[:space:]]+/, "", line)
			# Cached BuildKit steps echo the complete Dockerfile RUN instruction.
			# Words such as "failed" inside its retry shell are not diagnostics.
			if (line ~ /^\[[^]]+\][[:space:]]+RUN[[:space:]]/ ||
				line ~ /^RUN[[:space:]]/)
				next
			lower = tolower(line)
			if (lower ~ /(^|[^a-z])(warning|error|failed|failure|fatal)([^a-z]|$)/ ||
				lower ~ /undefined reference|ninja: build stopped|tests? passed|100% tests|total test time/ ||
				lower ~ /-- cemu frontend:|-- cemu runtime overlay:|cef .*already available/)
				emit(line)
		}
	' "${log_file}"
}

# The build stage reads the source through a BuildKit bind mount. Bind-mounted
# contents are not part of Docker's layer cache key, so include an explicit
# content fingerprint. An unchanged tree reuses the complete build layer;
# source edits rerun compilation while preserving the vcpkg/CMake cache mounts.
if ! docker build --progress=plain --target "${docker_target}" \
	--build-arg "GIT_HASH=${git_hash}" \
	--build-arg "CEMU_EXTEND_COMMIT_HASH=${commit_hash}" \
	--build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
	--build-arg "CEMU_FRONTEND=${frontend}" \
	--build-arg "CEMU_OVERLAY_BACKEND=${overlay_backend}" \
	--build-arg "CLEAN_BUILD=${clean_build}" \
	--build-arg "ENABLE_WXWIDGETS=${CEMU_ENABLE_WXWIDGETS:-ON}" \
	-t "${image_name}" "${project_dir}" >"${build_log}" 2>&1; then
	printf 'Docker %s release build failed.\n' "${build_platform}" >&2
	filtered_log="$(filter_build_log "${build_log}")"
	if [[ -n "${filtered_log}" ]]; then
		printf '%s\n' "${filtered_log}" >&2
	else
		# Infrastructure failures do not always contain compiler-style markers.
		# Preserve a bounded diagnostic instead of hiding the only useful output.
		tail -n 40 "${build_log}" >&2
	fi
	exit 1
fi
filter_build_log "${build_log}"

mkdir -p "${artifact_dir}"
if [[ "${build_platform}" == "win" ]]; then
	container_id="$(docker create "${image_name}")"
	docker cp "${container_id}:${container_artifact}" "${artifact_dir}/${temporary_name}"
	mv -f "${artifact_dir}/${temporary_name}" "${artifact_dir}/${artifact_name}"
else
	# Extraction is additive, so remove obsolete runtime copies that older
	# bundles may have left behind.
	rm -f "${artifact_dir}/.cemu-runtime/lib/libcef.so"
	rm -f "${artifact_dir}/libvulkan.so.1"
	container_id="$(docker create "${image_name}")"
	docker cp "${container_id}:/Cemu_release.bundle/." "${artifact_dir}"
	if [[ "${artifact_name}" != "Cemu_release" ]]; then
		mv -f "${artifact_dir}/Cemu_release" "${artifact_dir}/${artifact_name}"
	fi
fi

printf 'Docker %s release build: %s\n' "${build_platform}" "${artifact_dir}/${artifact_name}"
sha256sum "${artifact_dir}/${artifact_name}"
