#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsFunctionPatcher.h"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>

namespace
{
	constexpr std::uint32_t kBranchOpcode = 18;
	constexpr std::uint32_t kConditionalBranchOpcode = 16;
	constexpr std::uint32_t kBranchDisplacementMask = 0x03fffffcU;
	constexpr std::uint32_t kConditionalDisplacementMask = 0x0000fffcU;
	constexpr std::uint32_t kLisR12 = 0x3d800000U;
	constexpr std::uint32_t kOriR12R12 = 0x618c0000U;
	constexpr std::uint32_t kMtctrR12 = 0x7d8903a6U;
	constexpr std::uint32_t kBctr = 0x4e800420U;
	constexpr std::uint32_t kMaximumTrampolineBytes = 256;

	std::int32_t SignExtend(std::uint32_t value, unsigned bits)
	{
		const auto sign = std::uint32_t{1} << (bits - 1);
		return static_cast<std::int32_t>((value ^ sign) - sign);
	}

	bool RelativeBranch(std::uint32_t from, std::uint32_t to,
						std::uint32_t& instruction, bool link = false)
	{
		const auto displacement =
			static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
		if ((displacement & 3) != 0 || displacement < -0x02000000LL ||
			displacement > 0x01fffffcLL)
			return false;
		instruction = 0x48000000U |
					  (static_cast<std::uint32_t>(displacement) &
					   kBranchDisplacementMask) |
					  (link ? 1U : 0U);
		return true;
	}

	bool AbsoluteBranch(std::uint32_t to, std::uint32_t& instruction)
	{
		if ((to & 3U) != 0 || to > 0x01fffffcU)
			return false;
		instruction = 0x48000002U | (to & kBranchDisplacementMask);
		return true;
	}

	std::array<std::uint32_t, 4> FarJump(std::uint32_t target)
	{
		return {
			kLisR12 | (target >> 16),
			kOriR12R12 | (target & 0xffffU),
			kMtctrR12,
			kBctr,
		};
	}

	std::string ProcessName(WupsPatchProcess process)
	{
		switch (process)
		{
		case WupsPatchProcess::RootRpx:
			return "ROOT_RPX";
		case WupsPatchProcess::WiiUMenu:
			return "WII_U_MENU";
		case WupsPatchProcess::Tvii:
			return "TVII";
		case WupsPatchProcess::EManual:
			return "E_MANUAL";
		case WupsPatchProcess::HomeMenu:
			return "HOME_MENU";
		case WupsPatchProcess::ErrorDisplay:
			return "ERROR_DISPLAY";
		case WupsPatchProcess::MiniMiiverse:
			return "MINI_MIIVERSE";
		case WupsPatchProcess::Browser:
			return "BROWSER";
		case WupsPatchProcess::Miiverse:
			return "MIIVERSE";
		case WupsPatchProcess::Eshop:
			return "ESHOP";
		case WupsPatchProcess::DownloadManager:
			return "DOWNLOAD_MANAGER";
		case WupsPatchProcess::Game:
			return "GAME";
		case WupsPatchProcess::GameAndMenu:
			return "GAME_AND_MENU";
		case WupsPatchProcess::All:
			return "ALL";
		}
		return "UNKNOWN";
	}
} // namespace

std::optional<std::string_view> WupsPatchLibraryName(std::uint32_t library)
{
	static constexpr std::array names{
		"avm",
		"camera",
		"coreinit",
		"dc",
		"dmae",
		"drmapp",
		"erreula",
		"gx2",
		"h264",
		"lzma920",
		"mic",
		"nfc",
		"nio_prof",
		"nlibcurl",
		"nlibnss",
		"nlibnss2",
		"nn_ac",
		"nn_acp",
		"nn_act",
		"nn_aoc",
		"nn_boss",
		"nn_ccr",
		"nn_cmpt",
		"nn_dlp",
		"nn_ec",
		"nn_fp",
		"nn_hai",
		"nn_hpad",
		"nn_idbe",
		"nn_ndm",
		"nn_nets2",
		"nn_nfp",
		"nn_nim",
		"nn_olv",
		"nn_pdm",
		"nn_save",
		"nn_sl",
		"nn_spm",
		"nn_temp",
		"nn_uds",
		"nn_vctl",
		"nsysccr",
		"nsyshid",
		"nsyskbd",
		"nsysnet",
		"nsysuhs",
		"nsysuvd",
		"ntag",
		"padscore",
		"proc_ui",
		"snd_core",
		"snd_user",
		"sndcore2",
		"snduser2",
		"swkbd",
		"sysapp",
		"tcl",
		"tve",
		"uac",
		"uac_rpl",
		"usb_mic",
		"uvc",
		"uvd",
		"vpad",
		"vpadbase",
		"zlib125",
	};
	if (library >= names.size())
		return std::nullopt;
	return names[library];
}

bool PpcFunctionRelocator::Relocate(std::span<const std::uint32_t> input,
									std::uint32_t sourceAddress, std::uint32_t destinationAddress,
									std::size_t minimumBytes, std::vector<std::uint32_t>& output,
									std::size_t& consumedBytes, std::string& error)
{
	output.clear();
	consumedBytes = 0;
	error.clear();
	if (minimumBytes == 0 || (minimumBytes & 3U) != 0 ||
		minimumBytes > kMaximumInputWords * sizeof(std::uint32_t) ||
		input.size() < minimumBytes / sizeof(std::uint32_t) ||
		(sourceAddress & 3U) != 0 || (destinationAddress & 3U) != 0)
	{
		error = "PPC relocation has invalid input size or alignment";
		return false;
	}

	struct StubFixup
	{
		std::size_t instructionIndex{};
		std::uint32_t target{};
		bool conditional{};
	};
	std::vector<StubFixup> stubs;
	const auto wordCount = minimumBytes / sizeof(std::uint32_t);
	output.reserve(wordCount + 1 + wordCount * 4);
	for (std::size_t index = 0; index < wordCount; ++index)
	{
		const auto instruction = input[index];
		const auto opcode = instruction >> 26;
		const auto source = sourceAddress +
							static_cast<std::uint32_t>(index * sizeof(std::uint32_t));
		const auto destination = destinationAddress +
								 static_cast<std::uint32_t>(output.size() * sizeof(std::uint32_t));
		if (opcode == kBranchOpcode)
		{
			const bool absolute = (instruction & 2U) != 0;
			const auto displacement = SignExtend(
				instruction & kBranchDisplacementMask, 26);
			const auto target = absolute ? static_cast<std::uint32_t>(displacement) : source + static_cast<std::uint32_t>(displacement);
			const bool link = (instruction & 1U) != 0;
			std::uint32_t relocated{};
			if (RelativeBranch(destination, target, relocated, link))
				output.push_back(relocated);
			else
			{
				output.push_back(0);
				stubs.push_back({output.size() - 1, target, false});
			}
		}
		else if (opcode == kConditionalBranchOpcode)
		{
			const bool absolute = (instruction & 2U) != 0;
			const auto displacement = SignExtend(
				instruction & kConditionalDisplacementMask, 16);
			const auto target = absolute ? static_cast<std::uint32_t>(displacement) : source + static_cast<std::uint32_t>(displacement);
			const auto newDisplacement =
				static_cast<std::int64_t>(target) - destination;
			if ((newDisplacement & 3) == 0 &&
				newDisplacement >= -0x8000 &&
				newDisplacement <= 0x7ffc)
			{
				output.push_back(
					(instruction & ~kConditionalDisplacementMask & ~2U) |
					(static_cast<std::uint32_t>(newDisplacement) &
					 kConditionalDisplacementMask));
			}
			else
			{
				output.push_back(instruction &
								 ~kConditionalDisplacementMask & ~2U);
				stubs.push_back({output.size() - 1, target, true});
			}
		}
		else
			output.push_back(instruction);
	}
	consumedBytes = minimumBytes;

	const auto continuation = sourceAddress +
							  static_cast<std::uint32_t>(consumedBytes);
	const auto branchBackAddress = destinationAddress +
								   static_cast<std::uint32_t>(output.size() * 4);
	std::uint32_t branchBack{};
	if (RelativeBranch(branchBackAddress, continuation, branchBack))
		output.push_back(branchBack);
	else
	{
		const auto farJump = FarJump(continuation);
		output.insert(output.end(), farJump.begin(), farJump.end());
	}

	for (const auto& stub : stubs)
	{
		const auto stubAddress = destinationAddress +
								 static_cast<std::uint32_t>(output.size() * 4);
		const auto instructionAddress = destinationAddress +
										static_cast<std::uint32_t>(stub.instructionIndex * 4);
		if (stub.conditional)
		{
			const auto displacement =
				static_cast<std::int64_t>(stubAddress) - instructionAddress;
			if (displacement < -0x8000 || displacement > 0x7ffc)
			{
				error = "PPC conditional branch repair stub is out of range";
				return false;
			}
			output[stub.instructionIndex] |=
				static_cast<std::uint32_t>(displacement) &
				kConditionalDisplacementMask;
		}
		else if (!RelativeBranch(instructionAddress, stubAddress,
								 output[stub.instructionIndex],
								 (input[stub.instructionIndex] & 1U) != 0))
		{
			error = "PPC branch repair stub is out of REL24 range";
			return false;
		}
		const auto farJump = FarJump(stub.target);
		output.insert(output.end(), farJump.begin(), farJump.end());
	}
	if (output.size() * sizeof(std::uint32_t) > kMaximumTrampolineBytes)
	{
		error = "PPC relocated prologue exceeds the trampoline size limit";
		return false;
	}
	return true;
}

struct WupsFunctionPatchManager::Impl
{
	struct PatchRecord
	{
		WupsPatchRequest request;
		WupsResolvedPatchTarget target;
		std::uint32_t originalInstruction{};
		std::uint32_t oldCallThrough{};
		std::uint32_t trampolineAddress{};
		std::uint32_t trampolineSize{};
		bool callThroughWritten{};
		bool targetWritten{};
	};

	explicit Impl(std::shared_ptr<IWupsPatchPlatform> platform_) : platform(std::move(platform_))
	{
	}

	std::shared_ptr<IWupsPatchPlatform> platform;
	mutable std::mutex mutex;
	std::vector<WupsPatchRequest> pending;
	std::vector<PatchRecord> patches;
	std::map<std::uint32_t, WupsPatchOwner> claimedTargets;

	struct PreparedRequest
	{
		WupsPatchRequest request;
		WupsResolvedPatchTarget target;
		bool resolved{};
		bool unavailable{};
		std::string error;
		PatchRecord patch;
		std::uint32_t patchInstruction{};
		std::uint64_t moduleRevision{};
		bool built{};
	};

	struct ExecutableAllocation
	{
		std::uint32_t address{};
		std::uint32_t size{};
	};

	// Preflight runs without the ledger lock because the production platform may
	// enter the RPL loader. Events advance this revision so a preflight result
	// cannot be committed after its module started loading or unloading.
	std::map<std::string, std::uint64_t, std::less<>> moduleRevisions;

	bool Resolve(const WupsPatchRequest& request,
				 WupsResolvedPatchTarget& target, bool& unavailable,
				 std::string& error)
	{
		unavailable = false;
		switch (request.targetKind)
		{
		case WupsPatchTargetKind::NamedFunction:
		{
			auto resolved = platform->ResolveFunction(
				request.moduleName, request.functionName, error);
			if (!resolved)
			{
				unavailable = error.empty();
				return false;
			}
			target = std::move(*resolved);
			return true;
		}
		case WupsPatchTargetKind::VirtualAddress:
			target = {
				request.virtualAddress,
				request.moduleName, 0};
			return true;
		case WupsPatchTargetKind::PhysicalAddress:
		{
			auto address =
				platform->VirtualForPhysical(request.physicalAddress, error);
			if (!address)
				return false;
			target = {*address, request.moduleName, 0};
			return true;
		}
		}
		error = "function patch has an unknown target kind";
		return false;
	}

	static bool NeedsCleanup(const PatchRecord& patch)
	{
		return patch.targetWritten || patch.callThroughWritten;
	}

	bool Restore(PatchRecord& patch, std::string& error,
				 std::vector<ExecutableAllocation>& frees)
	{
		bool success = true;
		if (patch.targetWritten)
		{
			const std::array original{patch.originalInstruction};
			std::string writeError;
			if (!platform->WriteWords(
					patch.target.address, original, writeError))
			{
				success = false;
				error.append(error.empty() ? "" : "; ").append(writeError);
			}
			else
			{
				platform->InvalidateCode(patch.target.address, 4);
				patch.targetWritten = false;
			}
		}
		if (patch.callThroughWritten)
		{
			const std::array oldValue{patch.oldCallThrough};
			std::string writeError;
			if (!platform->WriteWords(
					patch.request.callThroughStorage, oldValue, writeError))
			{
				success = false;
				error.append(error.empty() ? "" : "; ").append(writeError);
			}
			else
				patch.callThroughWritten = false;
		}
		if (!patch.targetWritten && !patch.callThroughWritten &&
			patch.trampolineAddress != 0)
		{
			frees.push_back(
				{patch.trampolineAddress, patch.trampolineSize});
			patch.trampolineAddress = 0;
		}
		// A failed instruction restore leaves live modified guest code. Keep the
		// ownership claim so another plugin cannot layer a patch over that
		// partially restored address; a later cleanup retry releases it.
		if (!patch.targetWritten)
			claimedTargets.erase(patch.target.address);
		return success;
	}

	bool PrepareBuild(const WupsPatchRequest& request,
					  const WupsResolvedPatchTarget& target, PatchRecord& patch,
					  std::uint32_t& patchInstruction, std::string& error)
	{
		if ((target.address & 3U) != 0 ||
			(request.replacementAddress & 3U) != 0 ||
			(request.callThroughStorage & 3U) != 0 ||
			!platform->IsExecutable(target.address, 4) ||
			!platform->IsExecutable(request.replacementAddress, 4) ||
			!platform->IsWritable(request.callThroughStorage, 4))
		{
			error = fmt::format(
				"patch descriptor {} has a non-executable/misaligned target or "
				"replacement, or non-writable call-through storage",
				request.descriptorIndex);
			return false;
		}
		std::array<std::uint32_t, PpcFunctionRelocator::kMaximumInputWords>
			input{};
		if (!platform->ReadWords(target.address,
								 std::span<std::uint32_t>(input).first(1), error))
			return false;
		std::array<std::uint32_t, 1> oldStorage{};
		if (!platform->ReadWords(
				request.callThroughStorage, oldStorage, error))
			return false;

		std::uint32_t trampoline{};
		if (!platform->AllocateExecutableNear(
				target.address, kMaximumTrampolineBytes, 32,
				trampoline, error))
			return false;
		std::vector<std::uint32_t> relocated;
		std::size_t consumed{};
		if (!PpcFunctionRelocator::Relocate(
				std::span<const std::uint32_t>(input).first(1),
				target.address, trampoline, 4, relocated, consumed, error))
		{
			platform->FreeExecutable(trampoline, kMaximumTrampolineBytes);
			return false;
		}
		auto bridgeAddress = trampoline +
							 static_cast<std::uint32_t>(relocated.size() * 4);
		const auto bridge = FarJump(request.replacementAddress);
		relocated.insert(relocated.end(), bridge.begin(), bridge.end());
		if (relocated.size() * 4 > kMaximumTrampolineBytes ||
			!platform->WriteWords(trampoline, relocated, error))
		{
			platform->FreeExecutable(trampoline, kMaximumTrampolineBytes);
			return false;
		}
		platform->InvalidateCode(
			trampoline, static_cast<std::uint32_t>(relocated.size() * 4));

		if (!RelativeBranch(target.address, request.replacementAddress,
							patchInstruction) &&
			!RelativeBranch(target.address, bridgeAddress, patchInstruction) &&
			!AbsoluteBranch(bridgeAddress, patchInstruction))
		{
			platform->FreeExecutable(trampoline, kMaximumTrampolineBytes);
			error = fmt::format(
				"patch descriptor {} cannot reach its replacement or bridge",
				request.descriptorIndex);
			return false;
		}
		patch.request = request;
		patch.target = target;
		patch.originalInstruction = input[0];
		patch.oldCallThrough = oldStorage[0];
		patch.trampolineAddress = trampoline;
		patch.trampolineSize = kMaximumTrampolineBytes;
		return true;
	}

	std::vector<PreparedRequest> PrepareRequests(
		std::span<const WupsPatchRequest> requests)
	{
		std::vector<PreparedRequest> prepared;
		const auto process = platform->CurrentProcess();
		for (const auto& request : requests)
		{
			if (!WupsFunctionPatchManager::ProcessMatches(
					request.process, process))
				continue;
			PreparedRequest item;
			item.request = request;
			{
				std::lock_guard lock(mutex);
				item.moduleRevision = moduleRevisions[request.moduleName];
			}
			item.resolved = Resolve(item.request, item.target,
									item.unavailable, item.error);
			if (item.resolved)
				item.built = PrepareBuild(item.request, item.target,
										  item.patch, item.patchInstruction, item.error);
			prepared.push_back(std::move(item));
		}
		return prepared;
	}

	bool CommitBuild(PreparedRequest& prepared, std::string& error,
					 std::vector<ExecutableAllocation>& frees)
	{
		const auto& request = prepared.request;
		const auto& target = prepared.target;
		if (const auto conflict = claimedTargets.find(target.address);
			conflict != claimedTargets.end())
		{
			error = fmt::format(
				"patch descriptor {} overlaps address 0x{:08x} owned by {} generation {}",
				request.descriptorIndex, target.address,
				conflict->second.owner, conflict->second.generation);
			return false;
		}
		if (moduleRevisions[request.moduleName] != prepared.moduleRevision)
		{
			error = fmt::format(
				"patch descriptor {} target module '{}' lifetime {} changed or began unloading while the patch was prepared",
				request.descriptorIndex, target.moduleName,
				target.moduleLifetime);
			return false;
		}
		if (!prepared.built)
		{
			error = prepared.error;
			return false;
		}
		auto& patch = prepared.patch;

		const std::array callThrough{patch.trampolineAddress};
		if (!platform->WriteWords(
				request.callThroughStorage, callThrough, error))
		{
			frees.push_back({patch.trampolineAddress, patch.trampolineSize});
			patch.trampolineAddress = 0;
			return false;
		}
		patch.callThroughWritten = true;
		const std::array targetWrite{prepared.patchInstruction};
		if (!platform->WriteWords(target.address, targetWrite, error))
		{
			std::string rollbackError;
			Restore(patch, rollbackError, frees);
			if (!rollbackError.empty())
				error.append("; rollback: ").append(rollbackError);
			return false;
		}
		patch.targetWritten = true;
		platform->InvalidateCode(target.address, 4);
		claimedTargets.emplace(target.address, request.owner);
		return true;
	}

	void Free(std::span<const ExecutableAllocation> frees)
	{
		// The production executable allocator also enters the RPL loader. Every
		// caller invokes this only after releasing the patch ledger mutex.
		for (const auto& allocation : frees)
			platform->FreeExecutable(allocation.address, allocation.size);
	}

	void DiscardPrepared(std::span<PreparedRequest> prepared,
						 std::vector<ExecutableAllocation>& frees)
	{
		for (auto& item : prepared)
		{
			if (item.patch.trampolineAddress != 0)
				frees.push_back({item.patch.trampolineAddress,
								 item.patch.trampolineSize});
			item.patch.trampolineAddress = 0;
		}
	}

	void Rollback(std::vector<PatchRecord>& committed, std::string& error,
				  std::vector<ExecutableAllocation>& frees)
	{
		for (auto& patch : std::ranges::reverse_view(committed))
		{
			std::string rollbackError;
			Restore(patch, rollbackError, frees);
			if (!rollbackError.empty())
				error.append("; rollback: ").append(rollbackError);
		}
		for (auto& patch : committed)
			if (NeedsCleanup(patch))
				patches.push_back(std::move(patch));
	}

	bool ApplyRequests(std::span<PreparedRequest> requests,
					   bool allowPending, std::string& error,
					   std::vector<ExecutableAllocation>& frees)
	{
		std::vector<PatchRecord> committed;
		std::vector<WupsPatchRequest> newPending;
		for (auto& prepared : requests)
		{
			const auto& request = prepared.request;
			if (!prepared.resolved)
			{
				if (allowPending && prepared.unavailable && !request.mandatory)
				{
					newPending.push_back(request);
					continue;
				}
				error = fmt::format(
					"patch descriptor {} ({}) resolution failed: {}",
					request.descriptorIndex,
					request.mandatory ? "mandatory" : "optional",
					prepared.error.empty() ? "target is not loaded" : prepared.error);
				Rollback(committed, error, frees);
				DiscardPrepared(requests, frees);
				return false;
			}
			if (!CommitBuild(prepared, error, frees))
			{
				if (NeedsCleanup(prepared.patch))
				{
					patches.push_back(std::move(prepared.patch));
					prepared.patch = {};
				}
				Rollback(committed, error, frees);
				DiscardPrepared(requests, frees);
				return false;
			}
			committed.push_back(std::move(prepared.patch));
			prepared.patch = {};
		}
		patches.insert(patches.end(),
					   std::make_move_iterator(committed.begin()),
					   std::make_move_iterator(committed.end()));
		pending.insert(pending.end(),
					   std::make_move_iterator(newPending.begin()),
					   std::make_move_iterator(newPending.end()));
		return true;
	}
};

WupsFunctionPatchManager::WupsFunctionPatchManager(
	std::shared_ptr<IWupsPatchPlatform> platform) : m_impl(std::make_unique<Impl>(std::move(platform)))
{
}

WupsFunctionPatchManager::~WupsFunctionPatchManager()
{
	std::string error;
	(void)RemoveAll(error);
}

bool WupsFunctionPatchManager::ProcessMatches(
	WupsPatchProcess descriptor, WupsPatchProcess current)
{
	if (descriptor == WupsPatchProcess::All)
		return true;
	if (descriptor == WupsPatchProcess::GameAndMenu)
		return current == WupsPatchProcess::Game ||
			   current == WupsPatchProcess::WiiUMenu;
	return descriptor == current;
}

bool WupsFunctionPatchManager::Apply(
	std::span<const WupsPatchRequest> requests, std::string& error)
{
	error.clear();
	if (!m_impl->platform)
	{
		error = "function patch platform is unavailable";
		return false;
	}
	if (requests.empty())
		return true;
	const auto owner = requests.front().owner;
	if (owner.owner == 0 || owner.generation == 0 ||
		std::ranges::any_of(requests, [&](const auto& request) {
			return request.owner != owner;
		}))
	{
		error = "function patch transaction has invalid or mixed owner generations";
		return false;
	}
	auto prepared = m_impl->PrepareRequests(requests);
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	if (std::ranges::any_of(m_impl->patches, [&](const auto& patch) {
			return patch.request.owner == owner;
		}) ||
		std::ranges::any_of(m_impl->pending, [&](const auto& request) {
			return request.owner == owner;
		}))
	{
		error = "function patch owner generation is already registered";
		m_impl->DiscardPrepared(prepared, frees);
		lock.unlock();
		m_impl->Free(frees);
		return false;
	}
	const auto success = m_impl->ApplyRequests(
		prepared, true, error, frees);
	lock.unlock();
	m_impl->Free(frees);
	return success;
}

bool WupsFunctionPatchManager::Add(
	const WupsPatchRequest& request, std::string& error)
{
	error.clear();
	if (!m_impl->platform)
	{
		error = "function patch platform is unavailable";
		return false;
	}
	if (request.owner.owner == 0 || request.owner.generation == 0)
	{
		error = "dynamic function patch has an invalid owner generation";
		return false;
	}
	const auto preparedRequest = std::span{&request, 1};
	auto prepared = m_impl->PrepareRequests(preparedRequest);
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	const auto sameIdentity = [&](const auto& candidate) {
		return candidate.request.owner == request.owner &&
			   candidate.request.descriptorIndex == request.descriptorIndex;
	};
	if (std::ranges::any_of(m_impl->patches, sameIdentity) ||
		std::ranges::any_of(m_impl->pending, [&](const auto& candidate) {
			return candidate.owner == request.owner &&
				   candidate.descriptorIndex == request.descriptorIndex;
		}))
	{
		error = "dynamic function patch descriptor identity already exists";
		m_impl->DiscardPrepared(prepared, frees);
		lock.unlock();
		m_impl->Free(frees);
		return false;
	}
	const auto success = m_impl->ApplyRequests(
		prepared, true, error, frees);
	lock.unlock();
	m_impl->Free(frees);
	return success;
}

bool WupsFunctionPatchManager::Remove(const WupsPatchOwner& owner,
									  std::size_t descriptorIndex, std::string& error)
{
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	error.clear();
	bool found{};
	for (auto iterator = m_impl->patches.rbegin();
		 iterator != m_impl->patches.rend(); ++iterator)
	{
		if (iterator->request.owner != owner ||
			iterator->request.descriptorIndex != descriptorIndex)
			continue;
		found = true;
		if (!m_impl->Restore(*iterator, error, frees))
		{
			lock.unlock();
			m_impl->Free(frees);
			return false;
		}
		break;
	}
	const auto oldPending = m_impl->pending.size();
	std::erase_if(m_impl->pending, [&](const auto& request) {
		return request.owner == owner &&
			   request.descriptorIndex == descriptorIndex;
	});
	found = found || oldPending != m_impl->pending.size();
	if (!found)
	{
		error = "dynamic function patch descriptor was not found";
		lock.unlock();
		m_impl->Free(frees);
		return false;
	}
	std::erase_if(m_impl->patches, [&](const auto& patch) {
		return patch.request.owner == owner &&
			   patch.request.descriptorIndex == descriptorIndex;
	});
	lock.unlock();
	m_impl->Free(frees);
	return true;
}

bool WupsFunctionPatchManager::RemoveOwner(
	const WupsPatchOwner& owner, std::string& error)
{
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	error.clear();
	bool success = true;
	for (auto iterator = m_impl->patches.rbegin();
		 iterator != m_impl->patches.rend(); ++iterator)
	{
		if (iterator->request.owner != owner)
			continue;
		std::string restoreError;
		if (!m_impl->Restore(*iterator, restoreError, frees))
		{
			success = false;
			error.append(error.empty() ? "" : "; ").append(restoreError);
		}
	}
	if (success)
		std::erase_if(m_impl->patches, [&](const auto& patch) {
			return patch.request.owner == owner;
		});
	std::erase_if(m_impl->pending, [&](const auto& request) {
		return request.owner == owner;
	});
	lock.unlock();
	m_impl->Free(frees);
	return success;
}

bool WupsFunctionPatchManager::OnModuleLoaded(
	const WupsPatchModuleEvent& event, std::string& error)
{
	std::vector<WupsPatchRequest> candidates;
	{
		std::lock_guard lock(m_impl->mutex);
		++m_impl->moduleRevisions[event.moduleName];
		for (const auto& request : m_impl->pending)
			if (request.targetKind == WupsPatchTargetKind::NamedFunction &&
				request.moduleName == event.moduleName)
				candidates.push_back(request);
	}
	if (candidates.empty())
	{
		error.clear();
		return true;
	}
	auto prepared = m_impl->PrepareRequests(candidates);
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	std::erase_if(prepared, [&](const auto& item) {
		const bool removed = std::ranges::none_of(
			m_impl->pending, [&](const auto& pending) {
				return pending.owner == item.request.owner &&
					   pending.descriptorIndex == item.request.descriptorIndex;
			});
		if (removed && item.patch.trampolineAddress != 0)
			frees.push_back({item.patch.trampolineAddress,
							 item.patch.trampolineSize});
		return removed;
	});
	if (prepared.empty())
	{
		error.clear();
		lock.unlock();
		m_impl->Free(frees);
		return true;
	}
	if (!m_impl->ApplyRequests(prepared, false, error, frees))
	{
		lock.unlock();
		m_impl->Free(frees);
		return false;
	}
	for (const auto& item : prepared)
		std::erase_if(m_impl->pending, [&](const auto& pending) {
			return pending.owner == item.request.owner &&
				   pending.descriptorIndex == item.request.descriptorIndex;
		});
	lock.unlock();
	m_impl->Free(frees);
	return true;
}

bool WupsFunctionPatchManager::OnModuleUnloading(
	const WupsPatchModuleEvent& event, std::string& error)
{
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	error.clear();
	++m_impl->moduleRevisions[event.moduleName];
	bool success = true;
	std::vector<WupsPatchRequest> restoredPending;
	for (auto iterator = m_impl->patches.rbegin();
		 iterator != m_impl->patches.rend(); ++iterator)
	{
		if (iterator->target.moduleName != event.moduleName ||
			(event.moduleLifetime != 0 &&
			 iterator->target.moduleLifetime != event.moduleLifetime))
			continue;
		std::string restoreError;
		if (!m_impl->Restore(*iterator, restoreError, frees))
		{
			success = false;
			error.append(error.empty() ? "" : "; ").append(restoreError);
		}
		else
			restoredPending.push_back(iterator->request);
	}
	if (!success)
	{
		lock.unlock();
		m_impl->Free(frees);
		return false;
	}
	for (const auto& request : restoredPending)
		m_impl->pending.push_back(request);
	std::erase_if(m_impl->patches, [&](const auto& patch) {
		return patch.target.moduleName == event.moduleName &&
			   (event.moduleLifetime == 0 ||
				patch.target.moduleLifetime == event.moduleLifetime);
	});
	lock.unlock();
	m_impl->Free(frees);
	return true;
}

bool WupsFunctionPatchManager::RemoveAll(std::string& error)
{
	std::vector<Impl::ExecutableAllocation> frees;
	std::unique_lock lock(m_impl->mutex);
	error.clear();
	bool success = true;
	for (auto& patch : std::ranges::reverse_view(m_impl->patches))
	{
		std::string restoreError;
		if (!m_impl->Restore(patch, restoreError, frees))
		{
			success = false;
			error.append(error.empty() ? "" : "; ").append(restoreError);
		}
	}
	if (success)
		m_impl->patches.clear();
	m_impl->pending.clear();
	lock.unlock();
	m_impl->Free(frees);
	return success;
}

std::vector<WupsAppliedPatch> WupsFunctionPatchManager::Applied() const
{
	std::lock_guard lock(m_impl->mutex);
	std::vector<WupsAppliedPatch> result;
	result.reserve(m_impl->patches.size());
	for (const auto& patch : m_impl->patches)
		result.push_back({patch.request.owner, patch.request.descriptorIndex,
						  patch.target.address, patch.request.replacementAddress,
						  patch.trampolineAddress, patch.trampolineSize,
						  patch.target.moduleName, patch.target.moduleLifetime});
	return result;
}

std::size_t WupsFunctionPatchManager::PendingCount() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->pending.size();
}
