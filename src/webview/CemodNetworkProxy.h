#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace WebFrontend::CefOverlay
{
	class CemodNetworkProxy
	{
	  public:
		[[nodiscard]] static std::shared_ptr<CemodNetworkProxy> Create(
			const std::vector<std::string>& connectOrigins,
			const std::vector<std::string>& resourceOrigins,
			bool allowPrivateNetwork);
		virtual ~CemodNetworkProxy() = default;
		[[nodiscard]] virtual std::uint16_t Port() const noexcept = 0;
	};
} // namespace WebFrontend::CefOverlay
