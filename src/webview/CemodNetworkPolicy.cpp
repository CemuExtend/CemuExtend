#include "webview/CemodNetworkPolicy.h"

#include <boost/asio/ip/address.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>

namespace WebFrontend::CefOverlay
{
	namespace
	{
		std::string AsciiLower(std::string_view value)
		{
			std::string result(value);
			std::ranges::transform(result, result.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return result;
		}

		bool ValidDomain(std::string_view host)
		{
			if (host.empty() || host.size() > 253 || host.starts_with('.') ||
				host.ends_with('.') || host.find("..") != std::string_view::npos)
				return false;
			std::size_t cursor{};
			while (cursor < host.size())
			{
				const auto end = host.find('.', cursor);
				const auto label = host.substr(cursor,
					end == std::string_view::npos ? host.size() - cursor : end - cursor);
				if (label.empty() || label.size() > 63 || label.front() == '-' ||
					label.back() == '-' ||
					!std::ranges::all_of(label, [](unsigned char character) {
						return std::isalnum(character) || character == '-';
					}))
					return false;
				if (end == std::string_view::npos)
					break;
				cursor = end + 1;
			}
			return true;
		}
	} // namespace

	std::optional<CemodNetworkOrigin> ParseCemodNetworkOrigin(std::string_view url)
	{
		if (url.empty() || std::ranges::any_of(url, [](unsigned char character) {
			return character <= 0x20 || character == 0x7f;
		}))
			return std::nullopt;
		const auto separator = url.find("://");
		if (separator == std::string_view::npos)
			return std::nullopt;
		const auto scheme = AsciiLower(url.substr(0, separator));
		if (scheme != "https" && scheme != "wss")
			return std::nullopt;
		const auto authorityStart = separator + 3;
		const auto authorityEnd = url.find_first_of("/?#", authorityStart);
		const auto authority = url.substr(authorityStart,
			authorityEnd == std::string_view::npos ? url.size() - authorityStart
				: authorityEnd - authorityStart);
		if (authority.empty() || authority.find('@') != std::string_view::npos)
			return std::nullopt;

		std::string_view host;
		std::string_view portText;
		bool bracketed{};
		if (authority.starts_with('['))
		{
			const auto close = authority.find(']');
			if (close == std::string_view::npos)
				return std::nullopt;
			host = authority.substr(1, close - 1);
			bracketed = true;
			if (close + 1 < authority.size())
			{
				if (authority[close + 1] != ':')
					return std::nullopt;
				portText = authority.substr(close + 2);
			}
		}
		else
		{
			const auto colon = authority.rfind(':');
			if (colon != std::string_view::npos)
			{
				if (authority.find(':') != colon)
					return std::nullopt;
				host = authority.substr(0, colon);
				portText = authority.substr(colon + 1);
			}
			else
				host = authority;
		}
		if (host.empty() || authority.ends_with(':'))
			return std::nullopt;

		std::uint32_t port = 443;
		if (!portText.empty())
		{
			const auto parsed = std::from_chars(portText.data(),
				portText.data() + portText.size(), port);
			if (parsed.ec != std::errc{} || parsed.ptr != portText.data() + portText.size() ||
				port == 0 || port > 65535)
				return std::nullopt;
		}

		const auto lowerHost = AsciiLower(host);
		boost::system::error_code error;
		const auto address = boost::asio::ip::make_address(lowerHost, error);
		std::string identity;
		bool literal{};
		if (!error)
		{
			if (address.is_v6() != bracketed)
				return std::nullopt;
			literal = true;
			identity = address.is_v6() ? '[' + address.to_string() + ']'
				: address.to_string();
		}
		else
		{
			if (bracketed || !ValidDomain(lowerHost))
				return std::nullopt;
			identity = lowerHost;
		}
		return CemodNetworkOrigin{scheme + "://" + identity + ':' + std::to_string(port),
			lowerHost, literal};
	}

	bool IsCemodNetworkUrlAllowed(std::string_view url, CemodNetworkRequestKind kind,
		const std::vector<std::string>& connectOrigins,
		const std::vector<std::string>& resourceOrigins)
	{
		const auto origin = ParseCemodNetworkOrigin(url);
		if (!origin)
			return false;
		const auto& allowlist = kind == CemodNetworkRequestKind::Connect
			? connectOrigins : resourceOrigins;
		return std::ranges::find(allowlist, origin->canonical) != allowlist.end();
	}

	bool IsLocalNetworkHostname(std::string_view host)
	{
		const auto lower = AsciiLower(host);
		return lower == "localhost" || lower.ends_with(".localhost") ||
			lower.ends_with(".local") || lower.find('.') == std::string::npos;
	}

	bool IsPublicNetworkAddress(std::string_view value)
	{
		boost::system::error_code error;
		const auto address = boost::asio::ip::make_address(value, error);
		if (error || address.is_unspecified() || address.is_loopback() || address.is_multicast())
			return false;
		if (address.is_v4())
		{
			const auto bytes = address.to_v4().to_bytes();
			return !(bytes[0] == 0 || bytes[0] == 10 || bytes[0] == 127 || bytes[0] >= 224 ||
				(bytes[0] == 100 && (bytes[1] & 0xc0) == 0x40) ||
				(bytes[0] == 169 && bytes[1] == 254) ||
				(bytes[0] == 172 && (bytes[1] & 0xf0) == 16) ||
				(bytes[0] == 192 && bytes[1] == 0 && bytes[2] == 0) ||
				(bytes[0] == 192 && bytes[1] == 0 && bytes[2] == 2) ||
				(bytes[0] == 192 && bytes[1] == 168) ||
				(bytes[0] == 198 && (bytes[1] == 18 || bytes[1] == 19)) ||
				(bytes[0] == 198 && bytes[1] == 51 && bytes[2] == 100) ||
				(bytes[0] == 203 && bytes[1] == 0 && bytes[2] == 113));
		}
		const auto bytes = address.to_v6().to_bytes();
		const bool mappedV4 = std::ranges::all_of(bytes.begin(), bytes.begin() + 10,
			[](std::uint8_t byte) { return byte == 0; }) && bytes[10] == 0xff && bytes[11] == 0xff;
		if (mappedV4)
		{
			const auto mapped = boost::asio::ip::address_v4({bytes[12], bytes[13], bytes[14], bytes[15]});
			return IsPublicNetworkAddress(mapped.to_string());
		}
		// Reject the deprecated IPv4-compatible ::/96 form. Treating it as a
		// globally routable IPv6 address could bypass the IPv4 private-range checks.
		if (std::ranges::all_of(bytes.begin(), bytes.begin() + 12,
			[](std::uint8_t byte) { return byte == 0; }))
			return false;
		return !((bytes[0] & 0xfe) == 0xfc ||
			(bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) ||
			(bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0xc0) ||
			(bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0d && bytes[3] == 0xb8));
	}
} // namespace WebFrontend::CefOverlay
