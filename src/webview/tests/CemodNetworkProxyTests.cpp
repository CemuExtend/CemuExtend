#include "webview/CemodNetworkProxy.h"

#include <boost/asio.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <thread>

using boost::asio::ip::tcp;
using namespace WebFrontend::CefOverlay;

namespace
{
	std::string ReadHeaders(tcp::socket& socket)
	{
		boost::asio::streambuf response;
		boost::asio::read_until(socket, response, "\r\n\r\n");
		return {boost::asio::buffers_begin(response.data()),
			boost::asio::buffers_end(response.data())};
	}

	tcp::socket ConnectProxy(boost::asio::io_context& io, std::uint16_t proxyPort,
		std::string_view authority, std::string& response)
	{
		tcp::socket socket(io);
		socket.connect({boost::asio::ip::address_v4::loopback(), proxyPort});
		const auto request = "CONNECT " + std::string(authority) +
			" HTTP/1.1\r\nHost: " + std::string(authority) + "\r\n\r\n";
		boost::asio::write(socket, boost::asio::buffer(request));
		response = ReadHeaders(socket);
		return socket;
	}
}

int main()
{
	boost::asio::io_context io;
	{
		auto proxy = CemodNetworkProxy::Create({"https://127.0.0.1:24443"}, {}, false);
		assert(proxy);
		std::string response;
		auto socket = ConnectProxy(io, proxy->Port(), "127.0.0.1:24443", response);
		assert(response.starts_with("HTTP/1.1 403"));
	}
	{
		auto proxy = CemodNetworkProxy::Create({"https://example.com:443"}, {}, false);
		assert(proxy);
		std::string response;
		auto socket = ConnectProxy(io, proxy->Port(), "example.com:444", response);
		assert(response.starts_with("HTTP/1.1 403"));
	}
	{
		tcp::acceptor upstream(io, {boost::asio::ip::address_v4::loopback(), 0});
		const auto upstreamPort = upstream.local_endpoint().port();
		std::promise<void> served;
		auto servedFuture = served.get_future();
		std::thread server([&] {
			tcp::socket socket(io);
			upstream.accept(socket);
			std::array<char, 4> request{};
			boost::asio::read(socket, boost::asio::buffer(request));
			assert(std::string_view(request.data(), request.size()) == "PING");
			boost::asio::write(socket, boost::asio::buffer("PONG", 4));
			served.set_value();
		});

		const auto origin = "https://127.0.0.1:" + std::to_string(upstreamPort);
		auto proxy = CemodNetworkProxy::Create({origin}, {}, true);
		assert(proxy);
		std::string response;
		auto tunnel = ConnectProxy(io, proxy->Port(),
			"127.0.0.1:" + std::to_string(upstreamPort), response);
		assert(response.starts_with("HTTP/1.1 200"));
		boost::asio::write(tunnel, boost::asio::buffer("PING", 4));
		std::array<char, 4> reply{};
		boost::asio::read(tunnel, boost::asio::buffer(reply));
		assert(std::string_view(reply.data(), reply.size()) == "PONG");
		assert(servedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		server.join();
	}
	return 0;
}
