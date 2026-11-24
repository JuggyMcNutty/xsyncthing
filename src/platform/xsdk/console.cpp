#include "platform/xsdk/console.hpp"

#include <cerrno>
#include <format>

#include <arpa/inet.h>
#include <netinet/in.h>

extern "C" {
#include "xsdk/net.h"
#include "xsdk/terminate.h"
#include "xsdk/thread.h"
}

namespace xst::console {

void place_current_thread(unsigned hardware_thread)
{
	if (hardware_thread < XSDK_HARDWARE_THREADS)
		xsdk_thread_move(hardware_thread);
}

void on_home(void (*fn)()) { xsdk_on_terminate(fn); }

std::string address()
{
	in_addr a{};
	const int error = xsdk_net_address(&a);
	if (error == EAGAIN)
		return "getting an address...";
	if (error != 0)
		return std::format("no network (error {})", error);
	char text[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &a, text, sizeof text);
	return text;
}

} // namespace xst::console

// The network limits xsyncthing asks XAM for: enough sockets for several peers, discovery, relays and the GUI, and
// the largest buffers XAM takes (xsdk net.h).
extern "C" const XsdkNetworkLimits xsdk_network_limits = { 64, 16, 63, 63 };
