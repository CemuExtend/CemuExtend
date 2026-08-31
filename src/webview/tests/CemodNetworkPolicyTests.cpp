#include "webview/CemodNetworkPolicy.h"

#include <cassert>

using namespace WebFrontend::CefOverlay;

int main()
{
	const std::vector<std::string> connect{"https://api.example.com:443",
										   "wss://stream.example.com:443"};
	const std::vector<std::string> resources{"https://cdn.example.com:8443"};
	assert(IsCemodNetworkUrlAllowed("https://API.example.com/data?q=1",
									CemodNetworkRequestKind::Connect, connect, resources));
	assert(IsCemodNetworkUrlAllowed("wss://stream.example.com/socket",
									CemodNetworkRequestKind::Connect, connect, resources));
	assert(IsCemodNetworkUrlAllowed("https://cdn.example.com:8443/image.png",
									CemodNetworkRequestKind::Resource, connect, resources));
	assert(!IsCemodNetworkUrlAllowed("http://api.example.com/data",
									 CemodNetworkRequestKind::Connect, connect, resources));
	assert(!IsCemodNetworkUrlAllowed("https://api.example.com.evil.test/data",
									 CemodNetworkRequestKind::Connect, connect, resources));
	assert(!IsCemodNetworkUrlAllowed("https://user@api.example.com/data",
									 CemodNetworkRequestKind::Connect, connect, resources));
	assert(!IsCemodNetworkUrlAllowed("https://cdn.example.com:8443/image.png",
									 CemodNetworkRequestKind::Connect, connect, resources));

	assert(IsLocalNetworkHostname("localhost"));
	assert(IsLocalNetworkHostname("printer.local"));
	assert(IsLocalNetworkHostname("intranet"));
	assert(!IsLocalNetworkHostname("api.example.com"));
	for (const auto address : {"127.0.0.1", "10.0.0.1", "169.254.1.1", "172.16.0.1",
							   "192.168.1.1", "100.64.0.1", "198.51.100.1", "::1", "fe80::1", "fc00::1",
							   "2001:db8::1", "::ffff:192.168.1.1", "::7f00:1"})
		assert(!IsPublicNetworkAddress(address));
	assert(IsPublicNetworkAddress("8.8.8.8"));
	assert(IsPublicNetworkAddress("2606:4700:4700::1111"));
	assert(!IsPublicNetworkAddress("not-an-address"));
	return 0;
}
