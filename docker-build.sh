#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
image_name="${CEMU_DOCKER_IMAGE:-cemu-extend:build}"
artifact_dir="${project_dir}/result/bin"
temporary_name=".Cemu_release.$$.tmp"
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
	-t "${image_name}" "${project_dir}"

mkdir -p "${artifact_dir}"
docker run --rm --user "$(id -u):$(id -g)" \
	-v "${artifact_dir}:/artifacts" \
	"${image_name}" \
	cp /Cemu_release "/artifacts/${temporary_name}"
mv -f "${artifact_dir}/${temporary_name}" "${artifact_dir}/Cemu_release"

printf 'Docker release build: %s\n' "${artifact_dir}/Cemu_release"
sha256sum "${artifact_dir}/Cemu_release"
