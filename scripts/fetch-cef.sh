#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="${project_dir}/cmake/CefVersion.cmake"
read_cmake_value() { sed -n "s/^set($1 \"\(.*\)\")$/\1/p" "${version_file}"; }

platform="${1:-${CEMU_CEF_PLATFORM:-}}"
if [[ -z "${platform}" ]]; then
	case "$(uname -s):$(uname -m)" in
		Linux:x86_64) platform=linux64 ;;
		Linux:aarch64|Linux:arm64) platform=linuxarm64 ;;
		Darwin:x86_64) platform=macosx64 ;;
		Darwin:arm64) platform=macosarm64 ;;
		MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) platform=windows64 ;;
		*)
			printf 'Unable to infer a supported CEF platform from %s:%s\n' "$(uname -s)" "$(uname -m)" >&2
			exit 2
			;;
	esac
fi
case "${platform}" in
	linux64|linuxarm64|windows64|macosx64|macosarm64) ;;
	*)
		printf 'Unsupported CEF platform: %s\n' "${platform}" >&2
		exit 2
		;;
esac

cef_version="$(read_cmake_value CEMU_CEF_VERSION)"
cef_sha256="$(read_cmake_value "CEMU_CEF_SHA256_${platform}")"
if [[ -z "${cef_version}" || ! "${cef_sha256}" =~ ^[[:xdigit:]]{64}$ ]]; then
	printf 'Invalid CEF version/hash entry for %s in %s\n' "${platform}" "${version_file}" >&2
	exit 2
fi

archive="cef_binary_${cef_version}_${platform}.tar.bz2"
url="https://cef-builds.spotifycdn.com/${archive}"
download_dir="${CEMU_CEF_DOWNLOAD_DIR:-${project_dir}/dependencies/.cef-downloads}"
destination="${CEF_ROOT:-${project_dir}/dependencies/cef}"
archive_path="${download_dir}/${archive}"
required=(include/cef_app.h cmake/FindCEF.cmake libcef_dll/CMakeLists.txt LICENSE.txt)
case "${platform}" in
	linux64|linuxarm64)
		required+=(Release/libcef.so Release/libEGL.so Release/libGLESv2.so
			Release/chrome-sandbox Release/v8_context_snapshot.bin
			Resources/icudtl.dat Resources/resources.pak Resources/locales)
		;;
	windows64)
		required+=(Release/libcef.dll Release/libcef.lib Release/chrome_elf.dll
			Release/libEGL.dll Release/libGLESv2.dll Release/v8_context_snapshot.bin
			Resources/icudtl.dat Resources/resources.pak Resources/locales)
		;;
	macosx64|macosarm64)
		framework="Release/Chromium Embedded Framework.framework"
		required+=("${framework}/Chromium Embedded Framework"
			"${framework}/Libraries/libEGL.dylib"
			"${framework}/Libraries/libGLESv2.dylib"
			"${framework}/Resources/icudtl.dat"
			"${framework}/Resources/resources.pak")
		;;
esac

sha256_check() {
	if command -v sha256sum >/dev/null 2>&1; then
		printf '%s  %s\n' "${cef_sha256}" "$1" | sha256sum --check --status
	else
		[[ "$(shasum -a 256 "$1" | awk '{print $1}')" == "${cef_sha256}" ]]
	fi
}

mkdir -p "${download_dir}" "$(dirname -- "${destination}")"
destination_parent="$(cd -- "$(dirname -- "${destination}")" && pwd -P)"
destination_basename="$(basename -- "${destination}")"
case "${destination_basename}" in
	''|.|..|/)
		printf 'Refusing unsafe CEF_ROOT destination: %s\n' "${destination}" >&2
		exit 2
		;;
esac
if [[ "${destination_parent}" == / ]]; then
	destination="/${destination_basename}"
else
	destination="${destination_parent}/${destination_basename}"
fi
if [[ "${destination}" == "${project_dir}" ||
	( -n "${HOME:-}" && "${destination}" == "${HOME}" ) ]]; then
	printf 'Refusing unsafe CEF_ROOT destination: %s\n' "${destination}" >&2
	exit 2
fi
marker_value="${cef_version}:${platform}:${cef_sha256}"
destination_valid=true
if [[ ! -f "${destination}/.cemu-cef-version" ]] ||
	[[ "$(<"${destination}/.cemu-cef-version")" != "${marker_value}" ]]; then
	destination_valid=false
else
	for relative in "${required[@]}"; do
		if [[ ! -e "${destination}/${relative}" ]]; then
			destination_valid=false
			break
		fi
	done
fi
if [[ "${destination_valid}" == true ]]; then
	printf 'CEF %s (%s) already available at %s\n' "${cef_version}" "${platform}" "${destination}"
	exit 0
fi
if [[ ! -f "${archive_path}" ]] || ! sha256_check "${archive_path}"; then
	rm -f -- "${archive_path}.partial"
	curl --fail --location --retry 3 --output "${archive_path}.partial" "${url}"
	mv -- "${archive_path}.partial" "${archive_path}"
fi
if ! sha256_check "${archive_path}"; then
	printf 'CEF archive checksum mismatch: %s\n' "${archive_path}" >&2
	exit 1
fi

temporary="$(mktemp -d "${destination}.extract.XXXXXX")"
backup=""
cleanup() {
	rm -rf -- "${temporary}"
	if [[ -n "${backup}" && -e "${backup}" && ! -e "${destination}" ]]; then
		mv -- "${backup}" "${destination}"
	fi
}
trap cleanup EXIT
tar -xjf "${archive_path}" --strip-components=1 -C "${temporary}"

for relative in "${required[@]}"; do
	if [[ ! -e "${temporary}/${relative}" ]]; then
		printf 'CEF %s archive is missing required asset: %s\n' "${platform}" "${relative}" >&2
		exit 1
	fi
done

if [[ -e "${destination}" ]]; then
	backup="${destination}.previous.$$"
	mv -- "${destination}" "${backup}"
fi
mv -- "${temporary}" "${destination}"
temporary=""
printf '%s\n' "${marker_value}" >"${destination}/.cemu-cef-version"
if [[ -n "${backup}" ]]; then
	rm -rf -- "${backup}"
	backup=""
fi
trap - EXIT
printf 'CEF %s (%s) extracted to %s\n' "${cef_version}" "${platform}" "${destination}"
