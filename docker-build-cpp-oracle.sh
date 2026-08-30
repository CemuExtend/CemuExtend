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
context_dir=""
build_completed=0

usage() {
	printf 'Usage: %s [dev|build|win]\n' "${0##*/}" >&2
	printf 'Builds the fixed C++ oracle revision %s using Docker only.\n' "${oracle_commit}" >&2
}

case "${build_kind}" in
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
		return "${status}"
	fi

	if [[ "${status}" -ne 0 || "${build_completed}" -ne 1 || "${keep_context}" == 1 ]]; then
		printf 'Docker context retained for recovery: %s\n' "${context_dir}" >&2
		return "${status}"
	fi

	if [[ -f "${context_dir}/.cemu-oracle-context" \
		&& "$(<"${context_dir}/.cemu-oracle-context")" == "${oracle_commit}" \
		&& "${context_dir}" == /tmp/cemu-extend-cpp-oracle.* ]]; then
		rm -rf -- "${context_dir}"
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
		exit 1
	fi
}

# Export one repository revision and then recursively export every gitlink in
# that revision.  git archive deliberately writes no .git metadata, configs,
# credential helpers, or untracked files into the Docker context.
export_repository() {
	local repository=$1
	local revision=$2
	local destination=$3
	local entry metadata path mode type object sub_repository sub_destination

	git -C "${repository}" cat-file -e "${revision}^{commit}"
	ensure_clean_checkout "${repository}" "${revision}" "${repository}"
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
			printf 'Required submodule is not initialized at its pinned revision: %s\n' \
			"${sub_repository}" >&2
			exit 1
		fi

		export_repository "${sub_repository}" "${object}" "${sub_destination}"
	done < <(git -C "${repository}" ls-tree -rz "${revision}")
}

if [[ ! -d "${oracle_dir}" ]]; then
	printf 'C++ oracle worktree does not exist: %s\n' "${oracle_dir}" >&2
	exit 1
fi

oracle_dir="$(cd -- "${oracle_dir}" && pwd)"
if [[ "$(git -C "${oracle_dir}" rev-parse --show-toplevel)" != "${oracle_dir}" ]]; then
	printf 'C++ oracle path must be the worktree root: %s\n' "${oracle_dir}" >&2
	exit 1
fi
if [[ "$(git -C "${oracle_dir}" rev-parse HEAD)" != "${oracle_commit}" ]]; then
	printf 'C++ oracle HEAD must be %s; found %s\n' "${oracle_commit}" \
		"$(git -C "${oracle_dir}" rev-parse HEAD)" >&2
	exit 1
fi

context_dir="$(mktemp -d /tmp/cemu-extend-cpp-oracle.XXXXXX)"
printf '%s\n' "${oracle_commit}" > "${context_dir}/.cemu-oracle-context"
export_repository "${oracle_dir}" "${oracle_commit}" "${context_dir}"

# The context is made solely from commit objects, so this commit/submodule
# fingerprint is also a complete cache key without reading host working files.
source_fingerprint="$(
	{
		printf '%s\n' "${oracle_commit}"
		git -C "${oracle_dir}" submodule status --recursive
	} | sha256sum | cut -d' ' -f1
)"

docker build --progress=plain \
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
	"${context_dir}"

build_completed=1
printf 'Docker C++ oracle target built: %s (%s)\n' "${docker_target}" "${oracle_commit}"
