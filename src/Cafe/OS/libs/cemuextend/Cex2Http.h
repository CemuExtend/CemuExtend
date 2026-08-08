#pragma once

#include "cemuextend/services.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cemuextend_hle {

struct Cex2HttpResult
{
	cemuextend::wire::Status status{cemuextend::wire::Status::IoError};
	std::vector<std::byte> payload;
};

// Host-side fetch for guests that have no usable network stack of their own.
//
// A transfer cannot be answered inside the dispatch call that asks for it, so
// this is a three-step service: Start hands back a handle and runs the transfer
// on a host thread, Poll reports progress and copies the body out in chunks,
// and Release drops it. Handles are scoped to one session and are reaped with
// it, so a guest that disappears mid-transfer leaves nothing behind.
class Cex2Http
{
public:
	// `session` is the address-space id the handle belongs to. Handles are only
	// visible to the session that started them.
	static Cex2HttpResult Dispatch(std::uint64_t session, std::string_view principal,
		std::uint16_t operation, std::span<const std::byte> payload);
	// Cancels and drops every transfer belonging to a session. In-flight host
	// threads finish on their own and discard their result.
	static void ReleaseSession(std::uint64_t session);
	[[nodiscard]] static std::size_t ActiveTransfers();
};

} // namespace cemuextend_hle
