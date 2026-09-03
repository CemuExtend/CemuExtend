#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_platform="${1:-linux}"

case "${build_platform}" in
	linux)
		docker_target=""
		container_artifact=""
		artifact_suffix=""
		;;
	win)
		docker_target="build-windows-artifact"
		container_artifact="Cemu_release.exe"
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
rebundle="${CEMU_REBUNDLE:-0}"
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
case "${rebundle}" in
	0|1) ;;
	*)
		printf 'Unsupported CEMU_REBUNDLE: %s (expected 0 or 1)\n' "${rebundle}" >&2
		exit 2
		;;
esac
artifact_dir="${project_dir}/result/bin"
temporary_name=".Cemu_release.$$.tmp${artifact_suffix}"
artifact_name="Cemu_release${artifact_suffix}"
if [[ "${build_platform}" == "linux" && "${frontend}" == "headless" ]]; then
	artifact_name="Cemu_headless"
fi

compute_runtime_key() {
	{
		printf 'runtime-format=1\nfrontend=%s\noverlay=%s\n' \
			"${frontend}" "${overlay_backend}"
		git -C "${project_dir}" ls-files -z -- \
			Dockerfile vcpkg.json \
			':(glob)**/CMakeLists.txt' 'cmake/**' \
			tools/Cemu-runtime-launcher.sh tools/bundle-linux-runtime.sh \
			| LC_ALL=C sort -z \
			| while IFS= read -r -d '' runtime_input; do
				sha256sum "${project_dir}/${runtime_input}"
			done
		git -C "${project_dir}/dependencies/vcpkg" rev-parse HEAD 2>/dev/null || true
	} | sha256sum | cut -d' ' -f1
}

bundle_build=0
runtime_key=""
runtime_key_file="${artifact_dir}/.cemu-runtime-key"
if [[ "${build_platform}" == "linux" ]]; then
	if [[ "${frontend}" == "headless" ]]; then
		docker_target="build-linux-binary-artifact"
		container_artifact="Cemu_release"
	else
		runtime_key="$(compute_runtime_key)"
		runtime_ready=1
		for runtime_file in Cemu_release .Cemu_release.bin \
			.cemu-runtime/lib; do
			if [[ ! -e "${artifact_dir}/${runtime_file}" ]]; then
				runtime_ready=0
				break
			fi
		done
		if [[ "${frontend}" == "cef" || "${frontend}" == "webview" ]]; then
			for runtime_file in libcef.so resources.pak locales; do
				if [[ ! -e "${artifact_dir}/${runtime_file}" ]]; then
					runtime_ready=0
					break
				fi
			done
		fi
		installed_runtime_key=""
		if [[ -f "${runtime_key_file}" ]]; then
			installed_runtime_key="$(<"${runtime_key_file}")"
		fi
		if [[ "${rebundle}" == "1" || "${runtime_ready}" != "1" ||
			"${installed_runtime_key}" != "${runtime_key}" ]]; then
			docker_target="build-linux-artifact"
			container_artifact="Cemu_release.bundle"
			bundle_build=1
		else
			docker_target="build-linux-binary-artifact"
			container_artifact="Cemu_release"
		fi
	fi
fi

# The CMake tree is a shared BuildKit cache. Serialize callers so two wrappers
# cannot compile into it or replace result/bin at the same time.
mkdir -p "${project_dir}/build"
exec 9>"${project_dir}/build/.docker-build.lock"
if ! flock -n 9; then
	printf 'Another CemuExtend Docker build is already running.\n' >&2
	exit 1
fi

git_hash="$(git -C "${project_dir}" log --format=%h -1 2>/dev/null || printf unknown)"
commit_hash="$(git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown)"
compute_source_fingerprint() {
	{
		git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown
		# These host-only files do not affect the compiled Cemu artifact. Excluding
		# them lets wrapper and cache-policy edits reuse the verified build layer.
		git -C "${project_dir}" diff --binary --no-ext-diff HEAD -- . \
			':(exclude)docker-build.sh' \
			':(exclude)buildkitd.toml' 2>/dev/null || true
		git -C "${project_dir}" ls-files --others --exclude-standard -z -- \
			':(exclude)buildkitd.toml' 2>/dev/null \
			| LC_ALL=C sort -z \
			| xargs -0 -r sha256sum
	} | sha256sum | cut -d' ' -f1
}
source_fingerprint="$(compute_source_fingerprint)"

build_log="$(mktemp "${TMPDIR:-/tmp}/cemu-docker-build.XXXXXX.log")"
export_dir="$(mktemp -d "${TMPDIR:-/tmp}/cemu-docker-export.XXXXXX")"
bundle_staging=""
runtime_binary_temp=""
runtime_key_temp=""
builder_name="${CEMU_DOCKER_BUILDER:-cemu-extend}"
builder_ready=0
cleanup() {
	rm -f -- "${build_log}"
	rm -rf -- "${export_dir}"
	if [[ -n "${bundle_staging}" ]]; then
		rm -rf -- "${bundle_staging}"
	fi
	if [[ -n "${runtime_binary_temp}" ]]; then
		rm -f -- "${runtime_binary_temp}"
	fi
	if [[ -n "${runtime_key_temp}" ]]; then
		rm -f -- "${runtime_key_temp}"
	fi
	if [[ "${builder_ready}" == "1" ]]; then
		# Enforce the cap immediately after successful and failed builds instead
		# of waiting for BuildKit's periodic garbage-collection pass.
		docker buildx prune --builder "${builder_name}" --force \
			--max-used-space "${CEMU_DOCKER_CACHE_MAX:-48gb}" \
			--reserved-space "${CEMU_DOCKER_CACHE_RESERVED:-12gb}" \
			--timeout 2m >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

ensure_builder() {
	if ! docker buildx inspect "${builder_name}" >/dev/null 2>&1; then
		if ! docker buildx create \
			--name "${builder_name}" \
			--driver docker-container \
			--buildkitd-config "${project_dir}/buildkitd.toml" >/dev/null; then
			# A concurrent build may have created it after the inspect above.
			docker buildx inspect "${builder_name}" >/dev/null
		fi
	fi
	docker buildx inspect --builder "${builder_name}" --bootstrap >/dev/null
	builder_ready=1
}

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
ensure_builder
if ! docker buildx build --builder "${builder_name}" \
	--progress=plain --target "${docker_target}" \
	--build-arg "GIT_HASH=${git_hash}" \
	--build-arg "CEMU_EXTEND_COMMIT_HASH=${commit_hash}" \
	--build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
	--build-arg "CEMU_FRONTEND=${frontend}" \
	--build-arg "CEMU_OVERLAY_BACKEND=${overlay_backend}" \
	--build-arg "CLEAN_BUILD=${clean_build}" \
	--build-arg "ENABLE_WXWIDGETS=${CEMU_ENABLE_WXWIDGETS:-ON}" \
	--output "type=local,dest=${export_dir}" \
	"${project_dir}" >"${build_log}" 2>&1; then
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

# BuildKit receives a source snapshot after the fingerprint above is computed.
# If an editor changes compiled inputs while the build runs, the snapshot and
# cache key may describe different source generations. Never publish that
# potentially mixed artifact; the next stable invocation can rebuild it.
final_source_fingerprint="$(compute_source_fingerprint)"
if [[ "${final_source_fingerprint}" != "${source_fingerprint}" ]]; then
	printf 'CemuExtend sources changed during the Docker build; artifact was not installed.\n' >&2
	printf 'Run the build again after edits have finished.\n' >&2
	exit 1
fi

mkdir -p "${artifact_dir}"
if [[ "${build_platform}" == "win" ]]; then
	cp "${export_dir}/${container_artifact}" "${artifact_dir}/${temporary_name}"
	mv -f "${artifact_dir}/${temporary_name}" "${artifact_dir}/${artifact_name}"
elif [[ "${frontend}" == "headless" ]]; then
	cp "${export_dir}/${container_artifact}" "${artifact_dir}/${temporary_name}"
	mv -f "${artifact_dir}/${temporary_name}" "${artifact_dir}/${artifact_name}"
elif [[ "${bundle_build}" != "1" ]]; then
	# Keep the launcher and its already verified runtime in place. Replacing only
	# the payload avoids exporting and copying CEF on ordinary source changes.
	runtime_binary_temp="${artifact_dir}/.Cemu_release.bin.$$.tmp"
	cp "${export_dir}/${container_artifact}" "${runtime_binary_temp}"
	mv -f "${runtime_binary_temp}" "${artifact_dir}/.Cemu_release.bin"
	runtime_binary_temp=""
else
	# Extraction is additive, so remove obsolete runtime copies that older
	# bundles may have left behind.
	rm -f "${artifact_dir}/.cemu-runtime/lib/libcef.so"
	rm -f "${artifact_dir}/libvulkan.so.1"
	# Linux rejects an in-place copy over a running executable with ETXTBSY.
	# Stage the bundle, install its supporting files first, then atomically
	# replace the launcher payload. Existing processes retain the old inode and
	# the next launch uses the newly verified binary.
	bundle_staging="$(mktemp -d "${artifact_dir}/.bundle-stage.XXXXXX")"
	cp -a "${export_dir}/${container_artifact}/." "${bundle_staging}"
	if [[ -f "${bundle_staging}/.Cemu_release.bin" ]]; then
		runtime_binary_temp="${artifact_dir}/.Cemu_release.bin.$$.tmp"
		mv "${bundle_staging}/.Cemu_release.bin" "${runtime_binary_temp}"
	fi
	cp -a "${bundle_staging}/." "${artifact_dir}"
	if [[ -n "${runtime_binary_temp}" ]]; then
		mv -f "${runtime_binary_temp}" "${artifact_dir}/.Cemu_release.bin"
		runtime_binary_temp=""
	fi
	rm -rf -- "${bundle_staging}"
	bundle_staging=""
	if [[ "${artifact_name}" != "Cemu_release" ]]; then
		mv -f "${artifact_dir}/Cemu_release" "${artifact_dir}/${artifact_name}"
	fi
	# Host display libraries now live in .cemu-runtime/fallback, which the
	# launcher only uses when the host lacks them. A copy left in the always
	# preferred library directory by an older bundle would still shadow the
	# host's, so the host's GPU driver could not load.
	if [[ -d "${artifact_dir}/.cemu-runtime/fallback" ]]; then
		for fallback_library in "${artifact_dir}"/.cemu-runtime/fallback/*; do
			[[ -e "${fallback_library}" ]] || continue
			rm -f "${artifact_dir}/.cemu-runtime/lib/$(basename -- "${fallback_library}")"
		done
	fi
	runtime_key_temp="${runtime_key_file}.$$.tmp"
	printf '%s\n' "${runtime_key}" >"${runtime_key_temp}"
	mv -f "${runtime_key_temp}" "${runtime_key_file}"
	runtime_key_temp=""
fi

if [[ "${build_platform}" == "linux" && "${frontend}" != "headless" ]]; then
	if [[ "${bundle_build}" == "1" ]]; then
		printf 'Docker linux release build (runtime bundled): %s\n' \
			"${artifact_dir}/${artifact_name}"
	else
		printf 'Docker linux release build (runtime reused): %s\n' \
			"${artifact_dir}/${artifact_name}"
	fi
	sha256sum "${artifact_dir}/.Cemu_release.bin"
else
	printf 'Docker %s release build: %s\n' "${build_platform}" \
		"${artifact_dir}/${artifact_name}"
	sha256sum "${artifact_dir}/${artifact_name}"
fi
