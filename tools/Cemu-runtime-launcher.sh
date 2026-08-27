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

library_path="$launcher_dir"
force_x11=0
append_library_dir()
{
	case ":$library_path:" in
		*":$1:"*) ;;
		*) library_path="$library_path:$1" ;;
	esac
}

if [ -d /run/opengl-driver/lib ]; then
	# NixOS exposes the vendor GL implementation through /run/opengl-driver,
	# while the GLVND frontend and the matching GBM/DRM ABI live in immutable
	# store paths. Loading the Ubuntu copies bundled below together with the Nix
	# driver makes WebKitGTK's EGL initialization abort before the React frontend
	# can paint. Discover the driver closure and prefer those host libraries.
	nix_glvnd_found=0
	if command -v nix-store >/dev/null 2>&1; then
		# /run/opengl-driver is the active system graphics closure. It gives us
		# the matching 64-bit GLVND, GBM/DRM and Vulkan loader rather than an
		# arbitrary stale package from /nix/store.
		for nix_reference in $(nix-store -qR /run/opengl-driver 2>/dev/null || true); do
			if [ -e "$nix_reference/lib/libEGL.so.1" ] && [ -e "$nix_reference/lib/libGL.so.1" ]; then
				nix_glvnd_found=1
				append_library_dir "$nix_reference/lib"
			elif [ -e "$nix_reference/lib/libgbm.so.1" ] || \
				[ -e "$nix_reference/lib/libdrm.so.2" ] || \
				[ -e "$nix_reference/lib/libvulkan.so.1" ]; then
				append_library_dir "$nix_reference/lib"
			fi
		done
	fi
	# GLVND is ABI-stable and is not part of Mesa's direct Nix closure because
	# the vendor module is loaded by the frontend. Prefer a host copy when one is
	# installed; the glob remains a no-op on non-Nix systems.
	if [ "$nix_glvnd_found" -eq 0 ]; then
		for nix_glvnd_dir in /nix/store/*-libglvnd-*/lib; do
			if [ -e "$nix_glvnd_dir/libEGL.so.1" ] && [ -e "$nix_glvnd_dir/libGL.so.1" ]; then
				append_library_dir "$nix_glvnd_dir"
				break
			fi
		done
	fi
	append_library_dir /run/opengl-driver/lib
	if [ -d /run/opengl-driver/share/glvnd/egl_vendor.d ]; then
		export __EGL_VENDOR_LIBRARY_DIRS="${__EGL_VENDOR_LIBRARY_DIRS:-/run/opengl-driver/share/glvnd/egl_vendor.d}"
	fi
	# The portable Ubuntu GTK/WebKit stack cannot safely share its Wayland
	# wl_surface with the NixOS Vulkan WSI. The result is a protocol error as soon
	# as a game creates its swapchain. Use XWayland by default for this portable
	# runtime; advanced users can explicitly opt back into native Wayland.
	if [ "${CEMU_USE_WAYLAND:-0}" != "1" ] && [ -n "${DISPLAY:-}" ]; then
		force_x11=1
		export GDK_BACKEND=x11
		unset WAYLAND_DISPLAY
	fi
	# Avoid probing every Mesa ICD from the mixed portable/Nix closure on an
	# NVIDIA system. Preserve an explicit user selection.
	if [ -z "${VK_DRIVER_FILES:-}" ] && \
		[ -e /run/opengl-driver/lib/libGLX_nvidia.so.0 ] && \
		[ -f /run/opengl-driver/share/vulkan/icd.d/nvidia_icd.json ]; then
		export VK_DRIVER_FILES=/run/opengl-driver/share/vulkan/icd.d/nvidia_icd.json
	fi
	[ -d /run/opengl-driver/lib/gbm ] && export GBM_BACKENDS_PATH="${GBM_BACKENDS_PATH:-/run/opengl-driver/lib/gbm}"
	[ -d /run/opengl-driver/lib/dri ] && export LIBGL_DRIVERS_PATH="${LIBGL_DRIVERS_PATH:-/run/opengl-driver/lib/dri}"
fi
if [ -n "${NIX_LD_LIBRARY_PATH:-}" ]; then
	library_path="$library_path:$NIX_LD_LIBRARY_PATH"
fi
export LD_LIBRARY_PATH="$library_path:$runtime_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cef_sandbox="$launcher_dir/chrome-sandbox"
if [ ! -f "$cef_sandbox" ] || [ ! -u "$cef_sandbox" ] || [ "$(stat -c %u "$cef_sandbox" 2>/dev/null || printf 1)" != "0" ]; then
	# Portable archives cannot preserve a root-owned setuid helper when unpacked
	# by an ordinary user. Keep the sandbox when it is correctly installed and
	# otherwise use CEF's explicit runtime opt-out instead of aborting at startup.
	export CEMU_CEF_NO_SANDBOX="${CEMU_CEF_NO_SANDBOX:-1}"
fi
if [ -d "$runtime_dir/gio/modules" ]; then
	export GIO_MODULE_DIR="$runtime_dir/gio/modules"
fi
if [ -d "$runtime_dir/share" ]; then
	export XDG_DATA_DIRS="$runtime_dir/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
fi
if [ -d "$runtime_dir/share/X11/xkb" ]; then
	export XKB_CONFIG_ROOT="${XKB_CONFIG_ROOT:-$runtime_dir/share/X11/xkb}"
fi

if [ "$force_x11" -eq 1 ]; then
	has_ozone_platform=0
	for argument in "$@"; do
		case "$argument" in
			--ozone-platform|--ozone-platform=*) has_ozone_platform=1 ;;
		esac
	done
	if [ "$has_ozone_platform" -eq 0 ]; then
		set -- --ozone-platform=x11 "$@"
	fi
fi

if [ "${CEMU_CEF_NO_SANDBOX:-0}" = "1" ]; then
	# Chromium's Linux zygote assumes sandbox facilities that are unavailable in
	# the portable fallback. Launch child processes directly in that mode.
	exec "$launcher_dir/.Cemu_release.bin" --no-zygote "$@"
fi
exec "$launcher_dir/.Cemu_release.bin" "$@"
