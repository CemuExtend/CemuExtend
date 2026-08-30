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
rpx_contract_staging_dir=""
rpl_link_contract_staging_dir=""
rpl_call_contract_staging_dir=""
release_staging_dir=""
readonly rpx_contract_staging_marker='.cemu-rpx-contract-staging'
readonly rpl_link_contract_staging_marker='.cemu-rpl-link-contract-staging'
readonly rpl_call_contract_staging_marker='.cemu-rpl-call-contract-staging'
readonly release_staging_marker='.cemu-rust-release-staging'
readonly -a rpx_contract_artifacts=(fixture.rpx rust-trace.jsonl SHA256SUMS cex-trace-compare)
readonly -a rpl_link_contract_artifacts=(main.rpx provider.rpl rust-trace.jsonl SHA256SUMS cex-trace-compare)
readonly -a rpl_call_contract_artifacts=(main.rpx provider.rpl rust-link-trace.jsonl rust-execution-trace.jsonl SHA256SUMS cex-trace-compare)

cleanup_format_fix_output_dirs() {
	for output_dir in "${format_fix_output_dir}" "${ui_format_fix_output_dir}"; do
		if [[ -n "${output_dir}" && -d "${output_dir}" ]]; then
			rm -rf -- "${output_dir}"
		fi
	done
}

cleanup_rpx_contract_staging() {
	local status=$?
	local cleanup_status=0

	if [[ -n "${rpx_contract_staging_dir}" ]]; then
		if [[ "${rpx_contract_staging_dir}" == "${project_dir}"/.docker-rpx-contract.* \
			&& -d "${rpx_contract_staging_dir}" \
			&& ! -L "${rpx_contract_staging_dir}" \
			&& -f "${rpx_contract_staging_dir}/${rpx_contract_staging_marker}" \
			&& ! -L "${rpx_contract_staging_dir}/${rpx_contract_staging_marker}" \
			&& "$(<"${rpx_contract_staging_dir}/${rpx_contract_staging_marker}")" == 'cemu-extend-rpx-contract-staging-v1' ]]; then
			if ! rm -rf -- "${rpx_contract_staging_dir}"; then
				printf 'Could not remove the temporary RPX contract staging directory\n' >&2
				cleanup_status=1
			fi
		else
			printf 'Refusing to remove an unexpected temporary RPX contract staging directory\n' >&2
			cleanup_status=1
		fi
		# Do not retry an invalid or failed cleanup against a path that may have
		# changed after this check.
		rpx_contract_staging_dir=""
	fi

	if (( status != 0 )); then
		return "${status}"
	fi
	return "${cleanup_status}"
}

cleanup_rust_release_staging() {
	local status=$?
	local cleanup_status=0

	if [[ -n "${release_staging_dir}" ]]; then
		if [[ "${release_staging_dir}" == "${project_dir}"/.docker-rust-release.* \
			&& -d "${release_staging_dir}" \
			&& ! -L "${release_staging_dir}" \
			&& -f "${release_staging_dir}/${release_staging_marker}" \
			&& ! -L "${release_staging_dir}/${release_staging_marker}" \
			&& "$(<"${release_staging_dir}/${release_staging_marker}")" == 'cemu-extend-rust-release-staging-v1' ]]; then
			if ! rm -rf -- "${release_staging_dir}"; then
				printf 'Could not remove the temporary Rust release staging directory\n' >&2
				cleanup_status=1
			fi
		else
			printf 'Refusing to remove an unexpected temporary Rust release staging directory\n' >&2
			cleanup_status=1
		fi
		# Do not retry an invalid or failed cleanup against a path that may have
		# changed after this check.
		release_staging_dir=""
	fi

	if (( status != 0 )); then
		return "${status}"
	fi
	return "${cleanup_status}"
}

cleanup_rpl_link_contract_staging() {
	local status=$?
	local cleanup_status=0

	if [[ -n "${rpl_link_contract_staging_dir}" ]]; then
		if [[ "${rpl_link_contract_staging_dir}" == "${project_dir}"/.docker-rpl-link-contract.* \
			&& -d "${rpl_link_contract_staging_dir}" \
			&& ! -L "${rpl_link_contract_staging_dir}" \
			&& -f "${rpl_link_contract_staging_dir}/${rpl_link_contract_staging_marker}" \
			&& ! -L "${rpl_link_contract_staging_dir}/${rpl_link_contract_staging_marker}" \
			&& "$(<"${rpl_link_contract_staging_dir}/${rpl_link_contract_staging_marker}")" == 'cemu-extend-rpl-link-contract-staging-v1' ]]; then
			if ! rm -rf -- "${rpl_link_contract_staging_dir}"; then
				printf 'Could not remove the temporary RPL link contract staging directory\n' >&2
				cleanup_status=1
			fi
		else
			printf 'Refusing to remove an unexpected temporary RPL link contract staging directory\n' >&2
			cleanup_status=1
		fi
		rpl_link_contract_staging_dir=""
	fi

	if (( status != 0 )); then
		return "${status}"
	fi
	return "${cleanup_status}"
}

cleanup_rpl_call_contract_staging() {
	local status=$?
	local cleanup_status=0

	if [[ -n "${rpl_call_contract_staging_dir}" ]]; then
		if [[ "${rpl_call_contract_staging_dir}" == "${project_dir}"/.docker-rpl-call-contract.* \
			&& -d "${rpl_call_contract_staging_dir}" && ! -L "${rpl_call_contract_staging_dir}" \
			&& -f "${rpl_call_contract_staging_dir}/${rpl_call_contract_staging_marker}" \
			&& ! -L "${rpl_call_contract_staging_dir}/${rpl_call_contract_staging_marker}" \
			&& "$(<"${rpl_call_contract_staging_dir}/${rpl_call_contract_staging_marker}")" == 'cemu-extend-rpl-call-contract-staging-v1' ]]; then
			if ! rm -rf -- "${rpl_call_contract_staging_dir}"; then
				printf 'Could not remove the temporary RPL call contract staging directory\n' >&2
				cleanup_status=1
			fi
		else
			printf 'Refusing to remove an unexpected temporary RPL call contract staging directory\n' >&2
			cleanup_status=1
		fi
		rpl_call_contract_staging_dir=""
	fi
	if (( status != 0 )); then return "${status}"; fi
	return "${cleanup_status}"
}

cleanup_output_dirs() {
	local status=${1:-$?}
	local cleanup_status=0

	if ! cleanup_format_fix_output_dirs; then
		cleanup_status=1
	fi
	if ! cleanup_rpx_contract_staging; then
		cleanup_status=1
	fi
	if ! cleanup_rpl_link_contract_staging; then
		cleanup_status=1
	fi
	if ! cleanup_rpl_call_contract_staging; then
		cleanup_status=1
	fi
	if ! cleanup_rust_release_staging; then
		cleanup_status=1
	fi
	if (( status != 0 )); then
		return "${status}"
	fi
	return "${cleanup_status}"
}

validate_rpx_contract_artifacts() {
	local directory=$1
	local artifact entry

	if [[ ! -d "${directory}" || -L "${directory}" ]]; then
		printf 'RPX contract artifact staging is invalid\n' >&2
		return 1
	fi
	for artifact in "${rpx_contract_artifacts[@]}"; do
		if [[ ! -f "${directory}/${artifact}" || -L "${directory}/${artifact}" \
			|| ! -s "${directory}/${artifact}" ]]; then
			printf 'RPX contract artifact staging is incomplete\n' >&2
			return 1
		fi
	done
	if [[ ! -x "${directory}/cex-trace-compare" ]]; then
		printf 'RPX contract comparator artifact is not executable\n' >&2
		return 1
	fi
	while IFS= read -r -d '' entry; do
		case "${entry}" in
			fixture.rpx|rust-trace.jsonl|SHA256SUMS|cex-trace-compare) ;;
			"${rpx_contract_staging_marker}")
				if [[ "${directory}" != "${rpx_contract_staging_dir}" ]]; then
					printf 'RPX contract artifact staging contains unexpected entries\n' >&2
					return 1
				fi
				;;
			*)
				printf 'RPX contract artifact staging contains unexpected entries\n' >&2
				return 1
				;;
		esac
	done < <(find "${directory}" -mindepth 1 -maxdepth 1 -printf '%f\0')
	if ! LC_ALL=C awk '
		function hex64(value) { return length(value) == 64 && value ~ /^[0-9a-f]+$/ }
		NF == 2 && hex64($1) && $2 == "fixture.rpx" { fixture++; next }
		NF == 2 && hex64($1) && $2 == "rust-trace.jsonl" { trace++; next }
		NF == 2 && hex64($1) && $2 == "cex-trace-compare" { comparator++; next }
		{ invalid = 1 }
		END { exit invalid || fixture != 1 || trace != 1 || comparator != 1 }
	' "${directory}/SHA256SUMS"; then
		printf 'RPX contract checksum manifest is invalid\n' >&2
		return 1
	fi
	if ! (cd -- "${directory}" && sha256sum --check --status SHA256SUMS); then
		printf 'RPX contract checksum verification failed\n' >&2
		return 1
	fi
}

validate_rpl_link_contract_artifacts() {
	local directory=$1
	local artifact entry

	if [[ ! -d "${directory}" || -L "${directory}" ]]; then
		printf 'RPL link contract artifact staging is invalid\n' >&2
		return 1
	fi
	for artifact in "${rpl_link_contract_artifacts[@]}"; do
		if [[ ! -f "${directory}/${artifact}" || -L "${directory}/${artifact}" \
			|| ! -s "${directory}/${artifact}" ]]; then
			printf 'RPL link contract artifact staging is incomplete\n' >&2
			return 1
		fi
	done
	if [[ ! -x "${directory}/cex-trace-compare" ]]; then
		printf 'RPL link contract comparator artifact is not executable\n' >&2
		return 1
	fi
	while IFS= read -r -d '' entry; do
		case "${entry}" in
			main.rpx|provider.rpl|rust-trace.jsonl|SHA256SUMS|cex-trace-compare) ;;
			"${rpl_link_contract_staging_marker}")
				if [[ "${directory}" != "${rpl_link_contract_staging_dir}" ]]; then
					printf 'RPL link contract artifact staging contains unexpected entries\n' >&2
					return 1
				fi
				;;
			*)
				printf 'RPL link contract artifact staging contains unexpected entries\n' >&2
				return 1
				;;
		esac
	done < <(find "${directory}" -mindepth 1 -maxdepth 1 -printf '%f\0')
	if ! LC_ALL=C awk '
		function hex64(value) { return length(value) == 64 && value ~ /^[0-9a-f]+$/ }
		NF == 2 && hex64($1) && $2 == "main.rpx" { main++; next }
		NF == 2 && hex64($1) && $2 == "provider.rpl" { provider++; next }
		NF == 2 && hex64($1) && $2 == "rust-trace.jsonl" { trace++; next }
		NF == 2 && hex64($1) && $2 == "cex-trace-compare" { comparator++; next }
		{ invalid = 1 }
		END { exit invalid || main != 1 || provider != 1 || trace != 1 || comparator != 1 }
	' "${directory}/SHA256SUMS"; then
		printf 'RPL link contract checksum manifest is invalid\n' >&2
		return 1
	fi
	if ! (cd -- "${directory}" && sha256sum --check --status SHA256SUMS); then
		printf 'RPL link contract checksum verification failed\n' >&2
		return 1
	fi
}

validate_rpl_call_contract_artifacts() {
	local directory=$1
	local artifact entry mode

	if [[ ! -d "${directory}" || -L "${directory}" ]]; then
		printf 'RPL call contract artifact staging is invalid\n' >&2; return 1
	fi
	for artifact in "${rpl_call_contract_artifacts[@]}"; do
		if [[ ! -f "${directory}/${artifact}" || -L "${directory}/${artifact}" || ! -s "${directory}/${artifact}" ]]; then
			printf 'RPL call contract artifact staging is incomplete\n' >&2; return 1
		fi
		mode=$(stat -c '%a' -- "${directory}/${artifact}")
		case "${artifact}:${mode}" in
			cex-trace-compare:755|rust-link-trace.jsonl:600|rust-execution-trace.jsonl:600|main.rpx:600|provider.rpl:600|SHA256SUMS:644) ;;
			*) printf 'RPL call contract artifact mode is invalid\n' >&2; return 1 ;;
		esac
	done
	while IFS= read -r -d '' entry; do
		case "${entry}" in
			main.rpx|provider.rpl|rust-link-trace.jsonl|rust-execution-trace.jsonl|SHA256SUMS|cex-trace-compare) ;;
			"${rpl_call_contract_staging_marker}")
				[[ "${directory}" == "${rpl_call_contract_staging_dir}" ]] || { printf 'RPL call contract artifact staging contains unexpected entries\n' >&2; return 1; } ;;
			*) printf 'RPL call contract artifact staging contains unexpected entries\n' >&2; return 1 ;;
		esac
	done < <(find "${directory}" -mindepth 1 -maxdepth 1 -printf '%f\0')
	if [[ "$(wc -l < "${directory}/rust-link-trace.jsonl")" -ne 5 \
		|| "$(wc -l < "${directory}/rust-execution-trace.jsonl")" -ne 7 ]]; then
		printf 'RPL call contract trace record count is invalid\n' >&2
		return 1
	fi
	if ! LC_ALL=C awk '
		function hex64(value) { return length(value) == 64 && value ~ /^[0-9a-f]+$/ }
		NF == 2 && hex64($1) && $2 == "main.rpx" { main++; next }
		NF == 2 && hex64($1) && $2 == "provider.rpl" { provider++; next }
		NF == 2 && hex64($1) && $2 == "rust-link-trace.jsonl" { link++; next }
		NF == 2 && hex64($1) && $2 == "rust-execution-trace.jsonl" { execution++; next }
		NF == 2 && hex64($1) && $2 == "cex-trace-compare" { comparator++; next }
		{ invalid = 1 }
		END { exit invalid || main != 1 || provider != 1 || link != 1 || execution != 1 || comparator != 1 }
	' "${directory}/SHA256SUMS"; then
		printf 'RPL call contract checksum manifest is invalid\n' >&2; return 1
	fi
	(cd -- "${directory}" && sha256sum --check --status SHA256SUMS) || { printf 'RPL call contract checksum verification failed\n' >&2; return 1; }
}

exit_after_cleanup() {
	local status=$?
	local cleanup_status=0

	# An EXIT trap's return value does not reliably define the process status.
	# Disable it before exiting so cleanup cannot recurse, then make the policy
	# explicit: preserve the original failure, otherwise report cleanup failure.
	trap - EXIT
	if ! cleanup_output_dirs "${status}"; then
		cleanup_status=1
	fi
	if (( status != 0 )); then
		exit "${status}"
	fi
	exit "${cleanup_status}"
}

trap exit_after_cleanup EXIT

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
	oracle-smoke)
		docker_target=rust-oracle-smoke
		;;
	rpx-contract-artifacts|rpx-artifacts)
		docker_target=rust-rpx-contract-artifacts
		;;
	rpl-link-contract-artifacts)
		docker_target=rust-rpl-link-contract-artifacts
		;;
	rpl-call-contract-artifacts)
		docker_target=rust-rpl-call-contract-artifacts
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
		printf 'Usage: %s [lock|format|format-fix|ui-format-fix|check|clippy|test|audit|ci|oracle-smoke|rpx-contract-artifacts|rpl-link-contract-artifacts|rpl-call-contract-artifacts|headless|release|ui]\n' "${0##*/}" >&2
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
	if [[ -e "${artifact_dir}" || -L "${artifact_dir}" ]]; then
		printf 'Refusing to replace an existing Rust release artifact destination\n' >&2
		exit 1
	fi
	result_dir="${project_dir}/result"
	if [[ -L "${result_dir}" || ( -e "${result_dir}" && ! -d "${result_dir}" ) ]]; then
		printf 'Rust release artifact parent is not a directory\n' >&2
		exit 1
	fi
	mkdir -p -- "${result_dir}"
	release_staging_dir="$(mktemp -d "${project_dir}/.docker-rust-release.XXXXXX")"
	printf '%s\n' 'cemu-extend-rust-release-staging-v1' \
		> "${release_staging_dir}/${release_staging_marker}"
	docker "${docker_args[@]}" --output "type=local,dest=${release_staging_dir}" "${project_dir}"
	if ! mv -T -n -- "${release_staging_dir}" "${artifact_dir}"; then
		printf 'Could not publish Rust release artifacts\n' >&2
		exit 1
	fi
	if [[ -e "${release_staging_dir}" || -L "${release_staging_dir}" ]]; then
		printf 'Refusing to replace an existing Rust release staging destination\n' >&2
		exit 1
	fi
	release_staging_dir=""
	# The marker travels with the atomic rename. Remove it only after proving
	# that the published directory is still the staging directory we created.
	if [[ ! -d "${artifact_dir}" || -L "${artifact_dir}" \
		|| ! -f "${artifact_dir}/${release_staging_marker}" \
		|| -L "${artifact_dir}/${release_staging_marker}" \
		|| "$(<"${artifact_dir}/${release_staging_marker}")" != 'cemu-extend-rust-release-staging-v1' ]]; then
		printf 'Published Rust release artifact destination failed validation\n' >&2
		exit 1
	fi
	rm -f -- "${artifact_dir}/${release_staging_marker}"
	printf 'Docker Rust release build: %s\n' "${artifact_dir}/Cemu"
	printf 'Docker Rust release notices: %s, %s\n' "${artifact_dir}/LICENSE.txt" "${artifact_dir}/THIRD_PARTY_LICENSES.txt"
elif [[ "${build_kind}" == rpx-contract-artifacts || "${build_kind}" == rpx-artifacts ]]; then
	artifact_dir="${project_dir}/result/rpx-contract"
	result_dir="${project_dir}/result"
	if [[ -e "${artifact_dir}" || -L "${artifact_dir}" ]]; then
		printf 'Refusing to replace an existing RPX contract artifact destination\n' >&2
		exit 1
	fi
	if [[ -L "${result_dir}" || ( -e "${result_dir}" && ! -d "${result_dir}" ) ]]; then
		printf 'RPX contract artifact parent is not a directory\n' >&2
		exit 1
	fi
	mkdir -p -- "${result_dir}"
	rpx_contract_staging_dir="$(mktemp -d "${project_dir}/.docker-rpx-contract.XXXXXX")"
	printf '%s\n' 'cemu-extend-rpx-contract-staging-v1' \
		> "${rpx_contract_staging_dir}/${rpx_contract_staging_marker}"
	docker "${docker_args[@]}" --output "type=local,dest=${rpx_contract_staging_dir}" "${project_dir}"
	validate_rpx_contract_artifacts "${rpx_contract_staging_dir}"
	if ! mv -T -n -- "${rpx_contract_staging_dir}" "${artifact_dir}"; then
		printf 'Could not publish RPX contract artifacts\n' >&2
		exit 1
	fi
	if [[ -e "${rpx_contract_staging_dir}" || -L "${rpx_contract_staging_dir}" ]]; then
		printf 'Refusing to replace an existing RPX contract artifact destination\n' >&2
		exit 1
	fi
	rpx_contract_staging_dir=""
	# The marker travels with the atomic rename.  Remove it only after proving
	# that the published directory is still the staging directory we created.
	if [[ ! -d "${artifact_dir}" || -L "${artifact_dir}" \
		|| ! -f "${artifact_dir}/${rpx_contract_staging_marker}" \
		|| -L "${artifact_dir}/${rpx_contract_staging_marker}" \
		|| "$(<"${artifact_dir}/${rpx_contract_staging_marker}")" != 'cemu-extend-rpx-contract-staging-v1' ]]; then
		printf 'Published RPX contract artifact destination failed validation\n' >&2
		exit 1
	fi
	rm -f -- "${artifact_dir}/${rpx_contract_staging_marker}"
	validate_rpx_contract_artifacts "${artifact_dir}"
	printf 'Docker Rust RPX contract artifacts: %s\n' "${artifact_dir}"
elif [[ "${build_kind}" == rpl-link-contract-artifacts ]]; then
	artifact_dir="${project_dir}/result/rpl-link-contract"
	result_dir="${project_dir}/result"
	if [[ -e "${artifact_dir}" || -L "${artifact_dir}" ]]; then
		printf 'Refusing to replace an existing RPL link contract artifact destination\n' >&2
		exit 1
	fi
	if [[ -L "${result_dir}" || ( -e "${result_dir}" && ! -d "${result_dir}" ) ]]; then
		printf 'RPL link contract artifact parent is not a directory\n' >&2
		exit 1
	fi
	mkdir -p -- "${result_dir}"
	rpl_link_contract_staging_dir="$(mktemp -d "${project_dir}/.docker-rpl-link-contract.XXXXXX")"
	printf '%s\n' 'cemu-extend-rpl-link-contract-staging-v1' \
		> "${rpl_link_contract_staging_dir}/${rpl_link_contract_staging_marker}"
	docker "${docker_args[@]}" --output "type=local,dest=${rpl_link_contract_staging_dir}" "${project_dir}"
	validate_rpl_link_contract_artifacts "${rpl_link_contract_staging_dir}"
	if ! mv -T -n -- "${rpl_link_contract_staging_dir}" "${artifact_dir}"; then
		printf 'Could not publish RPL link contract artifacts\n' >&2
		exit 1
	fi
	if [[ -e "${rpl_link_contract_staging_dir}" || -L "${rpl_link_contract_staging_dir}" ]]; then
		printf 'Refusing to replace an existing RPL link contract artifact destination\n' >&2
		exit 1
	fi
	rpl_link_contract_staging_dir=""
	if [[ ! -d "${artifact_dir}" || -L "${artifact_dir}" \
		|| ! -f "${artifact_dir}/${rpl_link_contract_staging_marker}" \
		|| -L "${artifact_dir}/${rpl_link_contract_staging_marker}" \
		|| "$(<"${artifact_dir}/${rpl_link_contract_staging_marker}")" != 'cemu-extend-rpl-link-contract-staging-v1' ]]; then
		printf 'Published RPL link contract artifact destination failed validation\n' >&2
		exit 1
	fi
	rm -f -- "${artifact_dir}/${rpl_link_contract_staging_marker}"
	validate_rpl_link_contract_artifacts "${artifact_dir}"
	printf 'Docker Rust RPL link contract artifacts: %s\n' "${artifact_dir}"
elif [[ "${build_kind}" == rpl-call-contract-artifacts ]]; then
	artifact_dir="${project_dir}/result/rpl-call-contract"
	result_dir="${project_dir}/result"
	if [[ -e "${artifact_dir}" || -L "${artifact_dir}" ]]; then
		printf 'Refusing to replace an existing RPL call contract artifact destination\n' >&2; exit 1
	fi
	if [[ -L "${result_dir}" || ( -e "${result_dir}" && ! -d "${result_dir}" ) ]]; then
		printf 'RPL call contract artifact parent is not a directory\n' >&2; exit 1
	fi
	mkdir -p -- "${result_dir}"
	rpl_call_contract_staging_dir="$(mktemp -d "${project_dir}/.docker-rpl-call-contract.XXXXXX")"
	printf '%s\n' 'cemu-extend-rpl-call-contract-staging-v1' > "${rpl_call_contract_staging_dir}/${rpl_call_contract_staging_marker}"
	docker "${docker_args[@]}" --output "type=local,dest=${rpl_call_contract_staging_dir}" "${project_dir}"
	validate_rpl_call_contract_artifacts "${rpl_call_contract_staging_dir}"
	if ! mv -T -n -- "${rpl_call_contract_staging_dir}" "${artifact_dir}"; then
		printf 'Could not publish RPL call contract artifacts\n' >&2; exit 1
	fi
	if [[ -e "${rpl_call_contract_staging_dir}" || -L "${rpl_call_contract_staging_dir}" ]]; then
		printf 'Refusing to replace an existing RPL call contract artifact destination\n' >&2; exit 1
	fi
	rpl_call_contract_staging_dir=""
	if [[ ! -d "${artifact_dir}" || -L "${artifact_dir}" \
		|| ! -f "${artifact_dir}/${rpl_call_contract_staging_marker}" \
		|| -L "${artifact_dir}/${rpl_call_contract_staging_marker}" \
		|| "$(<"${artifact_dir}/${rpl_call_contract_staging_marker}")" != 'cemu-extend-rpl-call-contract-staging-v1' ]]; then
		printf 'Published RPL call contract artifact destination failed validation\n' >&2; exit 1
	fi
	rm -f -- "${artifact_dir}/${rpl_call_contract_staging_marker}"
	validate_rpl_call_contract_artifacts "${artifact_dir}"
	printf 'Docker Rust RPL call contract artifacts: %s\n' "${artifact_dir}"
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
