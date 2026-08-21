#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Application
{
	struct GuestAddress
	{
		std::uint32_t value{};
	};

	enum class PpcDebuggerControl : std::uint8_t
	{
		Break,
		Run,
		StepInto,
		StepOver,
	};

	struct PpcDebuggerInstruction
	{
		GuestAddress address;
		std::uint32_t opcode{};
		std::string mnemonic;
		std::string operands;
		bool current{};
		bool breakpoint{};
	};

	struct PpcDebuggerBreakpoint
	{
		std::uint64_t backendIdentity{};
		GuestAddress address;
		bool enabled{};
		bool logging{};
	};

	struct PpcDebuggerBackendSnapshot
	{
		bool available{};
		bool trapped{};
		GuestAddress instructionPointer;
		std::array<std::uint32_t, 32> gpr{};
		std::uint32_t linkRegister{};
		std::vector<PpcDebuggerInstruction> instructions;
		std::vector<PpcDebuggerBreakpoint> breakpoints;
		bool breakpointCapReached{};
		std::string diagnostic;
	};

	class IPpcDebuggerBackend
	{
	public:
		virtual ~IPpcDebuggerBackend() = default;
		[[nodiscard]] virtual PpcDebuggerBackendSnapshot Capture(
			GuestAddress center, std::uint32_t instructionCount,
			std::uint32_t breakpointLimit) = 0;
		virtual void ToggleExecuteBreakpoint(GuestAddress address) = 0;
		virtual void SetBreakpointEnabled(std::uint64_t identity,
			GuestAddress address, bool enabled) = 0;
		virtual void DeleteBreakpoint(std::uint64_t identity,
			GuestAddress address) = 0;
		virtual void Control(PpcDebuggerControl command) = 0;
	};

	struct PpcDebuggerBreakpointView
	{
		std::string identity;
		GuestAddress address;
		bool enabled{};
		bool logging{};
	};

	struct PpcDebuggerSnapshot
	{
		std::uint64_t generation{};
		bool available{};
		bool trapped{};
		GuestAddress instructionPointer;
		std::array<std::uint32_t, 32> gpr{};
		std::uint32_t linkRegister{};
		std::vector<PpcDebuggerInstruction> instructions;
		std::vector<PpcDebuggerBreakpointView> breakpoints;
		bool breakpointCapReached{};
		std::string diagnostic;
	};

	class PpcDebuggerFacade final
	{
	public:
		static constexpr std::uint32_t MaximumInstructionCount = 128;
		static constexpr std::uint32_t MaximumBreakpoints = 256;

		explicit PpcDebuggerFacade(std::unique_ptr<IPpcDebuggerBackend> backend);
		[[nodiscard]] PpcDebuggerSnapshot Capture(std::uint64_t ownerWindow,
			GuestAddress center, std::uint32_t instructionCount);
		void ToggleExecuteBreakpoint(std::uint64_t ownerWindow,
			std::uint64_t generation, GuestAddress address);
		void SetBreakpointEnabled(std::uint64_t ownerWindow,
			std::uint64_t generation, std::string_view identity, bool enabled);
		void DeleteBreakpoint(std::uint64_t ownerWindow,
			std::uint64_t generation, std::string_view identity);
		void Control(std::uint64_t ownerWindow, std::uint64_t generation,
			PpcDebuggerControl command);
		void CloseOwner(std::uint64_t ownerWindow) noexcept;
		void BeginShutdown() noexcept;

	private:
		struct BreakpointBinding { std::uint64_t backendIdentity{}; GuestAddress address; };
		struct OwnerState
		{
			std::uint64_t generation{};
			bool trapped{};
			std::unordered_map<std::string, BreakpointBinding> breakpoints;
		};
		[[nodiscard]] OwnerState& RequireOwner(std::uint64_t ownerWindow,
			std::uint64_t generation);
		[[nodiscard]] static std::string NewIdentity();
		static void ValidateAddress(GuestAddress address);

		std::unique_ptr<IPpcDebuggerBackend> m_backend;
		std::mutex m_mutex;
		std::unordered_map<std::uint64_t, OwnerState> m_owners;
		std::uint64_t m_nextGeneration{};
		bool m_shuttingDown{};
	};

	[[nodiscard]] std::unique_ptr<IPpcDebuggerBackend> CreateCafePpcDebuggerBackend();
}
