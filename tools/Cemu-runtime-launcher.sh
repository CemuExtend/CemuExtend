#!/bin/sh
set -eu

launcher_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
runtime_dir="$launcher_dir/.cemu-runtime"
webkit_link_root=/tmp/.cemu-wk.dir

if [ ! -x "$launcher_dir/.Cemu_release.bin" ]; then
	echo "CemuExtend runtime binary is missing: $launcher_dir/.Cemu_release.bin" >&2
	exit 127
fi

if [ -d "$runtime_dir/libexec/webkit2gtk-4.1" ]; then
	mkdir -p "$webkit_link_root"
	ln -snf "$runtime_dir/libexec/webkit2gtk-4.1" \
		"$webkit_link_root/webkit2gtk-4.1"
	ln -snf "$runtime_dir/libexec/webkit2gtk-4.1/injected-bundle" \
		"$webkit_link_root/injected-bundle"
	export WEBKIT_INJECTED_BUNDLE_PATH="$webkit_link_root/injected-bundle"
	export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
	export WEBKIT_DISABLE_DMABUF_RENDERER="${WEBKIT_DISABLE_DMABUF_RENDERER:-1}"
fi

export LD_LIBRARY_PATH="$runtime_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -d "$runtime_dir/gio/modules" ]; then
	export GIO_MODULE_DIR="$runtime_dir/gio/modules"
fi
if [ -d "$runtime_dir/share" ]; then
	export XDG_DATA_DIRS="$runtime_dir/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
fi

exec "$launcher_dir/.Cemu_release.bin" "$@"
