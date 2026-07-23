#pragma once

#include "Cafe/HW/Espresso/CemodPackage.h"

#include <cstdint>
#include <string>

// Runtime-neutral lifecycle contract. Implementations own every guest allocation,
// callback registration, patch, and module reference created for one package.
// Unload must be idempotent and must revoke callbacks before releasing memory.
class ITrustedPayloadInstance
{
public:
	virtual ~ITrustedPayloadInstance() = default;

	[[nodiscard]] virtual CemodPayloadFormat Format() const = 0;
	[[nodiscard]] virtual std::uint64_t OwnerHandle() const = 0;
	[[nodiscard]] virtual std::uint32_t Generation() const = 0;

	virtual bool OnApplicationStarts(std::string& error) = 0;
	virtual void OnReleaseForeground() = 0;
	virtual void OnAcquiredForeground() = 0;
	virtual void OnApplicationRequestsExit() = 0;
	virtual void OnApplicationEnds() = 0;
	virtual void Unload() = 0;
};
