#!/usr/bin/env bash
set -euo pipefail

# Build the frozen C++ oracle from a Docker context that contains only the
# tracked tree and the exact, initialized submodule revisions.  Worktree
# submodules use .git *files* that point back into the host's common gitdir;
# passing the worktree itself to Docker therefore cannot work reliably.

readonly oracle_commit="ab0b772029f0a5cd57c194cf338003fe8eae8ab8"
readonly vcpkg_builtin_baseline="f0fb3ddba5135b80982668de39dbaa139c00d281"
readonly vcpkg_baseline_bundle=".cemu-vcpkg-baseline.bundle"
readonly vcpkg_pinned_ref="refs/tags/cemu-vcpkg-pinned"
readonly vcpkg_bundle_max_bytes=$((512 * 1024 * 1024))
readonly oracle_dockerfile=".cemu-oracle.Dockerfile"
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
oracle_dir="${CEMU_CPP_ORACLE_DIR:-${project_dir}/../CemuExtend-cpp-oracle}"
build_kind="${1:-build}"
keep_context="${CEMU_CPP_ORACLE_KEEP_CONTEXT:-0}"
build_timeout_minutes="${CEMU_CPP_ORACLE_BUILD_TIMEOUT_MINUTES:-30}"
context_dir=""
bundle_repository=""
vcpkg_pinned_commit=""
vcpkg_sdl3_tree=""
build_log=""
export_log=""
rust_artifact_build_log=""
rpl_link_artifact_build_log=""
rpl_call_artifact_build_log=""
build_completed=0
trace_artifacts_dir=""
rpl_link_artifacts_dir=""
rpl_call_artifacts_dir=""
readonly trace_artifacts_marker='.cemu-rpx-oracle-artifacts-staging'
readonly rpl_link_artifacts_marker='.cemu-rpl-link-artifacts-staging'
readonly rpl_call_artifacts_marker='.cemu-rpl-call-artifacts-staging'
readonly -a rpx_contract_artifacts=(fixture.rpx rust-trace.jsonl SHA256SUMS cex-trace-compare)
readonly -a rpl_link_contract_artifacts=(main.rpx provider.rpl rust-trace.jsonl SHA256SUMS cex-trace-compare)
readonly -a rpl_call_link_contract_artifacts=(main.rpx provider.rpl rust-link-trace.jsonl SHA256SUMS cex-trace-compare)

usage() {
	printf 'Usage: %s [base|dev|build|win|trace|rpl-link-trace|rpl-call-trace]\n' "${0##*/}" >&2
	printf 'Builds the fixed C++ oracle revision %s using Docker only.\n' "${oracle_commit}" >&2
}

case "${build_kind}" in
	base)
		docker_target="cemu-extend-base"
		;;
	dev)
		docker_target="dev"
		;;
	build)
		docker_target="build"
		;;
	win)
		docker_target="build-windows-artifact"
		;;
	trace)
		docker_target="cpp-rpx-oracle-trace"
		;;
	rpl-link-trace)
		docker_target="cpp-rpl-link-oracle-trace"
		;;
	rpl-call-trace)
		docker_target="cpp-rpl-call-oracle-trace"
		;;
	*)
		usage
		exit 2
		;;
esac

if [[ ! "${build_timeout_minutes}" =~ ^[0-9]+$ ]]; then
	printf 'CEMU_CPP_ORACLE_BUILD_TIMEOUT_MINUTES must be a non-negative integer (0 disables the timeout)\n' >&2
	exit 2
fi

case "${keep_context}" in
	0|1) ;;
	*)
		printf 'CEMU_CPP_ORACLE_KEEP_CONTEXT must be 0 or 1\n' >&2
		exit 2
		;;
esac

cleanup_bundle_repository() {
	if [[ -z "${bundle_repository}" || ! -d "${bundle_repository}" ]]; then
		bundle_repository=""
		return 0
	fi
	if [[ ! -f "${bundle_repository}/.cemu-vcpkg-bundle-repository" \
		|| "$(<"${bundle_repository}/.cemu-vcpkg-bundle-repository")" != "${oracle_commit}:${vcpkg_pinned_commit}" \
		|| "${bundle_repository}" != /tmp/cemu-extend-vcpkg-bundle.* ]]; then
		printf 'Refusing to remove an unexpected temporary vcpkg bundle repository\n' >&2
		return 1
	fi
	if ! rm -rf -- "${bundle_repository}"; then
		printf 'Could not remove the temporary vcpkg bundle repository\n' >&2
		return 1
	fi
	bundle_repository=""
}

cleanup() {
	local status=${1:-$?}
	local cleanup_status=0

	# The temporary bare repository is only a bundle staging area. It never
	# contains working files and is removed even when a failed Docker context is
	# retained for recovery.
	if ! cleanup_bundle_repository; then
		cleanup_status=1
	fi

	# Preserve a failed context for inspection.  A successful context is removed
	# only when it is the marker-bearing directory made by mktemp below.  Set
	# CEMU_CPP_ORACLE_KEEP_CONTEXT=1 to retain a successful context as well.
	if [[ -z "${context_dir}" || ! -d "${context_dir}" ]]; then
		[[ -z "${export_log}" ]] || printf 'Oracle export log retained: %s\n' "${export_log}" >&2
		[[ -z "${build_log}" || "${build_completed}" -eq 1 ]] || printf 'Docker build log retained: %s\n' "${build_log}" >&2
		[[ -z "${rust_artifact_build_log}" || "${build_completed}" -eq 1 ]] \
			|| printf 'Rust RPX artifact Docker build log retained: %s\n' "${rust_artifact_build_log}" >&2
		[[ -z "${rpl_link_artifact_build_log}" || "${build_completed}" -eq 1 ]] \
			|| printf 'Rust RPL link artifact Docker build log retained: %s\n' "${rpl_link_artifact_build_log}" >&2
		[[ -z "${rpl_call_artifact_build_log}" || "${build_completed}" -eq 1 ]] \
			|| printf 'Rust RPL call artifact Docker build log retained: %s\n' "${rpl_call_artifact_build_log}" >&2
		if (( status != 0 )); then
			return "${status}"
		fi
		return "${cleanup_status}"
	fi

	if [[ "${status}" -ne 0 || "${build_completed}" -ne 1 || "${keep_context}" == 1 ]]; then
		printf 'Docker context retained for recovery: %s\n' "${context_dir}" >&2
		if [[ "${status}" -ne 0 || "${build_completed}" -ne 1 ]]; then
			[[ -z "${export_log}" ]] || printf 'Oracle export log retained: %s\n' "${export_log}" >&2
			[[ -z "${build_log}" ]] || printf 'Docker build log retained: %s\n' "${build_log}" >&2
			[[ -z "${rust_artifact_build_log}" ]] \
				|| printf 'Rust RPX artifact Docker build log retained: %s\n' "${rust_artifact_build_log}" >&2
			[[ -z "${rpl_link_artifact_build_log}" ]] \
				|| printf 'Rust RPL link artifact Docker build log retained: %s\n' "${rpl_link_artifact_build_log}" >&2
			[[ -z "${rpl_call_artifact_build_log}" ]] \
				|| printf 'Rust RPL call artifact Docker build log retained: %s\n' "${rpl_call_artifact_build_log}" >&2
		else
			if [[ -n "${export_log}" ]] && ! rm -f -- "${export_log}"; then
				printf 'Could not remove the temporary oracle export log\n' >&2
				cleanup_status=1
			fi
			if [[ -n "${build_log}" ]] && ! rm -f -- "${build_log}"; then
				printf 'Could not remove the temporary Docker build log\n' >&2
				cleanup_status=1
			fi
			if [[ -n "${rust_artifact_build_log}" ]] && ! rm -f -- "${rust_artifact_build_log}"; then
				printf 'Could not remove the temporary Rust RPX artifact Docker build log\n' >&2
				cleanup_status=1
			fi
			if [[ -n "${rpl_link_artifact_build_log}" ]] && ! rm -f -- "${rpl_link_artifact_build_log}"; then
				printf 'Could not remove the temporary Rust RPL link artifact Docker build log\n' >&2
				cleanup_status=1
			fi
			if [[ -n "${rpl_call_artifact_build_log}" ]] && ! rm -f -- "${rpl_call_artifact_build_log}"; then
				printf 'Could not remove the temporary Rust RPL call artifact Docker build log\n' >&2
				cleanup_status=1
			fi
		fi
		if (( status != 0 )); then
			return "${status}"
		fi
		return "${cleanup_status}"
	fi

	if [[ -f "${context_dir}/.cemu-oracle-context" \
		&& "$(<"${context_dir}/.cemu-oracle-context")" == "${oracle_commit}" \
		&& "${context_dir}" == /tmp/cemu-extend-cpp-oracle.* ]]; then
		if ! rm -rf -- "${context_dir}"; then
			printf 'Could not remove the temporary Docker context\n' >&2
			cleanup_status=1
		fi
		if [[ -n "${export_log}" ]] && ! rm -f -- "${export_log}"; then
			printf 'Could not remove the temporary oracle export log\n' >&2
			cleanup_status=1
		fi
		if [[ -n "${build_log}" ]] && ! rm -f -- "${build_log}"; then
			printf 'Could not remove the temporary Docker build log\n' >&2
			cleanup_status=1
		fi
		if [[ -n "${rust_artifact_build_log}" ]] && ! rm -f -- "${rust_artifact_build_log}"; then
			printf 'Could not remove the temporary Rust RPX artifact Docker build log\n' >&2
			cleanup_status=1
		fi
		if [[ -n "${rpl_link_artifact_build_log}" ]] && ! rm -f -- "${rpl_link_artifact_build_log}"; then
			printf 'Could not remove the temporary Rust RPL link artifact Docker build log\n' >&2
			cleanup_status=1
		fi
		if [[ -n "${rpl_call_artifact_build_log}" ]] && ! rm -f -- "${rpl_call_artifact_build_log}"; then
			printf 'Could not remove the temporary Rust RPL call artifact Docker build log\n' >&2
			cleanup_status=1
		fi
	else
		printf 'Refusing to remove unexpected Docker context\n' >&2
		cleanup_status=1
	fi
	if (( status != 0 )); then
		return "${status}"
	fi
	return "${cleanup_status}"
}

exit_after_general_cleanup() {
	local status=$?
	local cleanup_status=0

	trap - EXIT
	if ! cleanup "${status}"; then
		cleanup_status=1
	fi
	if (( status != 0 )); then
		exit "${status}"
	fi
	exit "${cleanup_status}"
}

trap exit_after_general_cleanup EXIT

cleanup_trace_artifacts() {
	local status=${1:-$?}
	local cleanup_status=0

	if [[ -z "${trace_artifacts_dir}" ]]; then
		if (( status != 0 )); then
			return "${status}"
		fi
		return 0
	fi
	if [[ "${trace_artifacts_dir}" == /tmp/cemu-extend-rpx-artifacts.* \
		&& -d "${trace_artifacts_dir}" \
		&& ! -L "${trace_artifacts_dir}" \
		&& -f "${trace_artifacts_dir}/${trace_artifacts_marker}" \
		&& ! -L "${trace_artifacts_dir}/${trace_artifacts_marker}" \
		&& "$(<"${trace_artifacts_dir}/${trace_artifacts_marker}")" == 'cemu-extend-rpx-oracle-artifacts-v1' ]]; then
		if ! rm -rf -- "${trace_artifacts_dir}"; then
			printf 'Could not remove the temporary RPX oracle artifact directory\n' >&2
			cleanup_status=1
		fi
	else
		printf 'Refusing to remove an unexpected temporary RPX oracle artifact directory\n' >&2
		cleanup_status=1
	fi
	# Do not retry an invalid or failed cleanup against a path that may have
	# changed after this check.
	trace_artifacts_dir=""
	if (( status != 0 )); then
		return "${status}"
	fi
	return "${cleanup_status}"
}

cleanup_rpl_link_artifacts() {
	local status=${1:-$?}
	local cleanup_status=0

	if [[ -z "${rpl_link_artifacts_dir}" ]]; then
		if (( status != 0 )); then return "${status}"; fi
		return 0
	fi
	if [[ "${rpl_link_artifacts_dir}" == /tmp/cemu-extend-rpl-link-artifacts.* \
		&& -d "${rpl_link_artifacts_dir}" && ! -L "${rpl_link_artifacts_dir}" \
		&& -f "${rpl_link_artifacts_dir}/${rpl_link_artifacts_marker}" \
		&& ! -L "${rpl_link_artifacts_dir}/${rpl_link_artifacts_marker}" \
		&& "$(<"${rpl_link_artifacts_dir}/${rpl_link_artifacts_marker}")" == 'cemu-extend-rpl-link-artifacts-v1' ]]; then
		if ! rm -rf -- "${rpl_link_artifacts_dir}"; then
			printf 'Could not remove the temporary RPL link oracle artifact directory\n' >&2
			cleanup_status=1
		fi
	else
		printf 'Refusing to remove an unexpected temporary RPL link oracle artifact directory\n' >&2
		cleanup_status=1
	fi
	rpl_link_artifacts_dir=""
	if (( status != 0 )); then return "${status}"; fi
	return "${cleanup_status}"
}

cleanup_rpl_call_artifacts() {
	local status=${1:-$?}
	local cleanup_status=0
	if [[ -z "${rpl_call_artifacts_dir}" ]]; then
		if (( status != 0 )); then return "${status}"; fi; return 0
	fi
	if [[ "${rpl_call_artifacts_dir}" == /tmp/cemu-extend-rpl-call-artifacts.* \
		&& -d "${rpl_call_artifacts_dir}" && ! -L "${rpl_call_artifacts_dir}" \
		&& -f "${rpl_call_artifacts_dir}/${rpl_call_artifacts_marker}" \
		&& ! -L "${rpl_call_artifacts_dir}/${rpl_call_artifacts_marker}" \
		&& "$(<"${rpl_call_artifacts_dir}/${rpl_call_artifacts_marker}")" == 'cemu-extend-rpl-call-artifacts-v1' ]]; then
		if ! rm -rf -- "${rpl_call_artifacts_dir}"; then
			printf 'Could not remove the temporary RPL call oracle artifact directory\n' >&2; cleanup_status=1
		fi
	else
		printf 'Refusing to remove an unexpected temporary RPL call oracle artifact directory\n' >&2; cleanup_status=1
	fi
	rpl_call_artifacts_dir=""
	if (( status != 0 )); then return "${status}"; fi
	return "${cleanup_status}"
}

exit_after_trace_cleanup() {
	local status=$?
	local cleanup_status=0

	# Preserve a failed Docker/CMake status exactly.  Conversely, a successful
	# trace run must not report success after its generated artifacts or context
	# could not be cleaned up.
	trap - EXIT
	if ! cleanup_trace_artifacts "${status}"; then
		cleanup_status=1
	fi
	if ! cleanup_rpl_link_artifacts "${status}"; then
		cleanup_status=1
	fi
	if ! cleanup_rpl_call_artifacts "${status}"; then
		cleanup_status=1
	fi
	if ! cleanup "${status}"; then
		cleanup_status=1
	fi
	if (( status != 0 )); then
		exit "${status}"
	fi
	exit "${cleanup_status}"
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
			"${trace_artifacts_marker}")
				if [[ "${directory}" != "${trace_artifacts_dir}" ]]; then
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
			"${rpl_link_artifacts_marker}")
				if [[ "${directory}" != "${rpl_link_artifacts_dir}" ]]; then
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

validate_rpl_call_link_contract_artifacts() {
	local directory=$1
	local artifact entry mode
	if [[ ! -d "${directory}" || -L "${directory}" ]]; then
		printf 'RPL call contract artifact staging is invalid\n' >&2; return 1
	fi
	for artifact in "${rpl_call_link_contract_artifacts[@]}"; do
		if [[ ! -f "${directory}/${artifact}" || -L "${directory}/${artifact}" || ! -s "${directory}/${artifact}" ]]; then
			printf 'RPL call contract artifact staging is incomplete\n' >&2; return 1
		fi
		mode=$(stat -c '%a' -- "${directory}/${artifact}")
		case "${artifact}:${mode}" in
			cex-trace-compare:755|rust-link-trace.jsonl:600|main.rpx:600|provider.rpl:600|SHA256SUMS:644) ;;
			*) printf 'RPL call contract artifact mode is invalid\n' >&2; return 1 ;;
		esac
	done
	[[ -x "${directory}/cex-trace-compare" ]] || { printf 'RPL call contract comparator artifact is not executable\n' >&2; return 1; }
	while IFS= read -r -d '' entry; do
		case "${entry}" in
			main.rpx|provider.rpl|rust-link-trace.jsonl|SHA256SUMS|cex-trace-compare) ;;
			"${rpl_call_artifacts_marker}") [[ "${directory}" == "${rpl_call_artifacts_dir}" ]] || { printf 'RPL call contract artifact staging contains unexpected entries\n' >&2; return 1; } ;;
			*) printf 'RPL call contract artifact staging contains unexpected entries\n' >&2; return 1 ;;
		esac
	done < <(find "${directory}" -mindepth 1 -maxdepth 1 -printf '%f\0')
	if [[ "$(wc -l < "${directory}/rust-link-trace.jsonl")" -ne 5 ]]; then
		printf 'RPL call link contract trace record count is invalid\n' >&2
		return 1
	fi
	if ! LC_ALL=C awk '
		function hex64(value) { return length(value) == 64 && value ~ /^[0-9a-f]+$/ }
		NF == 2 && hex64($1) && $2 == "main.rpx" { main++; next }
		NF == 2 && hex64($1) && $2 == "provider.rpl" { provider++; next }
		NF == 2 && hex64($1) && $2 == "rust-link-trace.jsonl" { link++; next }
		NF == 2 && hex64($1) && $2 == "cex-trace-compare" { comparator++; next }
		{ invalid = 1 }
		END { exit invalid || main != 1 || provider != 1 || link != 1 || comparator != 1 }
	' "${directory}/SHA256SUMS"; then
		printf 'RPL call contract checksum manifest is invalid\n' >&2; return 1
	fi
	(cd -- "${directory}" && sha256sum --check --status SHA256SUMS) || { printf 'RPL call contract checksum verification failed\n' >&2; return 1; }
}

ensure_clean_checkout() {
	local repository=$1
	local revision=$2
	local label=$3

	if ! git -C "${repository}" diff --quiet "${revision}" \
		|| ! git -C "${repository}" diff --cached --quiet "${revision}" \
		|| [[ -n "$(git -C "${repository}" status --porcelain=v1 --untracked-files=all)" ]]; then
		printf 'Refusing a dirty oracle checkout: %s\n' "${label}" >&2
		return 1
	fi
}

# Export one repository revision and then recursively export every gitlink in
# that revision.  git archive deliberately writes no .git metadata, configs,
# credential helpers, or untracked files into the Docker context.
export_repository() {
	local repository=$1
	local revision=$2
	local destination=$3
	local entry metadata path mode type object sub_repository sub_destination log_path
	if [[ "${repository}" == "${oracle_dir}" ]]; then
		log_path='.'
	else
		log_path="${repository#"${oracle_dir}"/}"
	fi

	if ! git -C "${repository}" cat-file -e "${revision}^{commit}" \
		|| ! ensure_clean_checkout "${repository}" "${revision}" "${log_path}"; then
		printf '%s status=failed\n' "${log_path}" >>"${export_log}"
		return 1
	fi
	mkdir -p "${destination}"
	git -C "${repository}" archive --format=tar "${revision}" | tar -x -C "${destination}"

	while IFS= read -r -d '' entry; do
		metadata=${entry%%$'\t'*}
		path=${entry#*$'\t'}
		read -r mode type object <<<"${metadata}"
		[[ "${mode}" == 160000 ]] || continue

		sub_repository="${repository}/${path}"
		sub_destination="${destination}/${path}"
		if [[ ! -d "${sub_repository}" ]] \
			|| [[ "$(git -C "${sub_repository}" rev-parse HEAD)" != "${object}" ]]; then
			printf '%s status=failed\n' "${log_path}/${path}" >>"${export_log}"
			printf 'Required submodule is not initialized at its pinned revision: %s\n' \
			"${log_path}/${path}" >&2
			return 1
		fi

		if ! export_repository "${sub_repository}" "${object}" "${sub_destination}"; then
			printf '%s status=failed\n' "${log_path}/${path}" >>"${export_log}"
			return 1
		fi
	done < <(git -C "${repository}" ls-tree -rz "${revision}")
	printf '%s status=ok\n' "${log_path}" >>"${export_log}"
}

# vcpkg manifest versioning resolves builtin-baseline through the vcpkg
# repository's Git object database. git archive intentionally omits that
# database, so supply the pinned vcpkg history and its version DB objects in
# the otherwise metadata-free Docker context. A bundle carries Git objects,
# not the host worktree's .git file, configuration, or credential helpers.
export_vcpkg_baseline_bundle() {
	local vcpkg_repository manifest_baseline bundle_bytes bundle_heads

	vcpkg_repository="${oracle_dir}/dependencies/vcpkg"
	manifest_baseline="$(
		git -C "${oracle_dir}" show "${oracle_commit}:vcpkg.json" \
			| sed -nE 's/^[[:space:]]*"builtin-baseline"[[:space:]]*:[[:space:]]*"([0-9a-f]{40})".*/\1/p'
	)"
	if [[ "${manifest_baseline}" != "${vcpkg_builtin_baseline}" ]]; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Fixed vcpkg baseline does not match the oracle manifest\n' >&2
		return 1
	fi
	vcpkg_pinned_commit="$(
		git -C "${oracle_dir}" ls-tree "${oracle_commit}" -- dependencies/vcpkg \
			| awk '$1 == "160000" && $2 == "commit" { print $3 }'
	)"
	if [[ ! "${vcpkg_pinned_commit}" =~ ^[0-9a-f]{40}$ ]] \
		|| [[ ! -d "${vcpkg_repository}" ]] \
		|| [[ "$(git -C "${vcpkg_repository}" rev-parse HEAD 2>/dev/null)" != "${vcpkg_pinned_commit}" ]] \
		|| ! git -C "${vcpkg_repository}" cat-file -e "${vcpkg_builtin_baseline}^{commit}" >/dev/null 2>&1 \
		|| ! git -C "${vcpkg_repository}" merge-base --is-ancestor \
			"${vcpkg_builtin_baseline}" "${vcpkg_pinned_commit}" >/dev/null 2>&1; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Pinned vcpkg history does not contain the required builtin-baseline\n' >&2
		return 1
	fi
	if ! vcpkg_sdl3_tree="$(
		git -C "${vcpkg_repository}" show "${vcpkg_pinned_commit}:versions/s-/sdl3.json" 2>/dev/null \
			| awk 'BEGIN { RS = "}" }
				/"git-tree"[[:space:]]*:/ \
				&& /"version"[[:space:]]*:[[:space:]]*"3\.4\.10"/ \
				&& /"port-version"[[:space:]]*:[[:space:]]*0/ {
					if (match($0, /"git-tree"[[:space:]]*:[[:space:]]*"[0-9a-f]{40}"/)) {
						value = substr($0, RSTART, RLENGTH)
						sub(/.*"git-tree"[[:space:]]*:[[:space:]]*"/, "", value)
						sub(/"$/, "", value)
						print value
						exit
					}
				}'
	)" || [[ ! "${vcpkg_sdl3_tree}" =~ ^[0-9a-f]{40}$ ]] \
		|| ! git -C "${vcpkg_repository}" cat-file -e "${vcpkg_sdl3_tree}^{tree}" >/dev/null 2>&1; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Pinned vcpkg version database does not provide the required SDL3 port tree\n' >&2
		return 1
	fi

	# git bundle creates a bundle from refs, not an arbitrary detached object.
	# Stage the Oracle-pinned history in a context-external bare repository so
	# neither the oracle worktree nor its refs are changed.
	bundle_repository="$(mktemp -d /tmp/cemu-extend-vcpkg-bundle.XXXXXX)"
	printf '%s:%s\n' "${oracle_commit}" "${vcpkg_pinned_commit}" > "${bundle_repository}/.cemu-vcpkg-bundle-repository"
	if ! git -C "${bundle_repository}" init --bare --quiet >/dev/null 2>&1 \
		|| ! git -C "${bundle_repository}" fetch --quiet --no-tags "${vcpkg_repository}" \
			"${vcpkg_pinned_commit}:${vcpkg_pinned_ref}" >/dev/null 2>&1 \
		|| [[ "$(git -C "${bundle_repository}" rev-parse "${vcpkg_pinned_ref}^{commit}" 2>/dev/null)" != "${vcpkg_pinned_commit}" ]]; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Could not stage the fixed vcpkg baseline bundle reference\n' >&2
		return 1
	fi
	if ! git -C "${bundle_repository}" bundle create \
		"${context_dir}/${vcpkg_baseline_bundle}" "${vcpkg_pinned_ref}" >/dev/null 2>&1; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Could not create the fixed vcpkg baseline bundle\n' >&2
		return 1
	fi
	if ! git -C "${vcpkg_repository}" bundle verify \
		"${context_dir}/${vcpkg_baseline_bundle}" >/dev/null 2>&1; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Fixed vcpkg baseline bundle verification failed\n' >&2
		return 1
	fi
	if ! bundle_heads="$(git bundle list-heads "${context_dir}/${vcpkg_baseline_bundle}" 2>/dev/null)" \
		|| [[ "${bundle_heads}" != "${vcpkg_pinned_commit} ${vcpkg_pinned_ref}" ]]; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Fixed vcpkg baseline bundle does not advertise exactly the requested reference\n' >&2
		return 1
	fi
	bundle_bytes="$(wc -c < "${context_dir}/${vcpkg_baseline_bundle}")"
	if (( bundle_bytes <= 0 || bundle_bytes > vcpkg_bundle_max_bytes )); then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Pinned vcpkg history bundle has an invalid size\n' >&2
		return 1
	fi
	# The bundle no longer needs the bare repository. Remove all local-fetch
	# metadata before Docker starts; a cleanup failure is a hard failure.
	if ! cleanup_bundle_repository; then
		printf '%s status=failed\n' "${vcpkg_baseline_bundle}" >>"${export_log}"
		printf 'Could not discard the temporary vcpkg bundle repository\n' >&2
		return 1
	fi
	printf '%s pinned=%s baseline=%s sdl3-tree=%s bytes=%s status=ok\n' \
		"${vcpkg_baseline_bundle}" "${vcpkg_pinned_commit}" "${vcpkg_builtin_baseline}" \
		"${vcpkg_sdl3_tree}" "${bundle_bytes}" >>"${export_log}"
}

# The frozen Dockerfile is exported verbatim and remains the source of truth.
# Build only the context-local derivative so all targets initialize an empty
# vcpkg Git database and fetch the fixed bundle before bootstrap/versioning.
create_oracle_dockerfile() {
	local source_dockerfile="${context_dir}/Dockerfile"
	local generated_dockerfile="${context_dir}/${oracle_dockerfile}"

	if ! awk -v pinned="${vcpkg_pinned_commit}" -v baseline="${vcpkg_builtin_baseline}" \
		-v sdl3_tree="${vcpkg_sdl3_tree}" '
		/&& bash \.\/dependencies\/vcpkg\/bootstrap-vcpkg\.sh -disableMetrics/ {
			print "    && test ! -e dependencies/vcpkg/.git " sprintf("%c", 92)
			print "    && git -C dependencies/vcpkg init --quiet " sprintf("%c", 92)
			print "    && git -C dependencies/vcpkg fetch --quiet ../../.cemu-vcpkg-baseline.bundle refs/tags/cemu-vcpkg-pinned:refs/tags/cemu-vcpkg-pinned " sprintf("%c", 92)
			print "    && git -C dependencies/vcpkg cat-file -e " pinned "^{commit} " sprintf("%c", 92)
			print "    && git -C dependencies/vcpkg cat-file -e " baseline "^{commit} " sprintf("%c", 92)
			print "    && git -C dependencies/vcpkg cat-file -e " sdl3_tree "^{tree} " sprintf("%c", 92)
			replacements++
		}
		{ print }
		END { exit replacements == 3 ? 0 : 1 }
	' "${source_dockerfile}" >"${generated_dockerfile}"; then
		rm -f -- "${generated_dockerfile}"
		printf '%s status=failed\n' "${oracle_dockerfile}" >>"${export_log}"
		printf 'Could not create the vcpkg-enabled oracle Dockerfile\n' >&2
		return 1
	fi
	printf '%s vcpkg-baseline=%s status=ok\n' \
		"${oracle_dockerfile}" "${vcpkg_builtin_baseline}" >>"${export_log}"
}

if [[ ! -d "${oracle_dir}" ]]; then
	printf 'C++ oracle worktree does not exist: .\n' >&2
	exit 1
fi

oracle_dir="$(cd -- "${oracle_dir}" && pwd)"
if [[ "$(git -C "${oracle_dir}" rev-parse --show-toplevel)" != "${oracle_dir}" ]]; then
	printf 'C++ oracle path must be the worktree root: .\n' >&2
	exit 1
fi
if [[ "$(git -C "${oracle_dir}" rev-parse HEAD)" != "${oracle_commit}" ]]; then
	printf 'C++ oracle HEAD at . must be %s (revision mismatch)\n' "${oracle_commit}" >&2
	exit 1
fi

context_dir="$(mktemp -d /tmp/cemu-extend-cpp-oracle.XXXXXX)"
export_log="$(mktemp /tmp/cemu-extend-cpp-oracle-export.XXXXXX.log)"
export_repository "${oracle_dir}" "${oracle_commit}" "${context_dir}"
export_vcpkg_baseline_bundle
create_oracle_dockerfile

if [[ "${build_kind}" == trace ]]; then
	# Generate the Rust fixture through a BuildKit local export; docker run and
	# host cargo are intentionally not part of this path.
	trace_artifacts_dir="$(mktemp -d /tmp/cemu-extend-rpx-artifacts.XXXXXX)"
	printf '%s\n' 'cemu-extend-rpx-oracle-artifacts-v1' \
		> "${trace_artifacts_dir}/${trace_artifacts_marker}"
	trap exit_after_trace_cleanup EXIT
	rust_artifact_build_log="$(mktemp /tmp/cemu-extend-rpx-artifacts-build.XXXXXX.log)"
	(docker build --progress=plain --file "${project_dir}/Dockerfile.rust" \
		--target rust-rpx-contract-artifacts \
		--build-arg "RUST_VERSION=${CEMU_RUST_VERSION:-1.97.1}" \
		--build-arg "CARGO_DENY_VERSION=${CEMU_CARGO_DENY_VERSION:-0.20.2}" \
		--build-arg "CARGO_ABOUT_VERSION=${CEMU_CARGO_ABOUT_VERSION:-0.9.2}" \
		--output "type=local,dest=${trace_artifacts_dir}" "${project_dir}" \
		>"${rust_artifact_build_log}" 2>&1) &
	rust_artifact_docker_pid=$!
	rust_artifact_status=0
	rust_artifact_timed_out=0
	rust_artifact_timeout_seconds=$((10#${build_timeout_minutes} * 60))
	if (( rust_artifact_timeout_seconds == 0 )); then
		wait "${rust_artifact_docker_pid}" || rust_artifact_status=$?
	else
		rust_artifact_started_at=$(date +%s)
		while kill -0 "${rust_artifact_docker_pid}" 2>/dev/null; do
			if (( $(date +%s) - rust_artifact_started_at >= rust_artifact_timeout_seconds )); then
				rust_artifact_timed_out=1
				kill -TERM "${rust_artifact_docker_pid}" 2>/dev/null || true
				for ((i = 0; i < 10; i++)); do
					kill -0 "${rust_artifact_docker_pid}" 2>/dev/null || break
					sleep 1
				done
				kill -KILL "${rust_artifact_docker_pid}" 2>/dev/null || true
				wait "${rust_artifact_docker_pid}" 2>/dev/null || true
				rust_artifact_status=124
				break
			fi
			sleep 1
		done
		if (( rust_artifact_timed_out == 0 )); then
			wait "${rust_artifact_docker_pid}" || rust_artifact_status=$?
		fi
	fi
	if (( rust_artifact_status != 0 )); then
		if (( rust_artifact_timed_out != 0 )); then
			printf 'Rust RPX artifact Docker build timed out after %s minute(s); log=%s\n' \
				"${build_timeout_minutes}" "${rust_artifact_build_log}" >&2
		else
			printf 'Rust RPX artifact Docker build failed (exit %s); log=%s\n' \
				"${rust_artifact_status}" "${rust_artifact_build_log}" >&2
		fi
		exit "${rust_artifact_status}"
	fi
	validate_rpx_contract_artifacts "${trace_artifacts_dir}"
	mkdir -p "${context_dir}/oracle-rpx" "${context_dir}/oracle-adapter"
	cp -a "${trace_artifacts_dir}/fixture.rpx" "${trace_artifacts_dir}/rust-trace.jsonl" \
		"${trace_artifacts_dir}/SHA256SUMS" "${trace_artifacts_dir}/cex-trace-compare" \
		"${context_dir}/oracle-rpx/"
	adapter_source_dir="${project_dir}/compat/cpp-oracle"
	if [[ ! -d "${adapter_source_dir}" || -L "${adapter_source_dir}" ]]; then
		printf 'Missing tracked C++ RPX adapter sources\n' >&2
		exit 1
	fi
	readonly -a adapter_sources=(CMakeLists.txt rpx_oracle_trace.cpp rpl_link_oracle_trace.cpp rpl_call_oracle_trace.cpp)
	while IFS= read -r -d '' adapter_entry; do
		adapter_name=${adapter_entry#"${adapter_source_dir}"/}
		case "${adapter_name}" in
			CMakeLists.txt|rpx_oracle_trace.cpp|rpl_link_oracle_trace.cpp|rpl_call_oracle_trace.cpp) ;;
			*)
				printf 'C++ RPX adapter directory contains an unexpected entry\n' >&2
				exit 1
				;;
		esac
	done < <(find "${adapter_source_dir}" -mindepth 1 -maxdepth 1 -printf '%p\0')
	for adapter_name in "${adapter_sources[@]}"; do
		adapter_path="${adapter_source_dir}/${adapter_name}"
		if [[ ! -f "${adapter_path}" || -L "${adapter_path}" ]]; then
			printf 'C++ RPX adapter source must be a regular non-symlink file\n' >&2
			exit 1
		fi
		cp -- "${adapter_path}" "${context_dir}/oracle-adapter/${adapter_name}"
	done
	if ! grep -q 'cpp_rpx_oracle_trace' "${context_dir}/oracle-adapter/CMakeLists.txt"; then
		printf 'C++ RPX adapter CMake target is missing\n' >&2
		exit 1
	fi
	cat >> "${context_dir}/CMakeLists.txt" <<'EOF'

add_subdirectory(oracle-adapter EXCLUDE_FROM_ALL)
EOF
cat >> "${context_dir}/${oracle_dockerfile}" <<EOF

FROM cemu-extend-base AS cpp-rpx-oracle-trace
ARG SOURCE_FINGERPRINT
WORKDIR /workspace/CemuExtend
RUN --mount=type=bind,source=.,target=/workspace/CemuExtend,rw \
    --mount=type=cache,id=cemu-extend-vcpkg,target=/root/.cache/vcpkg/archives,sharing=locked \
    --mount=type=cache,id=cemu-extend-vcpkg-downloads,target=/root/.cache/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=cemu-extend-cmake-rpx-oracle,target=/workspace/CemuExtend/build/docker-rpx-oracle,sharing=locked \
    test -n "\${SOURCE_FINGERPRINT}" \
    && test ! -e dependencies/vcpkg/.git \
    && git -C dependencies/vcpkg init --quiet \
    && git -C dependencies/vcpkg fetch --quiet ../../.cemu-vcpkg-baseline.bundle refs/tags/cemu-vcpkg-pinned:refs/tags/cemu-vcpkg-pinned \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_pinned_commit}^{commit} \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_builtin_baseline}^{commit} \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_sdl3_tree}^{tree} \
    && bash ./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cmake -S . -B build/docker-rpx-oracle -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_VCPKG=ON -DCEMU_FRONTEND=headless -DALLOW_PORTABLE=OFF \
       -DBUILD_TESTING=ON \
    && cmake --build build/docker-rpx-oracle --target cpp_rpx_oracle_trace --parallel \
    && adapter_bin="\$(find build/docker-rpx-oracle -type f -name cpp_rpx_oracle_trace -perm -111 -print -quit)" \
    && test -n "\${adapter_bin}" \
    && "\${adapter_bin}" oracle-rpx/fixture.rpx > oracle-rpx/cpp-trace.jsonl \
    && oracle-rpx/cex-trace-compare \
       --expected oracle-rpx/rust-trace.jsonl \
       --actual oracle-rpx/cpp-trace.jsonl
EOF
elif [[ "${build_kind}" == rpl-link-trace ]]; then
	# Keep the fixed-ab0b context flow: only the BuildKit-exported Rust contract
	# and the four adapter files are introduced after the oracle tree export.
	rpl_link_artifacts_dir="$(mktemp -d /tmp/cemu-extend-rpl-link-artifacts.XXXXXX)"
	printf '%s\n' 'cemu-extend-rpl-link-artifacts-v1' \
		> "${rpl_link_artifacts_dir}/${rpl_link_artifacts_marker}"
	trap exit_after_trace_cleanup EXIT
	rpl_link_artifact_build_log="$(mktemp /tmp/cemu-extend-rpl-link-artifacts-build.XXXXXX.log)"
	(docker build --progress=plain --file "${project_dir}/Dockerfile.rust" \
		--target rust-rpl-link-contract-artifacts \
		--build-arg "RUST_VERSION=${CEMU_RUST_VERSION:-1.97.1}" \
		--build-arg "CARGO_DENY_VERSION=${CEMU_CARGO_DENY_VERSION:-0.20.2}" \
		--build-arg "CARGO_ABOUT_VERSION=${CEMU_CARGO_ABOUT_VERSION:-0.9.2}" \
		--output "type=local,dest=${rpl_link_artifacts_dir}" "${project_dir}" \
		>"${rpl_link_artifact_build_log}" 2>&1) &
	rust_artifact_docker_pid=$!
	rust_artifact_status=0
	rust_artifact_timed_out=0
	rust_artifact_timeout_seconds=$((10#${build_timeout_minutes} * 60))
	if (( rust_artifact_timeout_seconds == 0 )); then
		wait "${rust_artifact_docker_pid}" || rust_artifact_status=$?
	else
		rust_artifact_started_at=$(date +%s)
		while kill -0 "${rust_artifact_docker_pid}" 2>/dev/null; do
			if (( $(date +%s) - rust_artifact_started_at >= rust_artifact_timeout_seconds )); then
				rust_artifact_timed_out=1
				kill -TERM "${rust_artifact_docker_pid}" 2>/dev/null || true
				for ((i = 0; i < 10; i++)); do
					kill -0 "${rust_artifact_docker_pid}" 2>/dev/null || break
					sleep 1
				done
				kill -KILL "${rust_artifact_docker_pid}" 2>/dev/null || true
				wait "${rust_artifact_docker_pid}" 2>/dev/null || true
				rust_artifact_status=124
				break
			fi
			sleep 1
		done
		if (( rust_artifact_timed_out == 0 )); then
			wait "${rust_artifact_docker_pid}" || rust_artifact_status=$?
		fi
	fi
	if (( rust_artifact_status != 0 )); then
		if (( rust_artifact_timed_out != 0 )); then
			printf 'Rust RPL link artifact Docker build timed out after %s minute(s); log=%s\n' \
				"${build_timeout_minutes}" "${rpl_link_artifact_build_log}" >&2
		else
			printf 'Rust RPL link artifact Docker build failed (exit %s); log=%s\n' \
				"${rust_artifact_status}" "${rpl_link_artifact_build_log}" >&2
		fi
		exit "${rust_artifact_status}"
	fi
	validate_rpl_link_contract_artifacts "${rpl_link_artifacts_dir}"
	mkdir -p "${context_dir}/oracle-rpx" "${context_dir}/oracle-adapter"
	cp -a "${rpl_link_artifacts_dir}/main.rpx" "${rpl_link_artifacts_dir}/provider.rpl" \
		"${rpl_link_artifacts_dir}/rust-trace.jsonl" "${rpl_link_artifacts_dir}/SHA256SUMS" \
		"${rpl_link_artifacts_dir}/cex-trace-compare" "${context_dir}/oracle-rpx/"
	adapter_source_dir="${project_dir}/compat/cpp-oracle"
	if [[ ! -d "${adapter_source_dir}" || -L "${adapter_source_dir}" ]]; then
		printf 'Missing tracked C++ RPL link adapter sources\n' >&2
		exit 1
	fi
	readonly -a adapter_sources=(CMakeLists.txt rpx_oracle_trace.cpp rpl_link_oracle_trace.cpp rpl_call_oracle_trace.cpp)
	while IFS= read -r -d '' adapter_entry; do
		adapter_name=${adapter_entry#"${adapter_source_dir}"/}
		case "${adapter_name}" in
			CMakeLists.txt|rpx_oracle_trace.cpp|rpl_link_oracle_trace.cpp|rpl_call_oracle_trace.cpp) ;;
			*) printf 'C++ RPL link adapter directory contains an unexpected entry\n' >&2; exit 1 ;;
		esac
	done < <(find "${adapter_source_dir}" -mindepth 1 -maxdepth 1 -printf '%p\0')
	for adapter_name in "${adapter_sources[@]}"; do
		adapter_path="${adapter_source_dir}/${adapter_name}"
		if [[ ! -f "${adapter_path}" || -L "${adapter_path}" ]]; then
			printf 'C++ RPL link adapter source must be a regular non-symlink file\n' >&2
			exit 1
		fi
		cp -- "${adapter_path}" "${context_dir}/oracle-adapter/${adapter_name}"
	done
	if ! grep -q 'cpp_rpl_link_oracle_trace' "${context_dir}/oracle-adapter/CMakeLists.txt"; then
		printf 'C++ RPL link adapter CMake target is missing\n' >&2
		exit 1
	fi
	cat >> "${context_dir}/CMakeLists.txt" <<'EOF'

add_subdirectory(oracle-adapter EXCLUDE_FROM_ALL)
EOF
	cat >> "${context_dir}/${oracle_dockerfile}" <<EOF

FROM cemu-extend-base AS cpp-rpl-link-oracle-trace
ARG SOURCE_FINGERPRINT
WORKDIR /workspace/CemuExtend
RUN --mount=type=bind,source=.,target=/workspace/CemuExtend,rw \
    --mount=type=cache,id=cemu-extend-vcpkg,target=/root/.cache/vcpkg/archives,sharing=locked \
    --mount=type=cache,id=cemu-extend-vcpkg-downloads,target=/root/.cache/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=cemu-extend-cmake-rpl-link-oracle,target=/workspace/CemuExtend/build/docker-rpl-link-oracle,sharing=locked \
    test -n "\${SOURCE_FINGERPRINT}" \
    && test ! -e dependencies/vcpkg/.git \
    && git -C dependencies/vcpkg init --quiet \
    && git -C dependencies/vcpkg fetch --quiet ../../.cemu-vcpkg-baseline.bundle refs/tags/cemu-vcpkg-pinned:refs/tags/cemu-vcpkg-pinned \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_pinned_commit}^{commit} \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_builtin_baseline}^{commit} \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_sdl3_tree}^{tree} \
    && bash ./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cmake -S . -B build/docker-rpl-link-oracle -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_VCPKG=ON -DCEMU_FRONTEND=headless -DALLOW_PORTABLE=OFF \
       -DBUILD_TESTING=ON \
    && cmake --build build/docker-rpl-link-oracle --target cpp_rpl_link_oracle_trace --parallel \
    && adapter_bin="\$(find build/docker-rpl-link-oracle -type f -name cpp_rpl_link_oracle_trace -perm -111 -print -quit)" \
    && test -n "\${adapter_bin}" \
    && "\${adapter_bin}" oracle-rpx/main.rpx oracle-rpx/provider.rpl > oracle-rpx/cpp-trace.jsonl \
    && oracle-rpx/cex-trace-compare \
       --expected oracle-rpx/rust-trace.jsonl \
       --actual oracle-rpx/cpp-trace.jsonl
EOF
elif [[ "${build_kind}" == rpl-call-trace ]]; then
	rpl_call_artifacts_dir="$(mktemp -d /tmp/cemu-extend-rpl-call-artifacts.XXXXXX)"
	printf '%s\n' 'cemu-extend-rpl-call-artifacts-v1' > "${rpl_call_artifacts_dir}/${rpl_call_artifacts_marker}"
	trap exit_after_trace_cleanup EXIT
	rpl_call_artifact_build_log="$(mktemp /tmp/cemu-extend-rpl-call-artifacts-build.XXXXXX.log)"
	(docker build --progress=plain --file "${project_dir}/Dockerfile.rust" \
		--target rust-rpl-call-link-contract-artifacts \
		--build-arg "RUST_VERSION=${CEMU_RUST_VERSION:-1.97.1}" \
		--build-arg "CARGO_DENY_VERSION=${CEMU_CARGO_DENY_VERSION:-0.20.2}" \
		--build-arg "CARGO_ABOUT_VERSION=${CEMU_CARGO_ABOUT_VERSION:-0.9.2}" \
		--output "type=local,dest=${rpl_call_artifacts_dir}" "${project_dir}" \
		>"${rpl_call_artifact_build_log}" 2>&1) &
	rust_artifact_docker_pid=$!
	rust_artifact_status=0
	rust_artifact_timed_out=0
	rust_artifact_timeout_seconds=$((10#${build_timeout_minutes} * 60))
	if (( rust_artifact_timeout_seconds == 0 )); then
		wait "${rust_artifact_docker_pid}" || rust_artifact_status=$?
	else
		rust_artifact_started_at=$(date +%s)
		while kill -0 "${rust_artifact_docker_pid}" 2>/dev/null; do
			if (( $(date +%s) - rust_artifact_started_at >= rust_artifact_timeout_seconds )); then
				rust_artifact_timed_out=1
				kill -TERM "${rust_artifact_docker_pid}" 2>/dev/null || true
				for ((i = 0; i < 10; i++)); do
					kill -0 "${rust_artifact_docker_pid}" 2>/dev/null || break
					sleep 1
				done
				kill -KILL "${rust_artifact_docker_pid}" 2>/dev/null || true
				wait "${rust_artifact_docker_pid}" 2>/dev/null || true
				rust_artifact_status=124
				break
			fi
			sleep 1
		done
		if (( rust_artifact_timed_out == 0 )); then wait "${rust_artifact_docker_pid}" || rust_artifact_status=$?; fi
	fi
	if (( rust_artifact_status != 0 )); then
		if (( rust_artifact_timed_out != 0 )); then
			printf 'Rust RPL call artifact Docker build timed out after %s minute(s); log=%s\n' "${build_timeout_minutes}" "${rpl_call_artifact_build_log}" >&2
		else
			printf 'Rust RPL call artifact Docker build failed (exit %s); log=%s\n' "${rust_artifact_status}" "${rpl_call_artifact_build_log}" >&2
		fi
		exit "${rust_artifact_status}"
	fi
	validate_rpl_call_link_contract_artifacts "${rpl_call_artifacts_dir}"
	mkdir -p "${context_dir}/oracle-rpx" "${context_dir}/oracle-adapter"
	cp -a "${rpl_call_artifacts_dir}/main.rpx" "${rpl_call_artifacts_dir}/provider.rpl" \
		"${rpl_call_artifacts_dir}/rust-link-trace.jsonl" \
		"${rpl_call_artifacts_dir}/SHA256SUMS" "${rpl_call_artifacts_dir}/cex-trace-compare" "${context_dir}/oracle-rpx/"
	adapter_source_dir="${project_dir}/compat/cpp-oracle"
	if [[ ! -d "${adapter_source_dir}" || -L "${adapter_source_dir}" ]]; then
		printf 'Missing tracked C++ RPL call adapter sources\n' >&2; exit 1
	fi
	readonly -a adapter_sources=(CMakeLists.txt rpx_oracle_trace.cpp rpl_link_oracle_trace.cpp rpl_call_oracle_trace.cpp)
	while IFS= read -r -d '' adapter_entry; do
		adapter_name=${adapter_entry#"${adapter_source_dir}"/}
		case "${adapter_name}" in
			CMakeLists.txt|rpx_oracle_trace.cpp|rpl_link_oracle_trace.cpp|rpl_call_oracle_trace.cpp) ;;
			*) printf 'C++ RPL call adapter directory contains an unexpected entry\n' >&2; exit 1 ;;
		esac
	done < <(find "${adapter_source_dir}" -mindepth 1 -maxdepth 1 -printf '%p\0')
	for adapter_name in "${adapter_sources[@]}"; do
		adapter_path="${adapter_source_dir}/${adapter_name}"
		if [[ ! -f "${adapter_path}" || -L "${adapter_path}" ]]; then
			printf 'C++ RPL call adapter source must be a regular non-symlink file\n' >&2; exit 1
		fi
		cp -- "${adapter_path}" "${context_dir}/oracle-adapter/${adapter_name}"
	done
	if ! grep -q 'cpp_rpl_call_oracle_trace' "${context_dir}/oracle-adapter/CMakeLists.txt"; then
		printf 'C++ RPL call adapter CMake target is missing\n' >&2; exit 1
	fi
	cat >> "${context_dir}/CMakeLists.txt" <<'EOF'

add_subdirectory(oracle-adapter EXCLUDE_FROM_ALL)
EOF
	cat >> "${context_dir}/${oracle_dockerfile}" <<EOF

FROM cemu-extend-base AS cpp-rpl-call-oracle-trace
ARG SOURCE_FINGERPRINT
WORKDIR /workspace/CemuExtend
RUN --mount=type=bind,source=.,target=/workspace/CemuExtend,rw \
    --mount=type=cache,id=cemu-extend-vcpkg,target=/root/.cache/vcpkg/archives,sharing=locked \
    --mount=type=cache,id=cemu-extend-vcpkg-downloads,target=/root/.cache/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=cemu-extend-cmake-rpl-call-oracle,target=/workspace/CemuExtend/build/docker-rpl-call-oracle,sharing=locked \
    test -n "\${SOURCE_FINGERPRINT}" \
    && test ! -e dependencies/vcpkg/.git \
    && git -C dependencies/vcpkg init --quiet \
    && git -C dependencies/vcpkg fetch --quiet ../../.cemu-vcpkg-baseline.bundle refs/tags/cemu-vcpkg-pinned:refs/tags/cemu-vcpkg-pinned \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_pinned_commit}^{commit} \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_builtin_baseline}^{commit} \
    && git -C dependencies/vcpkg cat-file -e ${vcpkg_sdl3_tree}^{tree} \
    && bash ./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cmake -S . -B build/docker-rpl-call-oracle -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_VCPKG=ON -DCEMU_FRONTEND=headless -DALLOW_PORTABLE=OFF -DBUILD_TESTING=ON \
    && cmake --build build/docker-rpl-call-oracle --target cpp_rpl_call_oracle_trace --parallel \
    && adapter_bin="\$(find build/docker-rpl-call-oracle -type f -name cpp_rpl_call_oracle_trace -perm -111 -print -quit)" \
    && test -n "\${adapter_bin}" \
    && "\${adapter_bin}" oracle-rpx/main.rpx oracle-rpx/provider.rpl > oracle-rpx/cpp-link-trace.jsonl \
    && oracle-rpx/cex-trace-compare \
       --expected oracle-rpx/rust-link-trace.jsonl \
       --actual oracle-rpx/cpp-link-trace.jsonl
EOF
fi

# The diagnostic log must never become part of the Docker context.  Inspect
# paths only (not file contents), rejecting any accidental export.log entry.
while IFS= read -r context_path; do
	case "${context_path}" in
		export.log)
			printf 'Refusing Docker context containing export.log\n' >&2
			exit 1
			;;
		*"${project_dir}"*|*"${oracle_dir}"*)
			printf 'Refusing Docker context containing a host workspace path\n' >&2
			exit 1
			;;
	esac
done < <(find "${context_dir}" -type f -printf '%P\n')
printf '%s\n' "${oracle_commit}" > "${context_dir}/.cemu-oracle-context"
context_file_count="$(find "${context_dir}" -type f | wc -l)"
context_du_summary="$(du -sh "${context_dir}" | cut -f1)"
printf 'context-files=%s context-size=%s\n' "${context_file_count}" "${context_du_summary}" >>"${export_log}"
printf 'Docker context export complete: files=%s size=%s\n' "${context_file_count}" "${context_du_summary}"

# The context is made solely from commit objects, so this commit/submodule
# fingerprint is also a complete cache key without reading host working files.
source_fingerprint="$(
	{
		printf '%s\n' "${oracle_commit}"
		git -C "${oracle_dir}" submodule status --recursive
		if [[ "${build_kind}" == trace ]]; then
			for artifact in "${rpx_contract_artifacts[@]}"; do
				printf 'rpx-artifact:%s ' "${artifact}"
				sha256sum "${context_dir}/oracle-rpx/${artifact}" | awk '{print $1}'
			done
			for adapter_name in CMakeLists.txt rpx_oracle_trace.cpp rpl_link_oracle_trace.cpp rpl_call_oracle_trace.cpp; do
				printf 'rpx-adapter:%s ' "${adapter_name}"
				sha256sum "${context_dir}/oracle-adapter/${adapter_name}" | awk '{print $1}'
			done
		elif [[ "${build_kind}" == rpl-link-trace ]]; then
			for artifact in "${rpl_link_contract_artifacts[@]}"; do
				printf 'rpl-link-artifact:%s ' "${artifact}"
				sha256sum "${context_dir}/oracle-rpx/${artifact}" | awk '{print $1}'
			done
			for adapter_name in CMakeLists.txt rpx_oracle_trace.cpp rpl_link_oracle_trace.cpp rpl_call_oracle_trace.cpp; do
				printf 'rpl-link-adapter:%s ' "${adapter_name}"
				sha256sum "${context_dir}/oracle-adapter/${adapter_name}" | awk '{print $1}'
			done
		elif [[ "${build_kind}" == rpl-call-trace ]]; then
			for artifact in "${rpl_call_link_contract_artifacts[@]}"; do
				printf 'rpl-call-artifact:%s ' "${artifact}"
				sha256sum "${context_dir}/oracle-rpx/${artifact}" | awk '{print $1}'
				printf 'mode:%s:%s\n' "${artifact}" "$(stat -c '%a' -- "${context_dir}/oracle-rpx/${artifact}")"
			done
			for adapter_name in CMakeLists.txt rpx_oracle_trace.cpp rpl_link_oracle_trace.cpp rpl_call_oracle_trace.cpp; do
				printf 'rpl-call-adapter:%s ' "${adapter_name}"
				sha256sum "${context_dir}/oracle-adapter/${adapter_name}" | awk '{print $1}'
			done
		fi
	} | sha256sum | cut -d' ' -f1
)"

build_log="$(mktemp /tmp/cemu-extend-cpp-oracle-build.XXXXXX.log)"
docker_build_args=(build --progress=plain \
	--file "${context_dir}/${oracle_dockerfile}" \
	--target "${docker_target}" \
	--build-arg "GIT_HASH=${oracle_commit:0:7}" \
	--build-arg "CEMU_EXTEND_COMMIT_HASH=${oracle_commit}" \
	--build-arg "SOURCE_FINGERPRINT=${source_fingerprint}" \
	--build-arg "CEMU_FRONTEND=${CEMU_FRONTEND:-cef}" \
	--build-arg "CEMU_OVERLAY_BACKEND=${CEMU_OVERLAY_BACKEND:-}" \
	--build-arg "CLEAN_BUILD=${CEMU_CLEAN_BUILD:-0}" \
	--build-arg "ENABLE_WXWIDGETS=${CEMU_ENABLE_WXWIDGETS:-ON}" \
	--tag "${CEMU_CPP_ORACLE_DOCKER_IMAGE:-cemu-extend-cpp-oracle:${build_kind}}" \
	"${context_dir}")

(docker "${docker_build_args[@]}" >"${build_log}" 2>&1) &
docker_pid=$!
build_timeout_seconds=$((10#${build_timeout_minutes} * 60))
build_status=0
timed_out=0
if (( build_timeout_seconds == 0 )); then
	wait "${docker_pid}" || build_status=$?
else
	started_at=$(date +%s)
	while kill -0 "${docker_pid}" 2>/dev/null; do
		if (( $(date +%s) - started_at >= build_timeout_seconds )); then
			timed_out=1
			kill -TERM "${docker_pid}" 2>/dev/null || true
			for ((i = 0; i < 10; i++)); do
				kill -0 "${docker_pid}" 2>/dev/null || break
				sleep 1
			done
			kill -KILL "${docker_pid}" 2>/dev/null || true
			wait "${docker_pid}" 2>/dev/null || true
			build_status=124
			break
		fi
		sleep 1
	done
	if (( timed_out == 0 )); then
		wait "${docker_pid}" || build_status=$?
	fi
fi

if (( build_status != 0 )); then
	if (( timed_out != 0 )); then
		printf 'Docker build timed out after %s minute(s); context=%s log=%s\n' "${build_timeout_minutes}" "${context_dir}" "${build_log}" >&2
	else
		printf 'Docker build failed (exit %s); context=%s log=%s\n' "${build_status}" "${context_dir}" "${build_log}" >&2
	fi
	exit "${build_status}"
fi

build_completed=1
printf 'Docker C++ oracle target built: %s (%s)\n' "${docker_target}" "${oracle_commit}"
