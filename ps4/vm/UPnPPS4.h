// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2: minimal UPnP-IGD client so the Sala LAN host can open its UDP port on the home router.
// SSDP discovery (multicast 239.255.255.250:1900), the device description over HTTP, then SOAP AddPortMapping /
// GetExternalIPAddress / DeletePortMapping on the WANIPConnection (or WANPPPConnection) service. Plain BSD sockets.
#pragma once

#include <string>

namespace UPnP
{
	struct MapResult
	{
		bool mapped = false;
		std::string externalIP; // what friends type to join (empty if the router did not say)
		std::string error;      // why it failed, for the log
	};

	// Blocking (a few seconds at most). localIP is the PS4's own address on the home network.
	MapResult MapUdpPort(unsigned short port, const std::string& localIP);
	void UnmapUdpPort(unsigned short port);

	// 10/8, 172.16/12, 192.168/16 and 100.64/10 (carrier-grade NAT): a router WAN address no one outside can reach.
	bool IsUnreachableFromInternet(const std::string& ip);
} // namespace UPnP

// Small PS4 system notification (top-left bubble), used for room events the player has to see.
void ForjaNotify(const std::string& message);
