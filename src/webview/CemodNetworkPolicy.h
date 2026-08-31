#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace WebFrontend::CefOverlay
{
	enum class CemodNetworkRequestKind
	{
		Connect,
		Resource,
	};

	struct CemodNetworkOrigin
	{
		std::string canonical;
		std::string host;
		bool addressLiteral{};
	};

	[[nodiscard]] std::optional<CemodNetworkOrigin> ParseCemodNetworkOrigin(
		std::string_view url);
	[[nodiscard]] bool IsCemodNetworkUrlAllowed(std::string_view url,
												CemodNetworkRequestKind kind, const std::vector<std::string>& connectOrigins,
												const std::vector<std::string>& resourceOrigins);
	[[nodiscard]] bool IsLocalNetworkHostname(std::string_view host);
	[[nodiscard]] bool IsPublicNetworkAddress(std::string_view address);
} // namespace WebFrontend::CefOverlay
