#include "Common/precompiled.h"

#include "application/PpcDebuggerFacade.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace Application
{
	namespace
	{
		class BreakpointLock final
		{
		  public:
			BreakpointLock() : values(debugger_lockBreakpoints()) {}
			~BreakpointLock()
			{
				debugger_unlockBreakpoints();
			}
			std::vector<DebuggerBreakpoint*>& values;
		};

		std::string FormatOperand(const PPCDisassemblerOperand& operand)
		{
			std::ostringstream output;
			switch (operand.type)
			{
			case PPCASM_OPERAND_TYPE_GPR:
				output << 'r' << unsigned(operand.registerIndex);
				break;
			case PPCASM_OPERAND_TYPE_FPR:
				output << 'f' << unsigned(operand.registerIndex);
				break;
			case PPCASM_OPERAND_TYPE_SPR:
				output << "spr" << unsigned(operand.registerIndex);
				break;
			case PPCASM_OPERAND_TYPE_CR:
				output << "cr" << unsigned(operand.registerIndex);
				break;
			case PPCASM_OPERAND_TYPE_CR_BIT:
				output << "crb" << unsigned(operand.registerIndex);
				break;
			case PPCASM_OPERAND_TYPE_CIMM:
				output << "0x" << std::hex << std::setw(8) << std::setfill('0') << operand.immU32;
				break;
			case PPCASM_OPERAND_TYPE_MEM:
				output << operand.immS32 << "(r" << unsigned(operand.registerIndex) << ')';
				break;
			case PPCASM_OPERAND_TYPE_PSQMODE:
				output << (operand.immS32 ? "single" : "paired");
				break;
			case PPCASM_OPERAND_TYPE_IMM:
			default:
				if (operand.isSignedImm)
					output << operand.immS32;
				else
					output << "0x" << std::hex << operand.immU32;
				break;
			}
			return output.str();
		}

		class CafePpcDebuggerBackend final : public IPpcDebuggerBackend
		{
		  public:
			PpcDebuggerBackendSnapshot Capture(GuestAddress center,
											   std::uint32_t instructionCount, std::uint32_t breakpointLimit) override
			{
				PpcDebuggerBackendSnapshot result;
				if (!CafeSystem::IsTitleRunning())
				{
					result.diagnostic = "No Wii U title is running.";
					return result;
				}
				result.available = true;
				if (auto* cpu = debugger_lockDebugSession())
				{
					result.trapped = true;
					result.instructionPointer.value = cpu->instructionPointer;
					const auto registers = debugger_getSnapshotFromSession(cpu);
					std::copy(std::begin(registers.gpr), std::end(registers.gpr), result.gpr.begin());
					result.linkRegister = registers.spr_lr;
					debugger_unlockDebugSession(cpu);
				}
				if (center.value == 0)
					center = result.instructionPointer;
				center.value &= ~std::uint32_t{3};
				const auto before = instructionCount / 3;
				const auto start64 = center.value >= before * 4 ? center.value - before * 4 : 0;
				BreakpointLock lock;
				for (std::uint32_t index = 0; index < instructionCount; ++index)
				{
					const auto address64 = static_cast<std::uint64_t>(start64) + index * 4ULL;
					if (address64 > std::numeric_limits<std::uint32_t>::max())
						break;
					const auto address = static_cast<std::uint32_t>(address64);
					if (!memory_isAddressRangeAccessible(address, 4))
						continue;
					std::uint32_t opcode = memory_readU32(address);
					bool hasBreakpoint = false;
					for (auto* bp = debugger_getFirstBP(address); bp; bp = bp->next)
						if (bp->isExecuteBP())
						{
							opcode = bp->originalOpcodeValue;
							hasBreakpoint = true;
							break;
						}
					PPCDisassembledInstruction decoded{};
					ppcAssembler_disassemble(address, opcode, &decoded);
					std::ostringstream operands;
					bool first = true;
					for (unsigned operandIndex = 0; operandIndex < PPCASM_OPERAND_COUNT; ++operandIndex)
					{
						if ((decoded.operandMask & (1U << operandIndex)) == 0)
							continue;
						if (!first)
							operands << ", ";
						first = false;
						operands << FormatOperand(decoded.operand[operandIndex]);
					}
					auto mnemonic = std::string(ppcAssembler_getInstructionName(decoded.ppcAsmCode));
					std::ranges::transform(mnemonic, mnemonic.begin(), [](unsigned char c) { return std::tolower(c); });
					result.instructions.push_back({{address}, opcode, std::move(mnemonic), operands.str(), address == result.instructionPointer.value, hasBreakpoint});
				}
				for (auto* head : lock.values)
					for (auto* bp = head; bp; bp = bp->next)
					{
						if (!bp->isExecuteBP() || bp->bpType == DEBUGGER_BP_T_ONE_SHOT)
							continue;
						if (result.breakpoints.size() == breakpointLimit)
						{
							result.breakpointCapReached = true;
							return result;
						}
						result.breakpoints.push_back({bp->id, {bp->address}, bp->enabled, bp->bpType == DEBUGGER_BP_T_LOGGING});
					}
				return result;
			}

			void ToggleExecuteBreakpoint(GuestAddress address) override
			{
				RequireWritableCode(address);
				BreakpointLock lock;
				debugger_toggleExecuteBreakpoint(address.value);
			}
			void SetBreakpointEnabled(std::uint64_t identity, GuestAddress address, bool enabled) override
			{
				RequireWritableCode(address);
				BreakpointLock lock;
				auto* bp = debugger_getBreakpointById(identity);
				if (!bp || bp->address != address.value || !bp->isExecuteBP() || bp->bpType == DEBUGGER_BP_T_ONE_SHOT)
					throw std::runtime_error("breakpoint is stale");
				debugger_toggleBreakpoint(address.value, enabled, bp);
			}
			void DeleteBreakpoint(std::uint64_t identity, GuestAddress address) override
			{
				RequireWritableCode(address);
				BreakpointLock lock;
				auto* bp = debugger_getBreakpointById(identity);
				if (!bp || bp->address != address.value || !bp->isExecuteBP() || bp->bpType == DEBUGGER_BP_T_ONE_SHOT)
					throw std::runtime_error("breakpoint is stale");
				debugger_deleteBreakpoint(identity);
			}
			void Control(PpcDebuggerControl command) override
			{
				if (!CafeSystem::IsTitleRunning())
					throw std::runtime_error("No Wii U title is running.");
				if (command == PpcDebuggerControl::Break)
				{
					if (!debugger_isTrapped())
						debugger_requestBreak();
					return;
				}
				auto* cpu = debugger_lockDebugSession();
				if (!cpu)
					throw std::runtime_error("The PPC debugger is not paused.");
				const auto native = command == PpcDebuggerControl::Run ? DebuggerStepCommand::Run : command == PpcDebuggerControl::StepInto ? DebuggerStepCommand::StepInto
																																			: DebuggerStepCommand::StepOver;
				debugger_stepCommand(native);
				debugger_unlockDebugSession(cpu);
			}

		  private:
			static void RequireWritableCode(GuestAddress address)
			{
				if (!CafeSystem::IsTitleRunning() || (address.value & 3U) != 0 ||
					!memory_isAddressRangeAccessible(address.value, 4))
					throw std::invalid_argument("guest address is not accessible aligned code");
			}
		};
	} // namespace

	PpcDebuggerFacade::PpcDebuggerFacade(std::unique_ptr<IPpcDebuggerBackend> backend)
		: m_backend(std::move(backend))
	{
		if (!m_backend)
			throw std::invalid_argument("PPC debugger backend is required");
	}

	void PpcDebuggerFacade::ValidateAddress(GuestAddress address)
	{
		if ((address.value & 3U) != 0)
			throw std::invalid_argument("guest address must be 4-byte aligned");
	}

	std::string PpcDebuggerFacade::NewIdentity()
	{
		static std::atomic_uint64_t sequence{1};
		static const std::uint64_t salt = std::random_device{}();
		std::ostringstream output;
		output << std::hex << (sequence.fetch_add(1, std::memory_order_relaxed) ^ salt);
		return output.str();
	}

	PpcDebuggerFacade::OwnerState& PpcDebuggerFacade::RequireOwner(
		std::uint64_t ownerWindow, std::uint64_t generation)
	{
		if (m_shuttingDown)
			throw std::runtime_error("PPC debugger is shutting down");
		auto found = m_owners.find(ownerWindow);
		if (found == m_owners.end() || found->second.generation != generation)
			throw std::invalid_argument("PPC debugger snapshot is stale");
		return found->second;
	}

	PpcDebuggerSnapshot PpcDebuggerFacade::Capture(std::uint64_t ownerWindow,
												   GuestAddress center, std::uint32_t instructionCount)
	{
		ValidateAddress(center);
		if (instructionCount == 0 || instructionCount > MaximumInstructionCount)
			throw std::invalid_argument("instruction count is outside the supported range");
		std::scoped_lock lock(m_mutex);
		if (m_shuttingDown)
			throw std::runtime_error("PPC debugger is shutting down");
		auto backend = m_backend->Capture(center, instructionCount, MaximumBreakpoints);
		PpcDebuggerSnapshot result;
		result.generation = ++m_nextGeneration;
		result.available = backend.available;
		result.trapped = backend.trapped;
		result.instructionPointer = backend.instructionPointer;
		result.gpr = backend.gpr;
		result.linkRegister = backend.linkRegister;
		result.instructions = std::move(backend.instructions);
		result.breakpointCapReached = backend.breakpointCapReached;
		result.diagnostic = std::move(backend.diagnostic);
		OwnerState state{result.generation, result.trapped, {}};
		for (const auto& breakpoint : backend.breakpoints)
		{
			auto identity = NewIdentity();
			state.breakpoints.emplace(identity, BreakpointBinding{breakpoint.backendIdentity, breakpoint.address});
			result.breakpoints.push_back({std::move(identity), breakpoint.address,
										  breakpoint.enabled, breakpoint.logging});
		}
		m_owners.insert_or_assign(ownerWindow, std::move(state));
		return result;
	}

	void PpcDebuggerFacade::ToggleExecuteBreakpoint(std::uint64_t ownerWindow,
													std::uint64_t generation, GuestAddress address)
	{
		ValidateAddress(address);
		std::scoped_lock lock(m_mutex);
		(void)RequireOwner(ownerWindow, generation);
		m_backend->ToggleExecuteBreakpoint(address);
		m_owners.erase(ownerWindow);
	}

	void PpcDebuggerFacade::SetBreakpointEnabled(std::uint64_t ownerWindow,
												 std::uint64_t generation, std::string_view identity, bool enabled)
	{
		std::scoped_lock lock(m_mutex);
		auto& owner = RequireOwner(ownerWindow, generation);
		const auto found = owner.breakpoints.find(std::string(identity));
		if (found == owner.breakpoints.end())
			throw std::invalid_argument("breakpoint identity is stale");
		m_backend->SetBreakpointEnabled(found->second.backendIdentity, found->second.address, enabled);
		m_owners.erase(ownerWindow);
	}

	void PpcDebuggerFacade::DeleteBreakpoint(std::uint64_t ownerWindow,
											 std::uint64_t generation, std::string_view identity)
	{
		std::scoped_lock lock(m_mutex);
		auto& owner = RequireOwner(ownerWindow, generation);
		const auto found = owner.breakpoints.find(std::string(identity));
		if (found == owner.breakpoints.end())
			throw std::invalid_argument("breakpoint identity is stale");
		m_backend->DeleteBreakpoint(found->second.backendIdentity, found->second.address);
		m_owners.erase(ownerWindow);
	}

	void PpcDebuggerFacade::Control(std::uint64_t ownerWindow,
									std::uint64_t generation, PpcDebuggerControl command)
	{
		std::scoped_lock lock(m_mutex);
		const auto& owner = RequireOwner(ownerWindow, generation);
		if (command == PpcDebuggerControl::Break && owner.trapped)
			throw std::invalid_argument("PPC debugger is already paused");
		if (command != PpcDebuggerControl::Break && !owner.trapped)
			throw std::invalid_argument("PPC debugger snapshot is not paused");
		m_backend->Control(command);
		m_owners.erase(ownerWindow);
	}

	void PpcDebuggerFacade::CloseOwner(std::uint64_t ownerWindow) noexcept
	{
		std::scoped_lock lock(m_mutex);
		m_owners.erase(ownerWindow);
	}

	void PpcDebuggerFacade::BeginShutdown() noexcept
	{
		std::scoped_lock lock(m_mutex);
		m_shuttingDown = true;
		m_owners.clear();
	}

	std::unique_ptr<IPpcDebuggerBackend> CreateCafePpcDebuggerBackend()
	{
		return std::make_unique<CafePpcDebuggerBackend>();
	}
} // namespace Application
