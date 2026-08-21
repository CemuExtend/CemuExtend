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
launcher="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/Cemu-runtime-launcher.sh"

[[ -x "${executable}" ]] || { echo "executable not found: ${executable}" >&2; exit 1; }
command -v ldd >/dev/null || { echo "ldd is required" >&2; exit 1; }

webkit_library=$(LC_ALL=C ldd "${executable}" | awk '$1 ~ /^libwebkit2gtk-/ { print $3; exit }')
webkit_name=""
webkit_exec_dir=""
if [[ -f "${webkit_library}" ]]; then
	command -v perl >/dev/null || { echo "perl is required for WebKitGTK bundling" >&2; exit 1; }
	webkit_name=$(basename "${webkit_library}" | sed -E 's/^libwebkit2gtk-([0-9]+\.[0-9]+)\.so.*/webkit2gtk-\1/')
	for candidate in \
		"$(dirname "${webkit_library}")/${webkit_name}" \
		"/usr/libexec/${webkit_name}" \
		"/usr/lib/${webkit_name}"; do
		if [[ -x "${candidate}/WebKitWebProcess" ]]; then
			webkit_exec_dir="$(readlink -f "${candidate}")"
			break
		fi
	done
	[[ -n "${webkit_exec_dir}" ]] || { echo "WebKitGTK helper processes were not found" >&2; exit 1; }
fi

install -d "${output_dir}" "${lib_dir}" "${runtime_dir}/libexec"
install -m755 "${executable}" "${output_dir}/.Cemu_release.bin"
install -m755 "${launcher}" "${output_dir}/Cemu_release"

elf_inputs=("${executable}")
if [[ -n "${webkit_exec_dir}" ]]; then
	cp -aL "${webkit_exec_dir}" "${runtime_dir}/libexec/${webkit_name}"
	while IFS= read -r -d '' helper; do elf_inputs+=("${helper}"); done \
		< <(find "${webkit_exec_dir}" -type f -print0)
fi

declare -A libraries=()
for input in "${elf_inputs[@]}"; do
	while read -r library; do
		[[ -n "${library}" && -f "${library}" ]] && libraries["${library}"]=1
	done < <(LC_ALL=C ldd "${input}" 2>/dev/null | awk \
		'/=> \// { print $3 } /^\// { print $1 }')
done

for library in "${!libraries[@]}"; do
	name=$(basename "${library}")
	case "${name}" in
		ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libnss_*.so.*|\
		libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libEGL.so.*|libdrm.so.*|libgbm.so.*|libvulkan.so.*)
			continue ;;
	esac
	cp -Lf "${library}" "${lib_dir}/${name}"
done

gio_module_dir=$(find /usr/lib /usr/libexec -path '*/gio/modules' -type d -print -quit 2>/dev/null || true)
if [[ -n "${gio_module_dir}" ]]; then
	install -d "${runtime_dir}/gio/modules"
	cp -aL "${gio_module_dir}/." "${runtime_dir}/gio/modules/"
fi
if [[ -f /usr/share/glib-2.0/schemas/gschemas.compiled ]]; then
	install -d "${runtime_dir}/share/glib-2.0/schemas"
	cp -L /usr/share/glib-2.0/schemas/gschemas.compiled \
		"${runtime_dir}/share/glib-2.0/schemas/"
fi

if [[ -n "${webkit_exec_dir}" ]]; then
	bundled_webkit="${lib_dir}/$(basename "${webkit_library}")"
	old_exec_path="${webkit_exec_dir}"
	new_exec_path="/tmp/.cemu-wk.dir/${webkit_name}"
	old_bundle_path="${old_exec_path}/injected-bundle/"
	new_bundle_path="/tmp/.cemu-wk.dir/injected-bundle/"
	OLD_PATH="${old_bundle_path}" NEW_PATH="${new_bundle_path}" perl -0pi -e '
	BEGIN { die "replacement is too long\n" if length($ENV{NEW_PATH}) > length($ENV{OLD_PATH}); }
	$replacement = $ENV{NEW_PATH} . "\0" x (length($ENV{OLD_PATH}) - length($ENV{NEW_PATH}));
	s/\Q$ENV{OLD_PATH}\E/$replacement/g;
	' "${bundled_webkit}"
	OLD_PATH="${old_exec_path}" NEW_PATH="${new_exec_path}" perl -0pi -e '
	BEGIN { die "replacement is too long\n" if length($ENV{NEW_PATH}) > length($ENV{OLD_PATH}); }
	$replacement = $ENV{NEW_PATH} . "\0" x (length($ENV{OLD_PATH}) - length($ENV{NEW_PATH}));
	s/\Q$ENV{OLD_PATH}\E/$replacement/g;
	' "${bundled_webkit}"
fi

echo "Bundled Linux runtime: ${output_dir}"
