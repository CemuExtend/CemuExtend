#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="${project_dir}/cmake/CefVersion.cmake"
read_cmake_value() { sed -n "s/^set($1 \"\(.*\)\")$/\1/p" "${version_file}"; }

cef_version="$(read_cmake_value CEMU_CEF_VERSION)"
cef_platform="$(read_cmake_value CEMU_CEF_PLATFORM)"
cef_sha256="$(read_cmake_value CEMU_CEF_SHA256)"
archive="cef_binary_${cef_version}_${cef_platform}.tar.bz2"
url="https://cef-builds.spotifycdn.com/${archive}"
download_dir="${CEMU_CEF_DOWNLOAD_DIR:-${project_dir}/dependencies/.cef-downloads}"
destination="${CEF_ROOT:-${project_dir}/dependencies/cef}"
archive_path="${download_dir}/${archive}"

mkdir -p "${download_dir}" "$(dirname -- "${destination}")"
if [[ ! -f "${archive_path}" ]] || ! printf '%s  %s\n' "${cef_sha256}" "${archive_path}" | sha256sum --check --status; then
	rm -f -- "${archive_path}.partial"
	curl --fail --location --retry 3 --output "${archive_path}.partial" "${url}"
	mv -- "${archive_path}.partial" "${archive_path}"
fi
printf '%s  %s\n' "${cef_sha256}" "${archive_path}" | sha256sum --check

temporary="$(mktemp -d "${destination}.extract.XXXXXX")"
trap 'rm -rf -- "${temporary}"' EXIT
tar -xjf "${archive_path}" --strip-components=1 -C "${temporary}"
if [[ -e "${destination}" ]]; then
	backup="${destination}.previous.$$"
	mv -- "${destination}" "${backup}"
	mv -- "${temporary}" "${destination}"
	rm -rf -- "${backup}"
else
	mv -- "${temporary}" "${destination}"
fi
trap - EXIT
printf 'CEF %s extracted to %s\n' "${cef_version}" "${destination}"
