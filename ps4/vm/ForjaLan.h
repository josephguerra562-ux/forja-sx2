// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 "Sala LAN": the PS2 network adapter as a virtual LAN cable between Forjas.
//
// One Forja creates the room (the host) and the others join it by IP or domain. Every Ethernet frame the PS2 sends
// travels in a UDP datagram to the host, which works as a network switch: it delivers to its own PS2 and forwards to
// the other players (broadcasts to everyone, unicasts to whoever owns the destination MAC). So games with a LAN mode
// (the same ones XLink Kai tunnels) play over the internet with no community server: the host carries the match.
//
// Each player gets a fixed address on the virtual LAN from its slot: 192.168.242.(10 + slot), handed to its PS2 by
// the internal DHCP server. The host is slot 0. Settings: [DEV9/ForjaLAN] in pcsx2/Config.h (LanHost, LanPort,
// LanUPnP, LanKey). The host opens its UDP port on the router through UPnP (ps4/vm/UPnPPS4.h) when it can.
#pragma once

#include "DEV9/net.h"
#include "DEV9/AdapterUtils.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

class ForjaLanAdapter : public NetAdapter
{
public:
	static constexpr int MaxPlayers = 8;

	ForjaLanAdapter();
	~ForjaLanAdapter() override;

	bool blocks() override { return false; }
	bool isInitialised() override { return m_initialised; }
	bool recv(NetPacket* pkt) override;
	bool send(NetPacket* pkt) override;
	void reloadSettings() override;
	void close() override;

private:
	using Clock = std::chrono::steady_clock;

	struct Peer
	{
		sockaddr_in addr{};
		PacketReader::MAC_Address mac{};
		int slot = -1;
		Clock::time_point lastSeen{};
	};

	bool m_initialised = false;
	bool m_isHost = false;
	int m_socket = -1;
	u16 m_port = 41020;
	std::string m_key;
	std::string m_serial;

	// Client side: the host's address and our place in the room.
	sockaddr_in m_hostAddr{};
	std::atomic<int> m_slot{-1};
	Clock::time_point m_lastHello{};
	Clock::time_point m_lastFromHost{};
	bool m_hostLost = false;
	bool m_rejected = false;
	int m_appliedSlot = -1; // the slot whose address the internal DHCP server hands out (net thread / constructor only)
	std::atomic<bool> m_closing{false};

	// Host side: who is in the room (guarded by m_peersLock; send() runs on the EE thread, recv() on the net thread).
	std::mutex m_peersLock;
	std::vector<Peer> m_peers;
	Clock::time_point m_lastPing{};

	AdapterUtils::Adapter m_adapter{};
	AdapterUtils::AdapterBuffer m_adapterBuffer;

	// UPnP runs on its own thread: the router can take seconds to answer and the game must not wait for it.
	std::thread m_upnpThread;
	std::atomic<bool> m_upnpMapped{false};

	static PacketReader::IP::IP_Address SlotIP(int slot);
	void ApplySlot(int slot);
	void SendTo(const sockaddr_in& to, u8 type, const void* body, size_t bodyLen);
	void SendHello();
	bool ResolveHost(const std::string& host, sockaddr_in* out);

	// Host switch: deliver a frame that came from `fromSlot` (-1 = our own PS2) to everyone it is meant for.
	// Returns true (and fills pkt) when the frame is also for our own PS2.
	bool HostRoute(const u8* frame, int len, const sockaddr_in* from, NetPacket* localOut);
	void HostHandleHello(const sockaddr_in& from, const u8* body, int len);
	void HostHousekeeping(Clock::time_point now);
	void ClientHousekeeping(Clock::time_point now);
	// One datagram from the host; true (pkt filled) when it carried a frame for our PS2.
	bool ClientHandle(const u8* dgram, int n, NetPacket* pkt, bool live);
	bool FrameIsForUs(const u8* frame) const;
};
