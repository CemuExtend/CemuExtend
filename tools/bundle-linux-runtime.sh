#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 EXECUTABLE OUTPUT_DIRECTORY" >&2
	exit 2
fi

executable="$(readlink -f "$1")"
output_dir="$2"
runtime_dir="${output_dir}/.cemu-runtime"
lib_dir="${runtime_dir}/lib"
fallback_dir="${runtime_dir}/fallback"
launcher="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/Cemu-runtime-launcher.sh"

[[ -x "${executable}" ]] || { echo "executable not found: ${executable}" >&2; exit 1; }
command -v ldd >/dev/null || { echo "ldd is required" >&2; exit 1; }

install -d "${output_dir}" "${lib_dir}" "${runtime_dir}/libexec"
install -m755 "${executable}" "${output_dir}/.Cemu_release.bin"
install -m755 "${launcher}" "${output_dir}/Cemu_release"
# libcef.so must remain beside its resource files in output_dir. A stale copy in
# the generic library directory makes CEF resolve icudtl.dat from the wrong path.
rm -f "${lib_dir}/libcef.so"
# CEF's private SwiftShader loader must not shadow the host Vulkan loader used
# by Cemu. Older additive bundles may have left this conflicting basename here.
rm -f "${output_dir}/libvulkan.so.1"

executable_dir="$(dirname -- "${executable}")"
if [[ -f "${executable_dir}/libcef.so" ]]; then
	for cef_file in \
		libcef.so libEGL.so libGLESv2.so libvk_swiftshader.so \
		vk_swiftshader_icd.json v8_context_snapshot.bin snapshot_blob.bin icudtl.dat resources.pak \
		chrome_100_percent.pak chrome_200_percent.pak CEF-LICENSE.txt CEF-README.txt; do
		[[ -f "${executable_dir}/${cef_file}" ]] || continue
		cp -aL "${executable_dir}/${cef_file}" "${output_dir}/${cef_file}"
	done
	if [[ -f "${executable_dir}/chrome-sandbox" ]]; then
		install -m4755 "${executable_dir}/chrome-sandbox" "${output_dir}/chrome-sandbox"
	fi
	[[ -d "${executable_dir}/locales" ]] || {
		echo "CEF locales are missing next to ${executable}" >&2
		exit 1
	}
	cp -aL "${executable_dir}/locales" "${output_dir}/locales"
	if [[ -f "${executable_dir}/cef-swiftshader/libvulkan.so.1" ]]; then
		install -d "${output_dir}/cef-swiftshader"
		cp -aL "${executable_dir}/cef-swiftshader/libvulkan.so.1" \
			"${output_dir}/cef-swiftshader/libvulkan.so.1"
	fi
fi

elf_inputs=("${executable}")
if [[ -f "${executable_dir}/libcef.so" ]]; then
	nss_library=$(LC_ALL=C ldd "${executable}" | awk '$1 == "libnss3.so" { print $3; exit }')
	nss_dir=""
	[[ -f "${nss_library}" ]] && nss_dir=$(dirname -- "${nss_library}")
	for nss_name in libsoftokn3.so libfreebl3.so libfreeblpriv3.so \
		libnssckbi.so libnssdbm3.so libssl3.so; do
		[[ -f "${nss_dir}/${nss_name}" ]] || continue
		cp -Lf "${nss_dir}/${nss_name}" "${lib_dir}/${nss_name}"
		elf_inputs+=("${nss_dir}/${nss_name}")
	done
	[[ -f "${lib_dir}/libsoftokn3.so" && -f "${lib_dir}/libfreeblpriv3.so" ]] || {
		echo "CEF NSS runtime modules were not found beside libnss3.so" >&2
		exit 1
	}
	# Chromium opens the Xlib/XCB bridge dynamically when using the X11 ozone
	# backend, so it does not appear in ldd's dependency closure. The portable
	# launcher deliberately selects X11 on NixOS; omitting this library makes
	# CEF terminate with SIGTRAP before the first browser is created.
	for cef_runtime_name in libX11-xcb.so.1; do
		cef_runtime_library=$(ldconfig -p 2>/dev/null | awk -v name="${cef_runtime_name}" \
			'$1 == name && $NF ~ /^\// { print $NF; exit }')
		if [[ -z "${cef_runtime_library}" || ! -f "${cef_runtime_library}" ]]; then
			cef_runtime_library=$(find /usr/lib /lib -name "${cef_runtime_name}" \
				-type f -print -quit 2>/dev/null || true)
		fi
		[[ -f "${cef_runtime_library}" ]] || {
			echo "CEF runtime library was not found: ${cef_runtime_name}" >&2
			exit 1
		}
		# Host stack, so it follows the same fallback rule as the libraries below.
		install -d "${fallback_dir}"
		cp -Lf "${cef_runtime_library}" "${fallback_dir}/${cef_runtime_name}"
		rm -f "${lib_dir}/${cef_runtime_name}"
		elf_inputs+=("${cef_runtime_library}")
	done
fi
declare -A libraries=()
for input in "${elf_inputs[@]}"; do
	while read -r library; do
		[[ -n "${library}" && -f "${library}" ]] && libraries["${library}"]=1
	done < <(LC_ALL=C ldd "${input}" 2>/dev/null | awk \
		'/=> \// { print $3 } /^\// { print $1 }')
done

# The host's GPU drivers are loaded into this process and are built against the
# host's display libraries: a Mesa ICD linked against a newer libwayland-client
# fails to resolve its symbols against the older copy bundled here, and the
# Vulkan loader then reports no drivers at all. These libraries therefore go to a
# fallback directory that the launcher only puts on the search path when the host
# does not provide them itself.
is_host_stack_library()
{
	case "$1" in
		libwayland-*.so.*|libX11.so.*|libX11-xcb.so.*|libxcb.so.*|libxcb-*.so.*|\
		libxshmfence.so.*|libdrm.so.*|libgbm.so.*)
			return 0 ;;
	esac
	return 1
}

for library in "${!libraries[@]}"; do
	name=$(basename "${library}")
	case "${name}" in
		ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libnss_*.so.*|\
		libcef.so|libvulkan.so.*)
			continue ;;
	esac
	if is_host_stack_library "${name}"; then
		install -d "${fallback_dir}"
		cp -Lf "${library}" "${fallback_dir}/${name}"
		# A previous additive bundle may still hold the shadowing copy.
		rm -f "${lib_dir}/${name}"
		continue
	fi
	cp -Lf "${library}" "${lib_dir}/${name}"
done

missing_libraries=$(LD_LIBRARY_PATH="${output_dir}:${lib_dir}:${fallback_dir}" LC_ALL=C \
	ldd "${output_dir}/.Cemu_release.bin" 2>/dev/null | \
	awk '$2 == "=>" && $3 == "not" { print $1 }')
if [[ -n "${missing_libraries}" ]]; then
	echo "Bundled runtime has unresolved shared libraries:" >&2
	echo "${missing_libraries}" >&2
	exit 1
fi

gio_module_dir=$(pkg-config --variable=giomoduledir gio-2.0 2>/dev/null || true)
if [[ -z "${gio_module_dir}" || ! -d "${gio_module_dir}" ]]; then
	gio_module_dir=$(find /usr/lib /usr/libexec -maxdepth 5 -path '*/gio/modules' -type d -print -quit 2>/dev/null || true)
fi
if [[ -n "${gio_module_dir}" ]]; then
	install -d "${runtime_dir}/gio/modules"
	cp -aL "${gio_module_dir}/." "${runtime_dir}/gio/modules/"
fi
if [[ -f /usr/share/glib-2.0/schemas/gschemas.compiled ]]; then
	install -d "${runtime_dir}/share/glib-2.0/schemas"
	cp -L /usr/share/glib-2.0/schemas/gschemas.compiled \
		"${runtime_dir}/share/glib-2.0/schemas/"
fi
if [[ -d /usr/share/X11/xkb ]]; then
	install -d "${runtime_dir}/share/X11"
	rm -rf "${runtime_dir}/share/X11/xkb"
	cp -aL /usr/share/X11/xkb "${runtime_dir}/share/X11/xkb"
fi

echo "Bundled Linux runtime: ${output_dir}"
