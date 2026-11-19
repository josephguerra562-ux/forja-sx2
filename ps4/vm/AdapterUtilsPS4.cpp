// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2: PCSX2's DEV9 adapter queries (pcsx2/DEV9/AdapterUtils.h) answered from the PS4's own network.
//
// The PS2 network adapter is emulated in "Sockets" mode (pcsx2/DEV9/sockets.cpp): the PS2's TCP/UDP traffic is replayed
// through ordinary BSD sockets of the PS4, so no raw access to the PS4's Ethernet/Wi-Fi is needed. What the sockets
// adapter and its internal DHCP server still ask for is the host adapter: its IP, netmask, router, DNS and MAC.
// On a PC that comes from getifaddrs(); the PS4 has none (musl's static one talks to Linux netlink), so this file gives a
// single ifaddrs-shaped entry, "ps4net", filled from sceNetCtlGetInfo.
#include "DEV9/AdapterUtils.h"

#include "common/Console.h"

#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>

#include <arpa/inet.h>
#include <cstring>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace PacketReader;
using namespace PacketReader::IP;

namespace
{
	constexpr const char* AdapterName = "ps4net";

	struct OwnedAdapter
	{
		ifaddrs ifa{};
		sockaddr_in addr{};
		sockaddr_in mask{};
		sockaddr_in broadcast{};
		char name[16]{};
	};

	// Loads the network modules and starts libnet once per process. CoverDownload and the telemetry do the same; a
	// second sceNetInit answers "already initialised", which is fine.
	bool EnsurePs4Net()
	{
		static const bool ready = [] {
			sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
			sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
			const int net = sceNetInit();
			const int ctl = sceNetCtlInit();
			Console.WriteLn("DEV9: PS4 net init=%#x netctl=%#x", net, ctl);
			return true;
		}();
		return ready;
	}

	// One dotted-quad field of sceNetCtl ("192.168.1.20"); nullopt when the PS4 has no value (no cable, no Wi-Fi, no DNS).
	std::optional<in_addr> NetCtlAddress(int code)
	{
		EnsurePs4Net();
		OrbisNetCtlInfo info;
		std::memset(&info, 0, sizeof(info));
		const int rc = sceNetCtlGetInfo(code, &info);
		if (rc < 0)
			return std::nullopt;
		char text[17]{};
		std::memcpy(text, info.ip_address, 16);
		in_addr out{};
		if (text[0] == '\0' || inet_pton(AF_INET, text, &out) != 1 || out.s_addr == 0)
			return std::nullopt;
		return out;
	}

	sockaddr_in MakeSockaddr(in_addr a)
	{
		sockaddr_in s{};
		s.sin_len = sizeof(sockaddr_in);
		s.sin_family = AF_INET;
		s.sin_addr = a;
		return s;
	}

	IP_Address ToIP(in_addr a)
	{
		IP_Address ip{};
		std::memcpy(ip.bytes, &a.s_addr, 4);
		return ip;
	}
} // namespace

void AdapterUtils::IfAdaptersDeleter::operator()(ifaddrs* buffer) const
{
	// ifa is the first member of OwnedAdapter, so the pointer handed out is the allocation's.
	delete reinterpret_cast<OwnedAdapter*>(buffer);
}

u16 AdapterUtils::ReadAddressFamily(const sockaddr* unknownAddr)
{
	sockaddr addr;
	std::memcpy(&addr, unknownAddr, offsetof(sockaddr, sa_family) + sizeof(addr.sa_family));
	return addr.sa_family;
}

AdapterUtils::Adapter* AdapterUtils::GetAllAdapters(AdapterBuffer* buffer)
{
	const std::optional<in_addr> ip = NetCtlAddress(ORBIS_NET_CTL_INFO_IP_ADDRESS);
	if (!ip.has_value())
	{
		Console.Error("DEV9: PS4 has no IP address (not connected to the internet?)");
		buffer->reset();
		return nullptr;
	}
	in_addr mask{};
	mask.s_addr = htonl(0xFFFFFF00u);
	if (const std::optional<in_addr> m = NetCtlAddress(ORBIS_NET_CTL_INFO_NETMASK))
		mask = *m;

	OwnedAdapter* a = new OwnedAdapter();
	std::strncpy(a->name, AdapterName, sizeof(a->name) - 1);
	a->addr = MakeSockaddr(*ip);
	a->mask = MakeSockaddr(mask);
	in_addr bcast{};
	bcast.s_addr = ip->s_addr | ~mask.s_addr;
	a->broadcast = MakeSockaddr(bcast);
	a->ifa.ifa_next = nullptr;
	a->ifa.ifa_name = a->name;
	a->ifa.ifa_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
	a->ifa.ifa_addr = reinterpret_cast<sockaddr*>(&a->addr);
	a->ifa.ifa_netmask = reinterpret_cast<sockaddr*>(&a->mask);
	a->ifa.ifa_broadaddr = reinterpret_cast<sockaddr*>(&a->broadcast);
	a->ifa.ifa_data = nullptr;
	buffer->reset(&a->ifa);

	char text[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &ip.value(), text, sizeof(text));
	Console.WriteLn("DEV9: PS4 adapter %s, IP %s", AdapterName, text);
	return buffer->get();
}

bool AdapterUtils::GetAdapter(const std::string& name, Adapter* adapter, AdapterBuffer* buffer)
{
	// There is only one: any name the settings carry ("Auto", "eth0" from a PC ini...) means the PS4's connection.
	(void)name;
	return GetAdapterAuto(adapter, buffer);
}

bool AdapterUtils::GetAdapterAuto(Adapter* adapter, AdapterBuffer* buffer)
{
	ifaddrs* found = GetAllAdapters(buffer);
	if (found == nullptr)
		return false;
	*adapter = *found;
	return true;
}

std::optional<MAC_Address> AdapterUtils::GetAdapterMAC(const Adapter* adapter)
{
	(void)adapter;
	EnsurePs4Net();
	OrbisNetCtlInfo info;
	std::memset(&info, 0, sizeof(info));
	if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_ETHER_ADDR, &info) < 0)
		return std::nullopt;
	MAC_Address mac{};
	std::memcpy(mac.bytes, info.ether_addr, 6);
	return mac;
}

std::optional<IP_Address> AdapterUtils::GetAdapterIP(const Adapter* adapter)
{
	if (adapter != nullptr && adapter->ifa_addr != nullptr && ReadAddressFamily(adapter->ifa_addr) == AF_INET)
	{
		sockaddr_in s;
		std::memcpy(&s, adapter->ifa_addr, sizeof(s));
		return ToIP(s.sin_addr);
	}
	if (const std::optional<in_addr> ip = NetCtlAddress(ORBIS_NET_CTL_INFO_IP_ADDRESS))
		return ToIP(*ip);
	return std::nullopt;
}

std::vector<IP_Address> AdapterUtils::GetGateways(const Adapter* adapter)
{
	(void)adapter;
	std::vector<IP_Address> out;
	if (const std::optional<in_addr> gw = NetCtlAddress(ORBIS_NET_CTL_INFO_DEFAULT_ROUTE))
		out.push_back(ToIP(*gw));
	return out;
}

// Compilation 146 (app): "Auto" gives the PS2 the DNS of PS2Online (45.7.228.197) first and the PS4's second. Sony's and GameSpy's
// servers are gone; that DNS sends their names to the fans' servers (2026-11-18: gate1.eu.dnas.playstation.org, gpcm.gamespy.com and
// peerchat.gamespy.com answered; Mortal Kombat Armageddon PAL went online with it in Forja). It does not answer other names, so the
// PS4's own DNS stays as the second one. A game revived by another group takes its DNS in En línea > DNS del servidor (Manual).
static constexpr u8 FORJA_FANS_DNS[4] = {45, 7, 228, 197};

std::vector<IP_Address> AdapterUtils::GetDNS(const Adapter* adapter)
{
	(void)adapter;
	std::vector<IP_Address> out;
	{
		IP_Address fans{};
		std::memcpy(fans.bytes, FORJA_FANS_DNS, 4);
		out.push_back(fans);
	}
	if (const std::optional<in_addr> d1 = NetCtlAddress(ORBIS_NET_CTL_INFO_PRIMARY_DNS))
		out.push_back(ToIP(*d1));
	if (const std::optional<in_addr> d2 = NetCtlAddress(ORBIS_NET_CTL_INFO_SECONDARY_DNS))
		out.push_back(ToIP(*d2));
	if (out.size() >= 2)
		Console.WriteLn("DEV9: Auto DNS: PS2Online %u.%u.%u.%u, then the PS4's %u.%u.%u.%u", out[0].bytes[0], out[0].bytes[1],
			out[0].bytes[2], out[0].bytes[3], out[1].bytes[0], out[1].bytes[1], out[1].bytes[2], out[1].bytes[3]);
	return out;
}

// Forja SX2: the internal DNS server's lookups (pcsx2/DEV9/InternalServers/DNS_Server.cpp, DNS mode "Internal" or a hosts
// entry without an address). musl's getaddrinfo reads /etc/resolv.conf, which the PS4 does not have; the system resolver
// asks the PS4's own DNS. Returns false when the name does not resolve.
bool Ps4ResolveHost(const char* host, u8 out_ip[4])
{
	EnsurePs4Net();
	static std::mutex s_lock;
	static int s_pool = -1;
	{
		std::lock_guard lock(s_lock);
		if (s_pool < 0)
			s_pool = sceNetPoolCreate("ForjaSX2 DEV9 DNS", 64 * 1024, 0);
	}
	if (s_pool < 0)
		return false;
	const OrbisNetId rid = sceNetResolverCreate("ForjaSX2 DEV9", s_pool, 0);
	if (rid < 0)
		return false;
	OrbisNetInAddr addr{};
	// timeout in microseconds (5 s), 2 retries
	const int rc = sceNetResolverStartNtoa(rid, host, &addr, 5 * 1000 * 1000, 2, 0);
	sceNetResolverDestroy(rid);
	if (rc < 0)
		return false;
	std::memcpy(out_ip, &addr, 4);
	return true;
}
