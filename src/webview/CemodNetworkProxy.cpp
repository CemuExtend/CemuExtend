#include "webview/CemodNetworkProxy.h"

#include "webview/CemodNetworkPolicy.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <compare>
#include <cctype>
#include <istream>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

namespace WebFrontend::CefOverlay
{
	namespace
	{
		using boost::asio::ip::tcp;
		constexpr std::size_t kMaximumProxyHeaderBytes = 16U * 1024U;
		constexpr std::size_t kMaximumProxyConnections = 64;

		struct ProxyAuthority
		{
			std::string host;
			std::uint16_t port{};
			auto operator<=>(const ProxyAuthority&) const = default;
		};

		std::optional<ProxyAuthority> ParseAuthority(std::string_view authority)
		{
			std::string_view host;
			std::string_view portText;
			if (authority.starts_with('['))
			{
				const auto close = authority.find(']');
				if (close == std::string_view::npos || close + 1 >= authority.size() ||
					authority[close + 1] != ':')
					return std::nullopt;
				host = authority.substr(1, close - 1);
				portText = authority.substr(close + 2);
			}
			else
			{
				const auto colon = authority.rfind(':');
				if (colon == std::string_view::npos || authority.find(':') != colon)
					return std::nullopt;
				host = authority.substr(0, colon);
				portText = authority.substr(colon + 1);
			}
			std::uint32_t port{};
			const auto parsed = std::from_chars(portText.data(),
				portText.data() + portText.size(), port);
			if (host.empty() || parsed.ec != std::errc{} ||
				parsed.ptr != portText.data() + portText.size() || port == 0 || port > 65535)
				return std::nullopt;
			std::string lower(host);
			std::ranges::transform(lower, lower.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return ProxyAuthority{std::move(lower), static_cast<std::uint16_t>(port)};
		}

		struct ProxyPolicy
		{
			std::set<ProxyAuthority> authorities;
			bool allowPrivateNetwork{};
			std::atomic_size_t activeConnections{};
		};

		class ProxySession final : public std::enable_shared_from_this<ProxySession>
		{
		  public:
			ProxySession(tcp::socket client, std::shared_ptr<ProxyPolicy> policy)
				: m_client(std::move(client)), m_upstream(m_client.get_executor()),
				  m_resolver(m_client.get_executor()), m_timer(m_client.get_executor()),
				  m_policy(std::move(policy))
			{
				++m_policy->activeConnections;
			}

			~ProxySession()
			{
				--m_policy->activeConnections;
			}

			void Start()
			{
				ArmTimeout(std::chrono::seconds(30));
				ReadHeader();
			}

		  private:
			void ArmTimeout(std::chrono::steady_clock::duration duration)
			{
				m_timer.expires_after(duration);
				m_timer.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
					if (!error) self->Close();
				});
			}

			void ReadHeader()
			{
				auto self = shared_from_this();
				boost::asio::async_read_until(m_client, m_header, "\r\n\r\n",
					[self](const boost::system::error_code& error, std::size_t) {
						if (error || self->m_header.size() > kMaximumProxyHeaderBytes)
						{
							self->Close();
							return;
						}
						self->HandleHeader();
					});
			}

			void HandleHeader()
			{
				std::istream stream(&m_header);
				std::string line;
				if (!std::getline(stream, line))
				{
					Close();
					return;
				}
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const auto firstSpace = line.find(' ');
				const auto secondSpace = firstSpace == std::string::npos ? std::string::npos
					: line.find(' ', firstSpace + 1);
				if (firstSpace == std::string::npos || secondSpace == std::string::npos ||
					line.substr(0, firstSpace) != "CONNECT" ||
					line.substr(secondSpace + 1) != "HTTP/1.1")
				{
					Reject("405 Method Not Allowed");
					return;
				}
				const auto authority = ParseAuthority(std::string_view(line).substr(
					firstSpace + 1, secondSpace - firstSpace - 1));
				if (!authority || !m_policy->authorities.contains(*authority))
				{
					Reject("403 Forbidden");
					return;
				}
				m_authority = *authority;
				auto self = shared_from_this();
				m_resolver.async_resolve(m_authority.host, std::to_string(m_authority.port),
					[self](const boost::system::error_code& error, tcp::resolver::results_type results) {
						if (error || results.empty() ||
							(!self->m_policy->allowPrivateNetwork &&
							 !std::ranges::all_of(results, [](const auto& entry) {
								 return IsPublicNetworkAddress(entry.endpoint().address().to_string());
							 })))
						{
							self->Reject("403 Forbidden");
							return;
						}
						self->Connect(std::move(results));
					});
			}

			void Connect(tcp::resolver::results_type results)
			{
				auto self = shared_from_this();
				boost::asio::async_connect(m_upstream, std::move(results),
					[self](const boost::system::error_code& error, const tcp::endpoint&) {
						if (error)
						{
							self->Reject("502 Bad Gateway");
							return;
						}
						self->AcceptTunnel();
					});
			}

			void AcceptTunnel()
			{
				m_response = "HTTP/1.1 200 Connection Established\r\n\r\n";
				auto self = shared_from_this();
				boost::asio::async_write(m_client, boost::asio::buffer(m_response),
					[self](const boost::system::error_code& error, std::size_t) {
						if (error)
						{
							self->Close();
							return;
						}
						self->ArmTimeout(std::chrono::minutes(30));
						self->ReadClient();
						self->ReadUpstream();
					});
			}

			void ReadClient()
			{
				auto self = shared_from_this();
				m_client.async_read_some(boost::asio::buffer(m_clientBuffer),
					[self](const boost::system::error_code& error, std::size_t bytes) {
						if (error) { self->Close(); return; }
						self->ArmTimeout(std::chrono::minutes(30));
						boost::asio::async_write(self->m_upstream,
							boost::asio::buffer(self->m_clientBuffer.data(), bytes),
							[self](const boost::system::error_code& writeError, std::size_t) {
								if (writeError) self->Close(); else self->ReadClient();
							});
					});
			}

			void ReadUpstream()
			{
				auto self = shared_from_this();
				m_upstream.async_read_some(boost::asio::buffer(m_upstreamBuffer),
					[self](const boost::system::error_code& error, std::size_t bytes) {
						if (error) { self->Close(); return; }
						self->ArmTimeout(std::chrono::minutes(30));
						boost::asio::async_write(self->m_client,
							boost::asio::buffer(self->m_upstreamBuffer.data(), bytes),
							[self](const boost::system::error_code& writeError, std::size_t) {
								if (writeError) self->Close(); else self->ReadUpstream();
							});
					});
			}

			void Reject(std::string_view status)
			{
				m_response = "HTTP/1.1 " + std::string(status) +
					"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
				auto self = shared_from_this();
				boost::asio::async_write(m_client, boost::asio::buffer(m_response),
					[self](const boost::system::error_code&, std::size_t) { self->Close(); });
			}

			void Close()
			{
				boost::system::error_code ignored;
				m_timer.cancel();
				m_resolver.cancel();
				m_client.close(ignored);
				m_upstream.close(ignored);
			}

			tcp::socket m_client;
			tcp::socket m_upstream;
			tcp::resolver m_resolver;
			boost::asio::steady_timer m_timer;
			boost::asio::streambuf m_header{kMaximumProxyHeaderBytes};
			std::shared_ptr<ProxyPolicy> m_policy;
			ProxyAuthority m_authority;
			std::string m_response;
			std::array<std::byte, 32U * 1024U> m_clientBuffer{};
			std::array<std::byte, 32U * 1024U> m_upstreamBuffer{};
		};

		class CemodNetworkProxyImpl final : public CemodNetworkProxy
		{
		  public:
			explicit CemodNetworkProxyImpl(std::shared_ptr<ProxyPolicy> policy)
				: m_policy(std::move(policy)), m_acceptor(m_io) {}

			~CemodNetworkProxyImpl() override
			{
				m_io.stop();
				if (m_thread.joinable()) m_thread.join();
			}

			bool Start()
			{
				boost::system::error_code error;
				m_acceptor.open(tcp::v4(), error);
				if (error) return false;
				m_acceptor.set_option(tcp::acceptor::reuse_address(false), error);
				if (error) return false;
				m_acceptor.bind({boost::asio::ip::address_v4::loopback(), 0}, error);
				if (error) return false;
				m_acceptor.listen(boost::asio::socket_base::max_listen_connections, error);
				if (error) return false;
				m_port = m_acceptor.local_endpoint(error).port();
				if (error || !m_port) return false;
				Accept();
				m_thread = std::thread([this] { m_io.run(); });
				return true;
			}

			std::uint16_t Port() const noexcept override { return m_port; }

		  private:
			void Accept()
			{
				m_acceptor.async_accept([this](const boost::system::error_code& error,
					tcp::socket socket) {
					if (!error && m_policy->activeConnections.load() < kMaximumProxyConnections)
						std::make_shared<ProxySession>(std::move(socket), m_policy)->Start();
					if (m_acceptor.is_open()) Accept();
				});
			}

			boost::asio::io_context m_io;
			std::shared_ptr<ProxyPolicy> m_policy;
			tcp::acceptor m_acceptor;
			std::thread m_thread;
			std::uint16_t m_port{};
		};

		void AddOrigins(std::set<ProxyAuthority>& authorities,
			const std::vector<std::string>& origins)
		{
			for (const auto& value : origins)
			{
				const auto origin = ParseCemodNetworkOrigin(value);
				if (!origin) continue;
				const auto colon = origin->canonical.rfind(':');
				std::uint32_t port{};
				if (colon == std::string::npos) continue;
				const auto text = std::string_view(origin->canonical).substr(colon + 1);
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
				if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
					port > 0 && port <= 65535)
					authorities.emplace(ProxyAuthority{origin->host,
						static_cast<std::uint16_t>(port)});
			}
		}
	} // namespace

	std::shared_ptr<CemodNetworkProxy> CemodNetworkProxy::Create(
		const std::vector<std::string>& connectOrigins,
		const std::vector<std::string>& resourceOrigins, bool allowPrivateNetwork)
	{
		auto policy = std::make_shared<ProxyPolicy>();
		policy->allowPrivateNetwork = allowPrivateNetwork;
		AddOrigins(policy->authorities, connectOrigins);
		AddOrigins(policy->authorities, resourceOrigins);
		if (policy->authorities.empty())
			return {};
		auto result = std::make_shared<CemodNetworkProxyImpl>(std::move(policy));
		return result->Start() ? result : nullptr;
	}
} // namespace WebFrontend::CefOverlay
