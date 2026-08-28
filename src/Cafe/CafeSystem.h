#pragma once
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/CafeEvent.h"

#include <memory>

namespace Host
{
	class ICanvasHost;
}

enum class CosCapabilityBits : uint64;
enum class CosCapabilityGroup : uint32;
enum class CafeConsoleRegion;

namespace CafeSystem
{
	void ConfigureCanvasHost(std::shared_ptr<Host::ICanvasHost> canvas);
	enum class PREPARE_STATUS_CODE
	{
		SUCCESS,
		CANCELLED,
		INVALID_RPX,
		UNABLE_TO_MOUNT, // failed to mount through TitleInfo (most likely caused by an invalid or outdated path)
		CEMOD_RUNTIME_BUSY,
	};

	void Initialize();
	void SetEventSink(IEventSink* sink);
	void EmitEvent(const Event& event);
	[[nodiscard]] bool Shutdown();

	PREPARE_STATUS_CODE PrepareForegroundTitle(TitleId titleId);
	PREPARE_STATUS_CODE PrepareForegroundTitleFromStandaloneRPX(const fs::path& path);
	[[nodiscard]] std::optional<TitleId> GetStandaloneTitleId(const fs::path& path);
	void LaunchForegroundTitle();
	// Atomically stops the current title and prepares/starts its replacement.
	// UI Stop/Close operations are serialized across the entire transition.
	[[nodiscard]] bool SwitchForegroundTitle(TitleId titleId);
	// Releases a fully prepared title that has not started its PPC scheduler.
	void AbortPreparedTitle();
	bool IsTitleRunning();

	bool GetOverrideArgStr(std::vector<std::string>& args);
	void SetOverrideArgs(std::span<std::string> args);
	void UnsetOverrideArgs();

	TitleId GetForegroundTitleId();
	uint16 GetForegroundTitleVersion();
	uint32 GetForegroundTitleSDKVersion();
	CafeConsoleRegion GetForegroundTitleRegion();
	CafeConsoleRegion GetPlatformRegion();
	std::string GetForegroundTitleName();
	std::string GetForegroundTitleArgStr();
	uint32 GetForegroundTitleOlvAccesskey();
	CosCapabilityBits GetForegroundTitleCosCapabilities(CosCapabilityGroup group);
	std::optional<sint32> GetForegroundTitleReturnStatus(); // valid once the foreground title exited gracefully via coreinit exit

	// Completes sandbox/WUPS guest callbacks and revokes trusted access before
	// title CPU threads are destroyed. Trusted code is retained for ShutdownTitle's
	// post-thread late phase. Safe to call repeatedly.
	bool PrepareTitleShutdown();
	[[nodiscard]] bool ShutdownTitle();

	std::string GetMlcStoragePath(TitleId titleId);
	void MlcStorageMountAllTitles();

	std::string GetInternalVirtualCodeFolder();

	uint32 GetRPXHashBase();
	uint32 GetRPXHashUpdated();

	[[nodiscard]] bool RequestRecreateCanvas();
	void NotifyPPCProcessExit(sint32 status);

}; // namespace CafeSystem

extern RPLModule* applicationRPX;

extern std::atomic_bool g_isGPUInitFinished;
