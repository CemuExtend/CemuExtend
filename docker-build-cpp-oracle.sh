#!/usr/bin/env bash
set -euo pipefail

# Build the frozen C++ oracle from a Docker context that contains only the
# tracked tree and the exact, initialized submodule revisions.  Worktree
# submodules use .git *files* that point back into the host's common gitdir;
# passing the worktree itself to Docker therefore cannot work reliably.

readonly oracle_commit="ab0b772029f0a5cd57c194cf338003fe8eae8ab8"
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
oracle_dir="${CEMU_CPP_ORACLE_DIR:-${project_dir}/../CemuExtend-cpp-oracle}"
build_kind="${1:-build}"
keep_context="${CEMU_CPP_ORACLE_KEEP_CONTEXT:-0}"
build_timeout_minutes="${CEMU_CPP_ORACLE_BUILD_TIMEOUT_MINUTES:-30}"
context_dir=""
build_log=""
export_log=""
build_completed=0

usage() {
	printf 'Usage: %s [base|dev|build|win]\n' "${0##*/}" >&2
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

cleanup() {
	local status=$?

	# Preserve a failed context for inspection.  A successful context is removed
	# only when it is the marker-bearing directory made by mktemp below.  Set
	# CEMU_CPP_ORACLE_KEEP_CONTEXT=1 to retain a successful context as well.
	if [[ -z "${context_dir}" || ! -d "${context_dir}" ]]; then
		[[ -z "${export_log}" ]] || printf 'Oracle export log retained: %s\n' "${export_log}" >&2
		[[ -z "${build_log}" || "${build_completed}" -eq 1 ]] || printf 'Docker build log retained: %s\n' "${build_log}" >&2
		return "${status}"
	fi

	if [[ "${status}" -ne 0 || "${build_completed}" -ne 1 || "${keep_context}" == 1 ]]; then
		printf 'Docker context retained for recovery: %s\n' "${context_dir}" >&2
		if [[ "${status}" -ne 0 || "${build_completed}" -ne 1 ]]; then
			[[ -z "${export_log}" ]] || printf 'Oracle export log retained: %s\n' "${export_log}" >&2
			[[ -z "${build_log}" ]] || printf 'Docker build log retained: %s\n' "${build_log}" >&2
		else
			[[ -z "${export_log}" ]] || rm -f -- "${export_log}"
			[[ -z "${build_log}" ]] || rm -f -- "${build_log}"
		fi
		return "${status}"
	fi

	if [[ -f "${context_dir}/.cemu-oracle-context" \
		&& "$(<"${context_dir}/.cemu-oracle-context")" == "${oracle_commit}" \
		&& "${context_dir}" == /tmp/cemu-extend-cpp-oracle.* ]]; then
		rm -rf -- "${context_dir}"
		[[ -z "${export_log}" ]] || rm -f -- "${export_log}"
		[[ -z "${build_log}" ]] || rm -f -- "${build_log}"
	else
		printf 'Refusing to remove unexpected Docker context: %s\n' "${context_dir}" >&2
		return 1
	fi
	return "${status}"
}

trap cleanup EXIT

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
	} | sha256sum | cut -d' ' -f1
)"

build_log="$(mktemp /tmp/cemu-extend-cpp-oracle-build.XXXXXX.log)"
docker_build_args=(build --progress=plain \
	--file "${context_dir}/Dockerfile" \
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
