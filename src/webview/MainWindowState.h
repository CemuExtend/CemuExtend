#pragma once

#include <cstdint>
#include <mutex>

namespace WebFrontend
{
	enum class MainWindowContentMode : std::uint8_t
	{
		Library,
		LaunchPending,
		Playing,
		ShuttingDown,
	};

	struct MainWindowSnapshot
	{
		MainWindowContentMode mode{MainWindowContentMode::Library};
		bool webViewVisible{true};
		bool renderSurfaceVisible{};
		std::uintptr_t mainWindowIdentity{};
		std::uint64_t generation{};
	};

	class MainWindowState final
	{
	public:
		explicit MainWindowState(std::uintptr_t mainWindowIdentity);

		[[nodiscard]] bool BeginLaunch();
		[[nodiscard]] bool CommitLaunch();
		[[nodiscard]] bool RollbackLaunch();
		[[nodiscard]] bool FinishEmulation();
		[[nodiscard]] bool BeginShutdown();
		[[nodiscard]] MainWindowSnapshot Snapshot() const;

	private:
		mutable std::mutex m_mutex;
		MainWindowSnapshot m_state;
	};
}
