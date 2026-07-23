#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class WupsPatchProcess : std::uint8_t
{
	RootRpx = 1,
	WiiUMenu = 2,
	Tvii = 3,
	EManual = 4,
	HomeMenu = 5,
	ErrorDisplay = 6,
	MiniMiiverse = 7,
	Browser = 8,
	Miiverse = 9,
	Eshop = 10,
	DownloadManager = 12,
	Game = 15,
	GameAndMenu = 16,
	All = 0xff,
};

enum class WupsPatchTargetKind : std::uint8_t
{
	NamedFunction,
	VirtualAddress,
	PhysicalAddress,
};

struct WupsPatchOwner
{
	std::uint64_t owner{};
	std::uint32_t generation{};

	[[nodiscard]] auto operator<=>(const WupsPatchOwner&) const = default;
};

struct WupsPatchRequest
{
	WupsPatchOwner owner;
	std::size_t descriptorIndex{};
	WupsPatchTargetKind targetKind{WupsPatchTargetKind::NamedFunction};
	bool mandatory{};
	std::string moduleName;
	std::string functionName;
	std::uint32_t virtualAddress{};
	std::uint32_t physicalAddress{};
	std::uint32_t replacementAddress{};
	std::uint32_t callThroughStorage{};
	WupsPatchProcess process{WupsPatchProcess::All};
};

struct WupsResolvedPatchTarget
{
	std::uint32_t address{};
	std::string moduleName;
	std::uint64_t moduleLifetime{};
};

struct WupsPatchModuleEvent
{
	std::string moduleName;
	std::uint64_t moduleLifetime{};
};

class IWupsPatchPlatform
{
public:
	virtual ~IWupsPatchPlatform() = default;

	[[nodiscard]] virtual WupsPatchProcess CurrentProcess() const = 0;
	virtual void SetCurrentProcess(WupsPatchProcess process) = 0;
	[[nodiscard]] virtual std::optional<WupsResolvedPatchTarget> ResolveFunction(
		std::string_view moduleName, std::string_view functionName,
		std::string& error) = 0;
	[[nodiscard]] virtual std::optional<std::uint32_t> VirtualForPhysical(
		std::uint32_t physicalAddress, std::string& error) = 0;
	[[nodiscard]] virtual bool IsExecutable(std::uint32_t address,
		std::uint32_t size) const = 0;
	[[nodiscard]] virtual bool IsWritable(std::uint32_t address,
		std::uint32_t size) const = 0;
	[[nodiscard]] virtual bool ReadWords(std::uint32_t address,
		std::span<std::uint32_t> words, std::string& error) = 0;
	[[nodiscard]] virtual bool WriteWords(std::uint32_t address,
		std::span<const std::uint32_t> words, std::string& error) = 0;
	[[nodiscard]] virtual bool AllocateExecutableNear(std::uint32_t target,
		std::uint32_t size, std::uint32_t alignment,
		std::uint32_t& address, std::string& error) = 0;
	virtual void FreeExecutable(std::uint32_t address,
		std::uint32_t size) = 0;
	virtual void InvalidateCode(std::uint32_t address,
		std::uint32_t size) = 0;
};

class PpcFunctionRelocator
{
public:
	static constexpr std::size_t kMaximumInputWords = 16;

	// Relocates whole PPC instructions until at least minimumBytes have been
	// consumed. Relative B/BL and BC/BCL instructions are repaired; far targets
	// receive local absolute CTR stubs.
	[[nodiscard]] static bool Relocate(std::span<const std::uint32_t> input,
		std::uint32_t sourceAddress, std::uint32_t destinationAddress,
		std::size_t minimumBytes, std::vector<std::uint32_t>& output,
		std::size_t& consumedBytes, std::string& error);
};

struct WupsAppliedPatch
{
	WupsPatchOwner owner;
	std::size_t descriptorIndex{};
	std::uint32_t targetAddress{};
	std::uint32_t replacementAddress{};
	std::uint32_t trampolineAddress{};
	std::uint32_t trampolineSize{};
	std::string moduleName;
	std::uint64_t moduleLifetime{};
};

class WupsFunctionPatchManager
{
public:
	explicit WupsFunctionPatchManager(
		std::shared_ptr<IWupsPatchPlatform> platform);
	~WupsFunctionPatchManager();

	[[nodiscard]] bool Apply(std::span<const WupsPatchRequest> requests,
		std::string& error);
	[[nodiscard]] bool RemoveOwner(const WupsPatchOwner& owner,
		std::string& error);
	[[nodiscard]] bool OnModuleLoaded(const WupsPatchModuleEvent& event,
		std::string& error);
	[[nodiscard]] bool OnModuleUnloading(const WupsPatchModuleEvent& event,
		std::string& error);
	[[nodiscard]] bool RemoveAll(std::string& error);

	[[nodiscard]] std::vector<WupsAppliedPatch> Applied() const;
	[[nodiscard]] std::size_t PendingCount() const;

	[[nodiscard]] static bool ProcessMatches(
		WupsPatchProcess descriptor, WupsPatchProcess current);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

[[nodiscard]] std::shared_ptr<IWupsPatchPlatform> CreateCemuWupsPatchPlatform();
[[nodiscard]] std::optional<std::string_view> WupsPatchLibraryName(
	std::uint32_t library);
