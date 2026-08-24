#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
image_name="${CEMU_DOCKER_IMAGE:-cemu-extend:appimage}"
enable_wxwidgets="${CEMU_ENABLE_WXWIDGETS:-ON}"
appimage_arch="${CEMU_APPIMAGE_ARCH:-X64}"
artifact_dir="${project_dir}/result/appimage"

git_hash="$(git -C "${project_dir}" log --format=%h -1 2>/dev/null || printf unknown)"
commit_hash="$(git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown)"
source_fingerprint="$({
    git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown
    git -C "${project_dir}" diff --binary --no-ext-diff HEAD 2>/dev/null || true
    git -C "${project_dir}" ls-files --others --exclude-standard -z 2>/dev/null \
        | LC_ALL=C sort -z \
        | xargs -0 -r sha256sum
} | sha256sum | cut -d' ' -f1)"

docker build --progress=plain --target appimage \
    --build-arg "GIT_HASH=${git_hash}" \
    --build-arg "CEMU_EXTEND_COMMIT_HASH=${commit_hash}" \
    --build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
    --build-arg "ENABLE_WXWIDGETS=${enable_wxwidgets}" \
    --build-arg "CEMU_APPIMAGE_ARCH=${appimage_arch}" \
    -t "${image_name}" "${project_dir}"

mkdir -p "${artifact_dir}"

# Copy everything appimage.sh dropped in ./artifacts inside the image out to
# the host. Use a throwaway container instead of `docker run ... cp` since we
# don't know the exact output filename (it's derived from the git hash).
container_id="$(docker create "${image_name}")"
trap 'docker rm -f "${container_id}" >/dev/null' EXIT

docker cp "${container_id}:/workspace/CemuExtend/artifacts/." "${artifact_dir}/"

echo "AppImage build output:"
find "${artifact_dir}" -maxdepth 1 -type f -name '*.AppImage' -exec sha256sum {} \;