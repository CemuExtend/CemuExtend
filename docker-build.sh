#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
image_name="${CEMU_DOCKER_IMAGE:-cemu-extend:build}"
frontend="${CEMU_FRONTEND:-webview}"
overlay_backend="${CEMU_OVERLAY_BACKEND:-}"
clean_build="${CEMU_CLEAN_BUILD:-0}"
case "${frontend}" in
	webview|wx|headless) ;;
	*)
		printf 'Unsupported CEMU_FRONTEND: %s (expected webview, wx, or headless)\n' "${frontend}" >&2
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
artifact_name="Cemu_release"
if [[ "${frontend}" == "headless" ]]; then
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
docker build --progress=plain --target build \
	--build-arg "GIT_HASH=${git_hash}" \
	--build-arg "CEMU_EXTEND_COMMIT_HASH=${commit_hash}" \
	--build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
	--build-arg "CEMU_FRONTEND=${frontend}" \
	--build-arg "CEMU_OVERLAY_BACKEND=${overlay_backend}" \
	--build-arg "CLEAN_BUILD=${clean_build}" \
	-t "${image_name}" "${project_dir}"

mkdir -p "${artifact_dir}"
# Extraction is additive, so remove the obsolete generic CEF copy that older
# bundles may have left behind. libcef.so belongs in the bundle root beside its
# ICU/resource files.
rm -f "${artifact_dir}/.cemu-runtime/lib/libcef.so"
rm -f "${artifact_dir}/libvulkan.so.1"
docker run --rm --user "$(id -u):$(id -g)" \
	-v "${artifact_dir}:/artifacts" \
	"${image_name}" \
	cp -a /Cemu_release.bundle/. /artifacts/
if [[ "${artifact_name}" != "Cemu_release" ]]; then
	mv -f "${artifact_dir}/Cemu_release" "${artifact_dir}/${artifact_name}"
fi

printf 'Docker release build: %s\n' "${artifact_dir}/${artifact_name}"
sha256sum "${artifact_dir}/${artifact_name}"
