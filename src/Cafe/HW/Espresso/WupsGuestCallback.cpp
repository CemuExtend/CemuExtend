#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsGuestCallback.h"

#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cemu/Logging/CemuLogging.h"

#include <array>
#include <cstring>

bool WupsGuestCallback::Invoke(RPLModule* module, std::uint64_t lifetimeId,
							   std::uint32_t targetVirtualAddress, std::span<const std::uint32_t> argumentWords,
							   std::uint32_t& result, std::string& error, bool aggregateByReference)
{
	error.clear();
	result = 0;
	RPLModuleLease moduleLease;
	if (!RPLLoader_AcquireExternalModuleLease(module, lifetimeId, moduleLease, error))
		return false;
	MPTR callbackAddress{};
	if (!RPLLoader_ResolveModuleAddress(moduleLease, targetVirtualAddress,
										sizeof(std::uint32_t),
										RPLModuleAddressKind::Executable, callbackAddress) ||
		(callbackAddress & 3U) != 0)
	{
		error = fmt::format("guest callback target 0x{:08x} is not executable module memory",
							targetVirtualAddress);
		return false;
	}
	if (argumentWords.size() > 32)
	{
		error = "guest callback has more than 32 ABI argument words";
		return false;
	}
	PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
	if (!cpu)
	{
		error = "guest callback requires an emulated CPU thread";
		return false;
	}
	// A PPC EABI callback needs a 16-byte aligned stack with headroom for the
	// argument save area. Cemu thread stacks are only 8-byte aligned (see
	// __OSThreadInit), and the caller (e.g. coreinit_start) may hand us any valid
	// r1, so we align a working copy of the stack pointer down into the unused
	// region below the current frame and restore the caller's r1 afterwards.
	if (cpu->gpr[1] < 128)
	{
		error = "guest callback stack pointer is too low to build a frame";
		return false;
	}

	std::array<std::uint32_t, 32> savedGpr;
	std::array<FPR_t, 32> savedFpr;
	std::array<std::uint8_t, 32> savedCr;
	std::memcpy(savedGpr.data(), cpu->gpr, sizeof(cpu->gpr));
	// Align down for the duration of the callback; savedGpr already captured the
	// caller's original r1, which is restored below.
	cpu->gpr[1] &= ~static_cast<std::uint32_t>(0xFU);
	std::memcpy(savedFpr.data(), cpu->fpr, sizeof(cpu->fpr));
	std::memcpy(savedCr.data(), cpu->cr, sizeof(cpu->cr));
	const auto savedFpscr = cpu->fpscr;
	const auto savedXerCa = cpu->xer_ca;
	const auto savedXerSo = cpu->xer_so;
	const auto savedXerOv = cpu->xer_ov;
	const auto savedLr = cpu->spr.LR;
	const auto savedCtr = cpu->spr.CTR;
	const auto savedXer = cpu->spr.XER;
	const auto savedReservationAddress = cpu->reservedMemAddr;
	const auto savedReservationValue = cpu->reservedMemValue;
	const auto hadMemoryException = cpu->memoryException;

	// The WUPS hook ABI hands loader argument aggregates by reference.
	PPCCoreCallbackData_t callbackData{};
	if (aggregateByReference && !argumentWords.empty())
	{
		const std::uint32_t structBytes =
			static_cast<std::uint32_t>(argumentWords.size() * sizeof(std::uint32_t));
		cpu->gpr[1] -= (structBytes + 15u) & ~15u; // preserve 16-byte alignment
		const MPTR structAddress = cpu->gpr[1];
		for (std::size_t index = 0; index < argumentWords.size(); ++index)
			memory_writeU32(structAddress +
								static_cast<MPTR>(index * sizeof(std::uint32_t)),
							argumentWords[index]);
		_PPCCoreCallback_writeGPRArg(callbackData, cpu, structAddress);
	}
	else
	{
		for (const auto word : argumentWords)
			_PPCCoreCallback_writeGPRArg(callbackData, cpu, word);
	}
	if (const auto sda2 = RPLLoader_GetModuleSDA2Base(moduleLease))
		cpu->gpr[2] = sda2;
	if (const auto sda1 = RPLLoader_GetModuleSDA1Base(moduleLease))
		cpu->gpr[13] = sda1;

	result = PPCCoreCallback(callbackAddress, callbackData);
	const bool memoryException = !hadMemoryException && cpu->memoryException;

	// Restoring savedGpr rewinds r1 (undoing any by-reference struct reservation).
	std::memcpy(cpu->gpr, savedGpr.data(), sizeof(cpu->gpr));
	std::memcpy(cpu->fpr, savedFpr.data(), sizeof(cpu->fpr));
	std::memcpy(cpu->cr, savedCr.data(), sizeof(cpu->cr));
	cpu->fpscr = savedFpscr;
	cpu->xer_ca = savedXerCa;
	cpu->xer_so = savedXerSo;
	cpu->xer_ov = savedXerOv;
	cpu->spr.LR = savedLr;
	cpu->spr.CTR = savedCtr;
	cpu->spr.XER = savedXer;
	cpu->reservedMemAddr = savedReservationAddress;
	cpu->reservedMemValue = savedReservationValue;
	cpu->memoryException = hadMemoryException;

	if (memoryException)
	{
		error = fmt::format("guest callback at 0x{:08x} raised a memory exception",
							targetVirtualAddress);
		return false;
	}
	return true;
}
