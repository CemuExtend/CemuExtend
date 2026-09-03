#pragma once

#include "cemuextend/services.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cemuextend_hle
{
	struct Cex2MediaResult
	{
		cemuextend::wire::Status status{cemuextend::wire::Status::NotSupported};
		std::vector<std::byte> payload;
	};

	class Cex2Media
	{
	  public:
		static Cex2MediaResult Dispatch(std::uint64_t session, std::string_view principal,
										std::uint16_t operation, std::span<const std::byte> payload);
		static void ReleaseSession(std::uint64_t session);
		static void MixMicrophone(std::span<std::int16_t> samples);
		static std::size_t ActiveSessions();
	};
} // namespace cemuextend_hle
