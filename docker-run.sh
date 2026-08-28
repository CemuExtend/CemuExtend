#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
image_name="${CEMU_DOCKER_RUNTIME_IMAGE:-cemu-extend:runtime}"
frontend="${CEMU_FRONTEND:-cef}"
runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
gdk_backend="${GDK_BACKEND:-}"

if [[ "${frontend}" == "webview" ]]; then
	frontend=cef
fi

if [[ "${frontend}" == "cef" ]]; then
	if [[ -z "${DISPLAY:-}" || ! -d /tmp/.X11-unix ]]; then
		printf 'The CEF desktop frontend requires an X11 or XWayland DISPLAY.\n' >&2
		exit 2
	fi
	if [[ -n "${gdk_backend}" && "${gdk_backend}" != "x11" ]]; then
		printf 'The CEF desktop frontend requires GDK_BACKEND=x11.\n' >&2
		exit 2
	fi
	gdk_backend=x11
elif [[ -z "${gdk_backend}" ]]; then
	if [[ -n "${WAYLAND_DISPLAY:-}" && -S "${runtime_dir}/${WAYLAND_DISPLAY}" ]]; then
		gdk_backend=wayland
	else
		gdk_backend=x11
	fi
fi

case "${frontend}" in
	cef|wx) ;;
	*)
		printf 'Docker desktop runtime supports cef or wx, got: %s\n' "${frontend}" >&2
		exit 2
		;;
esac

git_hash="$(git -C "${project_dir}" log --format=%h -1 2>/dev/null || printf unknown)"
commit_hash="$(git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown)"
source_fingerprint="$({
	git -C "${project_dir}" rev-parse HEAD 2>/dev/null || printf unknown
	git -C "${project_dir}" diff --binary --no-ext-diff HEAD 2>/dev/null || true
	git -C "${project_dir}" ls-files --others --exclude-standard -z 2>/dev/null \
		| LC_ALL=C sort -z \
		| xargs -0 -r sha256sum
} | sha256sum | cut -d' ' -f1)"

docker build --progress=plain --target runtime \
	--build-arg "GIT_HASH=${git_hash}" \
	--build-arg "CEMU_EXTEND_COMMIT_HASH=${commit_hash}" \
	--build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
	--build-arg "CEMU_FRONTEND=${frontend}" \
	--build-arg "CLEAN_BUILD=${CEMU_CLEAN_BUILD:-0}" \
	-t "${image_name}" "${project_dir}"

data_dir="${CEMU_DOCKER_DATA_DIR:-${XDG_DATA_HOME:-${HOME}/.local/share}/Cemu-Docker}"
config_dir="${CEMU_DOCKER_CONFIG_DIR:-${XDG_CONFIG_HOME:-${HOME}/.config}/Cemu-Docker}"
cache_dir="${CEMU_DOCKER_CACHE_DIR:-${XDG_CACHE_HOME:-${HOME}/.cache}/Cemu-Docker}"
mkdir -p "${data_dir}" "${config_dir}" "${cache_dir}"

run_args=(
	--rm
	--init
	--user "$(id -u):$(id -g)"
	--env HOME=/home/cemu
	--env XDG_CACHE_HOME=/home/cemu/.cache
	--env XDG_CONFIG_HOME=/home/cemu/.config
	--env XDG_DATA_HOME=/home/cemu/.local/share
	--env XDG_RUNTIME_DIR=/tmp/cemu-runtime
	--env MESA_SHADER_CACHE_DIR=/home/cemu/.cache/Cemu/mesa_shader_cache
	--env "GDK_BACKEND=${gdk_backend}"
	--volume "${data_dir}:/home/cemu/.local/share/Cemu"
	--volume "${config_dir}:/home/cemu/.config/Cemu"
	--volume "${cache_dir}:/home/cemu/.cache/Cemu"
)

if [[ "${frontend}" == "wx" && "${gdk_backend}" == "wayland" &&
	-n "${WAYLAND_DISPLAY:-}" && -S "${runtime_dir}/${WAYLAND_DISPLAY}" ]]; then
	run_args+=(
		--env "WAYLAND_DISPLAY=${WAYLAND_DISPLAY}"
		--volume "${runtime_dir}/${WAYLAND_DISPLAY}:/tmp/cemu-runtime/${WAYLAND_DISPLAY}"
	)
fi

if [[ -n "${DISPLAY:-}" && -d /tmp/.X11-unix ]]; then
	run_args+=(
		--env "DISPLAY=${DISPLAY}"
		--volume /tmp/.X11-unix:/tmp/.X11-unix:rw
	)
	xauthority="${XAUTHORITY:-}"
	if [[ -z "${xauthority}" && -f "${HOME}/.Xauthority" ]]; then
		xauthority="${HOME}/.Xauthority"
	fi
	if [[ -n "${xauthority}" && -f "${xauthority}" ]]; then
		run_args+=(
			--env XAUTHORITY=/tmp/cemu-runtime/Xauthority
			--volume "${xauthority}:/tmp/cemu-runtime/Xauthority:ro"
		)
	fi
fi

if [[ -S "${runtime_dir}/pulse/native" ]]; then
	run_args+=(
		--env PULSE_SERVER=unix:/tmp/cemu-runtime/pulse-native
		--volume "${runtime_dir}/pulse/native:/tmp/cemu-runtime/pulse-native"
	)
	pulse_cookie="${PULSE_COOKIE:-}"
	if [[ -z "${pulse_cookie}" && -f "${XDG_CONFIG_HOME:-${HOME}/.config}/pulse/cookie" ]]; then
		pulse_cookie="${XDG_CONFIG_HOME:-${HOME}/.config}/pulse/cookie"
	elif [[ -z "${pulse_cookie}" && -f "${HOME}/.pulse-cookie" ]]; then
		pulse_cookie="${HOME}/.pulse-cookie"
	fi
	if [[ -n "${pulse_cookie}" && -f "${pulse_cookie}" ]]; then
		run_args+=(
			--env PULSE_COOKIE=/tmp/cemu-runtime/pulse-cookie
			--volume "${pulse_cookie}:/tmp/cemu-runtime/pulse-cookie:ro"
		)
	fi
fi

if [[ -d /dev/dri ]]; then
	run_args+=(--volume /dev/dri:/dev/dri)
	for device in /dev/dri/renderD* /dev/dri/card*; do
		if [[ -e "${device}" ]]; then
			run_args+=(--group-add "$(stat -c %g "${device}")")
		fi
	done
fi

exec docker run "${run_args[@]}" "${image_name}" "$@"
