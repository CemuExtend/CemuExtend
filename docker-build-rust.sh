#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_kind="${1:-ci}"
image_name="${CEMU_RUST_DOCKER_IMAGE:-cemu-extend-rust:${build_kind}}"
rust_version="${CEMU_RUST_VERSION:-1.97.1}"
cargo_deny_version="${CEMU_CARGO_DENY_VERSION:-0.20.2}"
cargo_about_version="${CEMU_CARGO_ABOUT_VERSION:-0.9.2}"
format_fix_output_dir=""
ui_format_fix_output_dir=""

cleanup_format_fix_output_dirs() {
	for output_dir in "${format_fix_output_dir}" "${ui_format_fix_output_dir}"; do
		if [[ -n "${output_dir}" && -d "${output_dir}" ]]; then
			rm -rf -- "${output_dir}"
		fi
	done
}

trap cleanup_format_fix_output_dirs EXIT

case "${build_kind}" in
	lock|lockfile)
		docker_target=lockfile
		;;
	format)
		docker_target=rust-format
		;;
	format-fix)
		docker_target=rust-format-fix
		;;
	ui-format-fix)
		docker_target=rust-ui-format-fix
		;;
	check)
		docker_target=rust-check
		;;
	clippy)
		docker_target=rust-clippy
		;;
	test)
		docker_target=rust-test
		;;
	audit)
		docker_target=rust-deny
		;;
	ci)
		docker_target=ci
		;;
	headless)
		docker_target=rust-headless
		;;
	release)
		docker_target=rust-release
		;;
	ui)
		docker_target=rust-ui
		;;
	*)
		printf 'Usage: %s [lock|format|format-fix|ui-format-fix|check|clippy|test|audit|ci|headless|release|ui]\n' "${0##*/}" >&2
		exit 2
		;;
esac

docker_args=(
	build
	--file "${project_dir}/Dockerfile.rust"
	--progress plain
	--target "${docker_target}"
	--build-arg "RUST_VERSION=${rust_version}"
	--build-arg "CARGO_DENY_VERSION=${cargo_deny_version}"
	--build-arg "CARGO_ABOUT_VERSION=${cargo_about_version}"
)

if [[ "${build_kind}" == lock || "${build_kind}" == lockfile ]]; then
	docker "${docker_args[@]}" --output "type=local,dest=${project_dir}" "${project_dir}"
	printf 'Docker Rust lockfile generated: %s\n' "${project_dir}/Cargo.lock"
elif [[ "${build_kind}" == release ]]; then
	artifact_dir="${project_dir}/result/rust"
	mkdir -p "${artifact_dir}"
	docker "${docker_args[@]}" --output "type=local,dest=${artifact_dir}" "${project_dir}"
	printf 'Docker Rust release build: %s\n' "${artifact_dir}/Cemu"
	printf 'Docker Rust release notices: %s, %s\n' "${artifact_dir}/LICENSE.txt" "${artifact_dir}/THIRD_PARTY_LICENSES.txt"
elif [[ "${build_kind}" == format-fix ]]; then
	format_fix_output_dir="$(mktemp -d "${project_dir}/.docker-format-fix.XXXXXX")"
	docker "${docker_args[@]}" --output "type=local,dest=${format_fix_output_dir}" "${project_dir}"
	cp -a "${format_fix_output_dir}/." "${project_dir}/"
	rm -rf -- "${format_fix_output_dir}"
	format_fix_output_dir=""
	printf 'Docker Rust formatted sources exported: %s\n' "${project_dir}"
elif [[ "${build_kind}" == ui-format-fix ]]; then
	ui_format_fix_output_dir="$(mktemp -d "${project_dir}/.docker-ui-format-fix.XXXXXX")"
	docker "${docker_args[@]}" --output "type=local,dest=${ui_format_fix_output_dir}" "${project_dir}"
	cp -a "${ui_format_fix_output_dir}/ui/." "${project_dir}/ui/"
	rm -rf -- "${ui_format_fix_output_dir}"
	ui_format_fix_output_dir=""
	printf 'Docker UI formatted sources exported: %s\n' "${project_dir}/ui"
else
	docker "${docker_args[@]}" --tag "${image_name}" "${project_dir}"
	printf 'Docker Rust target built: %s (%s)\n' "${docker_target}" "${image_name}"
fi
