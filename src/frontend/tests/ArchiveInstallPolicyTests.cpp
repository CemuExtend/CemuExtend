#include "frontend/ArchiveInstallPolicy.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using namespace Frontend::ArchiveInstallPolicy;

int main()
{
	assert(NormalizeRelativePath("pack/rules.txt") == fs::path("pack/rules.txt"));
	assert(NormalizeRelativePath("pack/./rules.txt") == fs::path("pack/rules.txt"));
	for (const std::string_view unsafe : {
			 "", "../escape", "foo/../../escape", "foo/../escape", "/absolute",
			 "\\absolute", "C:\\Windows\\escape", "C:drive-relative", "..\\escape"})
	{
		assert(!NormalizeRelativePath(unsafe));
	}

	const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() /
						  ("cemu-archive-policy-" + std::to_string(seed));
	const fs::path target = root / "target";
	const fs::path staging = root / "staging";
	const fs::path backup = root / "backup";
	fs::create_directories(target);
	std::ofstream(target / "old.txt") << "old";

	// A failed final rename must put the original target back.
	const auto failed = CommitStagedDirectory(staging, target, backup, true);
	assert(!failed.committed);
	assert(failed.rollbackSucceeded);
	assert(fs::exists(target / "old.txt"));
	assert(!fs::exists(backup));

	fs::create_directories(staging);
	std::ofstream(staging / "new.txt") << "new";
	const auto committed = CommitStagedDirectory(staging, target, backup, true);
	assert(committed.committed);
	assert(fs::exists(target / "new.txt"));
	assert(!fs::exists(target / "old.txt"));
	assert(!fs::exists(backup));

	fs::remove_all(root);
}
