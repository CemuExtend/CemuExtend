#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Application
{
	enum class PpcThreadState : std::uint8_t
	{
		None,
		Ready,
		Running,
		Waiting,
		Moribund,
		Suspended,
		Unknown,
	};

	struct PpcThreadSnapshot
	{
		std::uint32_t address{};
		std::uint64_t identity{};
		std::uint32_t entryPoint{};
		std::uint32_t stackLow{};
		std::uint32_t stackHigh{};
		std::uint32_t instructionPointer{};
		std::uint32_t linkRegister{};
		PpcThreadState state{PpcThreadState::Unknown};
		std::uint8_t requestedAffinity{};
		std::uint8_t effectiveAffinity{};
		std::int32_t basePriority{};
		std::int32_t effectivePriority{};
		std::uint64_t wakeUpTime{};
		std::uint64_t totalCycles{};
		std::string name;
		std::uint32_t gpr3{};
		std::uint32_t gpr4{};
		std::uint32_t gpr5{};
		std::uint32_t gpr6{};
		std::uint32_t gpr7{};
		bool cancelRequested{};
		bool suspensionOwnedByFacade{};
		std::uint32_t waitingMutex{};
		std::uint32_t mutexOwner{};
		std::uint32_t mutexLockCount{};
	};

	struct PpcThreadsSnapshot
	{
		std::uint64_t generation{};
		bool available{};
		std::string diagnostic;
		std::vector<PpcThreadSnapshot> threads;
	};

	enum class PpcThreadCommand : std::uint8_t
	{
		Suspend,
		Resume,
		AdjustPriority,
	};

	struct PpcThreadCommandRequest
	{
		std::uint64_t generation{};
		std::uint32_t threadAddress{};
		std::uint64_t threadIdentity{};
		PpcThreadCommand command{PpcThreadCommand::Suspend};
		std::int32_t priorityDelta{};
	};

	struct PpcThreadCommandResult
	{
		bool applied{};
		std::string diagnostic;
	};

	class IDiagnosticsService
	{
	  public:
		virtual ~IDiagnosticsService() = default;
		[[nodiscard]] virtual PpcThreadsSnapshot CapturePpcThreads() = 0;
		[[nodiscard]] virtual PpcThreadCommandResult ExecutePpcThreadCommand(
			const PpcThreadCommandRequest& request) = 0;
	};
} // namespace Application
