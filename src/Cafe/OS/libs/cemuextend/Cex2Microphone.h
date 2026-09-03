#pragma once

#include "cemuextend/services.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cemuextend_hle
{
	struct Cex2MicrophoneOwner
	{
		std::uint64_t addressSpaceId{};
		std::uint32_t generation{};
		std::uint32_t sessionId{};
		friend bool operator==(const Cex2MicrophoneOwner&, const Cex2MicrophoneOwner&) = default;
	};

	struct Cex2MicrophoneResult
	{
		cemuextend::wire::Status status{cemuextend::wire::Status::NotSupported};
		std::vector<std::byte> payload;
	};

	class Cex2Microphone
	{
	  public:
		static Cex2MicrophoneResult Dispatch(Cex2MicrophoneOwner owner, std::string_view principal,
											 std::uint16_t operation, std::span<const std::byte> payload);
		static void ReleaseSession(Cex2MicrophoneOwner owner);
		static void ReleaseOwner(std::uint64_t addressSpaceId, std::uint32_t generation);
		static void ReleaseAll();
	};
} // namespace cemuextend_hle
