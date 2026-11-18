// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 "Sala LAN": see ForjaLan.h.
//
// Wire format (UDP, every datagram): "FJL1" + type byte + body.
//   HELLO   client->host  mac[6] keyLen key serialLen serial      (repeated each second until WELCOME)
//   WELCOME host->client  slot serialLen serial
//   REJECT  host->client  reason (1 wrong key, 2 room full, 3 same MAC as someone in the room)
//   FRAME   both ways     one raw Ethernet frame of the PS2
//   PING    both ways     keep-alive every 2 s (also keeps the routers' NAT mappings open)
//   BYE     both ways     leaving the room
#include "ForjaLan.h"
#include "UPnPPS4.h"

#include "DEV9/PacketReader/MAC_Address.h"
#include "common/Console.h"
#include "VMManager.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace PacketReader;
using namespace PacketReader::IP;

bool Ps4ResolveHost(const char* host, u8 out_ip[4]);

namespace
{
	constexpr u8 kMagic[4] = {'F', 'J', 'L', '1'};
	enum : u8
	{
		T_HELLO = 1,
		T_WELCOME = 2,
		T_REJECT = 3,
		T_FRAME = 4,
		T_PING = 5,
		T_BYE = 6,
	};
	constexpr int kHeader = 5;
	constexpr auto kPingEvery = std::chrono::seconds(2);
	constexpr auto kPeerTimeout = std::chrono::seconds(15);
	constexpr auto kHelloEvery = std::chrono::milliseconds(1000);
	const IP_Address kMask{{{255, 255, 255, 0}}};
	const IP_Address kGateway{{{192, 168, 242, 1}}};

	bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b)
	{
		return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
	}

	bool IsGroupMAC(const u8* mac) { return (mac[0] & 1) != 0; } // broadcast or multicast

	std::string EndpointText(const sockaddr_in& a)
	{
		char ip[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
		return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
	}

	std::string IPText(const IP_Address& ip)
	{
		return std::to_string(ip.bytes[0]) + "." + std::to_string(ip.bytes[1]) + "." + std::to_string(ip.bytes[2]) + "." +
			   std::to_string(ip.bytes[3]);
	}

	// Reads a length-prefixed string from a body; false if it runs past the end.
	bool ReadString(const u8*& p, const u8* end, std::string* out)
	{
		if (p >= end)
			return false;
		const size_t len = *p++;
		if (static_cast<size_t>(end - p) < len)
			return false;
		out->assign(reinterpret_cast<const char*>(p), len);
		p += len;
		return true;
	}

	void WriteString(std::vector<u8>& out, const std::string& s)
	{
		const size_t len = std::min<size_t>(s.size(), 255);
		out.push_back(static_cast<u8>(len));
		out.insert(out.end(), s.begin(), s.begin() + len);
	}
} // namespace

IP_Address ForjaLanAdapter::SlotIP(int slot)
{
	return IP_Address{{{192, 168, 242, static_cast<u8>(10 + slot)}}};
}

ForjaLanAdapter::ForjaLanAdapter()
{
	const auto& cfg = EmuConfig.DEV9;
	m_isHost = cfg.LanHost.empty();
	m_port = static_cast<u16>(cfg.LanPort > 0 && cfg.LanPort < 65536 ? cfg.LanPort : 41020);
	m_key = cfg.LanKey;
	m_serial = VMManager::GetDiscSerial();

	// Compilation 144: a room needs a key; without one anybody who finds the port could come in.
	if (m_isHost && m_key.empty())
	{
		ForjaNotify("Sala LAN: ponle una clave a la sala para abrirla (menú del emulador > Extras > Sala LAN, clave)");
		return;
	}

	if (!AdapterUtils::GetAdapterAuto(&m_adapter, &m_adapterBuffer))
	{
		ForjaNotify("Sala LAN: la PS4 no tiene internet");
		return;
	}

	// A MAC of our own, so two Forjas in the room never share one: Sony's prefix plus the end of the PS4's MAC.
	MAC_Address mac = defaultMAC;
	if (const std::optional<MAC_Address> hostMac = AdapterUtils::GetAdapterMAC(&m_adapter))
	{
		mac.bytes[3] = hostMac->bytes[3];
		mac.bytes[4] = hostMac->bytes[4];
		mac.bytes[5] = hostMac->bytes[5];
	}
	SetMACAddress(&mac);

	m_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_socket < 0)
	{
		Console.Error("Sala LAN: socket() failed (%d)", errno);
		return;
	}
	fcntl(m_socket, F_SETFL, fcntl(m_socket, F_GETFL, 0) | O_NONBLOCK);
	const int one = 1;
	setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	const int buf = 256 * 1024;
	setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
	setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

	sockaddr_in local{};
	local.sin_len = sizeof(local);
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(m_isHost ? m_port : 0);
	if (bind(m_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
	{
		ForjaNotify("Sala LAN: el puerto UDP " + std::to_string(m_port) + " está ocupado");
		::close(m_socket);
		m_socket = -1;
		return;
	}

	const std::optional<IP_Address> lanIP = AdapterUtils::GetAdapterIP(&m_adapter);
	const std::string lanText = lanIP.has_value() ? IPText(*lanIP) : std::string("?");

	if (m_isHost)
	{
		m_slot = 0;
		InitInternalServer(&m_adapter, true, SlotIP(0), kMask, kGateway);
		ForjaNotify("Sala LAN creada. En tu casa: " + lanText + "  puerto " + std::to_string(m_port));
		if (cfg.LanUPnP)
		{
			m_upnpThread = std::thread([this, lanText] {
				// Map, then renew every 30 min while the room is open (some routers only grant 2-hour leases).
				auto lastMap = Clock::now() - std::chrono::hours(1);
				while (!m_closing.load())
				{
					if (Clock::now() - lastMap >= std::chrono::minutes(30))
					{
						const bool first = !m_upnpMapped.load();
						const UPnP::MapResult r = UPnP::MapUdpPort(m_port, lanText);
						lastMap = Clock::now();
						if (r.mapped)
						{
							m_upnpMapped = true;
							if (first)
							{
								if (r.externalIP.empty())
									ForjaNotify("Sala LAN: puerto abierto por UPnP. Tu IP pública: búscala en tu router");
								else if (UPnP::IsUnreachableFromInternet(r.externalIP))
									ForjaNotify("Sala LAN: tu internet usa CGNAT (" + r.externalIP +
												"). Desde fuera no te pueden encontrar; solo en tu misma red");
								else
									ForjaNotify("Sala LAN: tus amigos entran con " + r.externalIP + "  puerto " +
												std::to_string(m_port));
							}
						}
						else if (first)
						{
							Console.Warning("Sala LAN: UPnP: %s", r.error.c_str());
							ForjaNotify("Sala LAN: no se pudo abrir el puerto solo. Abre UDP " + std::to_string(m_port) +
										" en tu router hacia " + lanText);
						}
					}
					for (int i = 0; i < 10 && !m_closing.load(); i++)
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
			});
		}
	}
	else
	{
		if (!ResolveHost(cfg.LanHost, &m_hostAddr))
		{
			ForjaNotify("Sala LAN: no encuentro al anfitrión " + cfg.LanHost);
			::close(m_socket);
			m_socket = -1;
			return;
		}
		Console.WriteLn("Sala LAN: joining %s from %s", EndpointText(m_hostAddr).c_str(), lanText.c_str());

		// Wait up to 4 s for our slot, so the PS2 gets its final address from the first DHCP answer.
		const auto deadline = Clock::now() + std::chrono::seconds(4);
		NetPacket scratch;
		while (m_slot.load() < 0 && Clock::now() < deadline)
		{
			if (Clock::now() - m_lastHello >= std::chrono::milliseconds(500))
				SendHello();
			pollfd p{};
			p.fd = m_socket;
			p.events = POLLIN;
			if (poll(&p, 1, 100) <= 0)
				continue;
			u8 dgram[2048];
			sockaddr_in from{};
			socklen_t fromLen = sizeof(from);
			const ssize_t n = recvfrom(m_socket, dgram, sizeof(dgram), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
			if (n >= kHeader && SameEndpoint(from, m_hostAddr))
				ClientHandle(dgram, static_cast<int>(n), &scratch, false);
			if (m_rejected)
				break;
		}

		if (m_slot.load() >= 0)
			InitInternalServer(&m_adapter, true, SlotIP(m_slot.load()), kMask, kGateway);
		else
		{
			// Not in the room yet: a provisional address from our MAC, replaced when the host answers.
			const int provisional = 100 + (mac.bytes[5] % 100);
			InitInternalServer(&m_adapter, true, SlotIP(provisional), kMask, kGateway);
			if (!m_rejected)
				ForjaNotify("Sala LAN: el anfitrión " + cfg.LanHost + " no responde todavía; sigo intentando");
		}
		m_appliedSlot = m_slot.load();
	}

	m_initialised = true;
}

ForjaLanAdapter::~ForjaLanAdapter()
{
	close();
}

void ForjaLanAdapter::close()
{
	if (m_closing.exchange(true))
		return;
	if (m_socket >= 0)
	{
		if (m_isHost)
		{
			std::lock_guard lock(m_peersLock);
			for (const Peer& p : m_peers)
				SendTo(p.addr, T_BYE, nullptr, 0);
			m_peers.clear();
		}
		else if (m_slot.load() >= 0)
			SendTo(m_hostAddr, T_BYE, nullptr, 0);
	}
	if (m_upnpThread.joinable())
		m_upnpThread.join();
	if (m_upnpMapped.load())
		UPnP::UnmapUdpPort(m_port);
	if (m_socket >= 0)
	{
		::close(m_socket);
		m_socket = -1;
	}
	m_initialised = false;
}

void ForjaLanAdapter::reloadSettings()
{
	// Host, port, key and UPnP changes restart the adapter (net.cpp ReconfigureLiveNet). Logging flags need nothing.
}

bool ForjaLanAdapter::ResolveHost(const std::string& host, sockaddr_in* out)
{
	std::string name = host;
	u16 port = m_port;
	const size_t colon = name.rfind(':');
	if (colon != std::string::npos) // "dominio.duckdns.org:41020"
	{
		port = static_cast<u16>(std::atoi(name.c_str() + colon + 1));
		name.resize(colon);
	}
	std::memset(out, 0, sizeof(*out));
	out->sin_len = sizeof(*out);
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	if (inet_pton(AF_INET, name.c_str(), &out->sin_addr) == 1)
		return true;
	u8 ip[4];
	if (!Ps4ResolveHost(name.c_str(), ip))
		return false;
	std::memcpy(&out->sin_addr.s_addr, ip, 4);
	return true;
}

void ForjaLanAdapter::SendTo(const sockaddr_in& to, u8 type, const void* body, size_t bodyLen)
{
	if (m_socket < 0 || bodyLen > 1800)
		return;
	u8 dgram[kHeader + 1800];
	std::memcpy(dgram, kMagic, 4);
	dgram[4] = type;
	if (bodyLen)
		std::memcpy(dgram + kHeader, body, bodyLen);
	sendto(m_socket, dgram, kHeader + bodyLen, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void ForjaLanAdapter::SendHello()
{
	std::vector<u8> body(ps2MAC.bytes, ps2MAC.bytes + 6);
	WriteString(body, m_key);
	WriteString(body, m_serial);
	SendTo(m_hostAddr, T_HELLO, body.data(), body.size());
	m_lastHello = Clock::now();
}

void ForjaLanAdapter::ApplySlot(int slot)
{
	m_slot = slot;
	if (m_initialised && slot != m_appliedSlot)
	{
		// The PS2 asked DHCP before the host answered: hand it the right address on its next renewal.
		ReloadInternalServer(&m_adapter, true, SlotIP(slot), kMask, kGateway);
		m_appliedSlot = slot;
	}
}

bool ForjaLanAdapter::FrameIsForUs(const u8* frame) const
{
	const MAC_Address* dst = reinterpret_cast<const MAC_Address*>(frame);
	const MAC_Address* src = reinterpret_cast<const MAC_Address*>(frame + 6);
	if (*src == ps2MAC)
		return false;
	return *dst == ps2MAC || IsGroupMAC(frame);
}

// ---------------------------------------------------------------------------------------------------------------------
// Client

bool ForjaLanAdapter::ClientHandle(const u8* dgram, int n, NetPacket* pkt, bool live)
{
	if (std::memcmp(dgram, kMagic, 4) != 0)
		return false;
	m_lastFromHost = Clock::now();
	if (m_hostLost)
	{
		m_hostLost = false;
		ForjaNotify("Sala LAN: conexión con el anfitrión recuperada");
	}
	const u8* body = dgram + kHeader;
	const int len = n - kHeader;
	switch (dgram[4])
	{
		case T_WELCOME:
		{
			// Compilation 144: a slot from the host is one of the room's eight, or nothing (the address is 192.168.242.(10 + slot)).
			if (len < 1 || body[0] >= MaxPlayers)
				return false;
			const int slot = body[0];
			const u8* p = body + 1;
			std::string hostSerial;
			ReadString(p, body + len, &hostSerial);
			const bool fresh = m_slot.load() != slot;
			ApplySlot(slot);
			if (fresh)
			{
				ForjaNotify("Sala LAN: entraste como jugador " + std::to_string(slot + 1) + " (" + IPText(SlotIP(slot)) + ")");
				if (!hostSerial.empty() && !m_serial.empty() && hostSerial != m_serial)
					ForjaNotify("Sala LAN: el anfitrión tiene otro juego o versión (" + hostSerial + ")");
			}
			(void)live;
			return false;
		}
		case T_REJECT:
		{
			const int reason = len >= 1 ? body[0] : 0;
			if (!m_rejected)
			{
				m_rejected = true;
				ForjaNotify(reason == 1 ? "Sala LAN: clave equivocada" :
							reason == 2 ? "Sala LAN: la sala está llena (8 jugadores)" :
							reason == 3 ? "Sala LAN: otra PS4 de la sala tiene tu misma MAC" :
										  "Sala LAN: el anfitrión no te dejó entrar");
			}
			return false;
		}
		case T_FRAME:
		{
			if (len < 14 || len > static_cast<int>(sizeof(pkt->buffer)) || !FrameIsForUs(body))
				return false;
			std::memcpy(pkt->buffer, body, static_cast<size_t>(len));
			pkt->size = len;
			return true;
		}
		case T_BYE:
			if (m_slot.load() >= 0)
				ForjaNotify("Sala LAN: el anfitrión cerró la sala");
			m_slot = -1;
			return false;
		default: // PING
			return false;
	}
}

void ForjaLanAdapter::ClientHousekeeping(Clock::time_point now)
{
	if (m_rejected)
		return;
	if (m_slot.load() < 0)
	{
		if (now - m_lastHello >= kHelloEvery)
			SendHello();
		return;
	}
	if (now - m_lastPing >= kPingEvery)
	{
		SendTo(m_hostAddr, T_PING, nullptr, 0);
		m_lastPing = now;
	}
	if (!m_hostLost && now - m_lastFromHost >= kPeerTimeout)
	{
		m_hostLost = true;
		ForjaNotify("Sala LAN: se perdió la conexión con el anfitrión");
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Host

void ForjaLanAdapter::HostHandleHello(const sockaddr_in& from, const u8* body, int len)
{
	if (len < 6)
		return;
	MAC_Address mac;
	std::memcpy(mac.bytes, body, 6);
	const u8* p = body + 6;
	std::string key, serial;
	ReadString(p, body + len, &key);
	ReadString(p, body + len, &serial);

	auto reject = [&](u8 reason) { SendTo(from, T_REJECT, &reason, 1); };
	auto welcome = [&](int slot) {
		std::vector<u8> out{static_cast<u8>(slot)};
		WriteString(out, m_serial);
		SendTo(from, T_WELCOME, out.data(), out.size());
	};

	if (key != m_key)
	{
		reject(1);
		return;
	}

	std::lock_guard lock(m_peersLock);
	for (Peer& peer : m_peers)
	{
		if (SameEndpoint(peer.addr, from)) // HELLO repeated (our WELCOME got lost)
		{
			peer.lastSeen = Clock::now();
			welcome(peer.slot);
			return;
		}
	}
	if (mac == ps2MAC)
	{
		reject(3);
		return;
	}
	for (const Peer& peer : m_peers)
	{
		if (peer.mac == mac)
		{
			reject(3);
			return;
		}
	}
	int slot = -1;
	for (int s = 1; s < MaxPlayers && slot < 0; s++)
	{
		bool used = false;
		for (const Peer& peer : m_peers)
			used |= peer.slot == s;
		if (!used)
			slot = s;
	}
	if (slot < 0)
	{
		reject(2);
		return;
	}
	Peer peer;
	peer.addr = from;
	peer.mac = mac;
	peer.slot = slot;
	peer.lastSeen = Clock::now();
	m_peers.push_back(peer);
	welcome(slot);
	ForjaNotify("Sala LAN: entró el jugador " + std::to_string(slot + 1) + " (" + EndpointText(from) + ")");
	if (!serial.empty() && !m_serial.empty() && serial != m_serial)
		ForjaNotify("Sala LAN: el jugador " + std::to_string(slot + 1) + " tiene otro juego o versión (" + serial + ")");
}

bool ForjaLanAdapter::HostRoute(const u8* frame, int len, const sockaddr_in* from, NetPacket* localOut)
{
	const MAC_Address* dst = reinterpret_cast<const MAC_Address*>(frame);
	bool forLocal = false;
	{
		std::lock_guard lock(m_peersLock);
		if (from != nullptr && *dst == ps2MAC)
			forLocal = true;
		else if (IsGroupMAC(frame))
		{
			forLocal = from != nullptr;
			for (const Peer& peer : m_peers)
				if (from == nullptr || !SameEndpoint(peer.addr, *from))
					SendTo(peer.addr, T_FRAME, frame, static_cast<size_t>(len));
		}
		else
		{
			const Peer* target = nullptr;
			for (const Peer& peer : m_peers)
				if (peer.mac == *dst)
					target = &peer;
			if (target != nullptr)
			{
				if (from == nullptr || !SameEndpoint(target->addr, *from))
					SendTo(target->addr, T_FRAME, frame, static_cast<size_t>(len));
			}
			else // unknown destination: like a switch, flood it
			{
				for (const Peer& peer : m_peers)
					if (from == nullptr || !SameEndpoint(peer.addr, *from))
						SendTo(peer.addr, T_FRAME, frame, static_cast<size_t>(len));
			}
		}
	}
	if (forLocal && localOut != nullptr && FrameIsForUs(frame) && len <= static_cast<int>(sizeof(localOut->buffer)))
	{
		std::memcpy(localOut->buffer, frame, static_cast<size_t>(len));
		localOut->size = len;
		return true;
	}
	return false;
}

void ForjaLanAdapter::HostHousekeeping(Clock::time_point now)
{
	std::vector<int> gone;
	{
		std::lock_guard lock(m_peersLock);
		const bool ping = now - m_lastPing >= kPingEvery;
		for (auto it = m_peers.begin(); it != m_peers.end();)
		{
			if (now - it->lastSeen >= kPeerTimeout)
			{
				gone.push_back(it->slot);
				it = m_peers.erase(it);
				continue;
			}
			if (ping)
				SendTo(it->addr, T_PING, nullptr, 0);
			++it;
		}
		if (ping)
			m_lastPing = now;
	}
	for (int slot : gone)
		ForjaNotify("Sala LAN: el jugador " + std::to_string(slot + 1) + " se desconectó");
}

// ---------------------------------------------------------------------------------------------------------------------
// NetAdapter

bool ForjaLanAdapter::recv(NetPacket* pkt)
{
	if (m_socket < 0)
		return false;
	if (NetAdapter::recv(pkt)) // DHCP answers from the internal server
		return true;

	const Clock::time_point now = Clock::now();
	if (m_isHost)
		HostHousekeeping(now);
	else
		ClientHousekeeping(now);

	for (int i = 0; i < 64; i++)
	{
		u8 dgram[2048];
		sockaddr_in from{};
		socklen_t fromLen = sizeof(from);
		const ssize_t n = recvfrom(m_socket, dgram, sizeof(dgram), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
		if (n <= 0)
			return false;
		if (n < kHeader || std::memcmp(dgram, kMagic, 4) != 0)
			continue;

		if (!m_isHost)
		{
			if (!SameEndpoint(from, m_hostAddr))
				continue;
			if (ClientHandle(dgram, static_cast<int>(n), pkt, true))
			{
				InspectRecv(pkt);
				return true;
			}
			continue;
		}

		const u8 type = dgram[4];
		const u8* body = dgram + kHeader;
		const int len = static_cast<int>(n) - kHeader;
		if (type == T_HELLO)
		{
			HostHandleHello(from, body, len);
			continue;
		}
		bool known = false;
		{
			std::lock_guard lock(m_peersLock);
			for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
			{
				if (!SameEndpoint(it->addr, from))
					continue;
				known = true;
				if (type == T_BYE)
				{
					const int slot = it->slot;
					m_peers.erase(it);
					ForjaNotify("Sala LAN: salió el jugador " + std::to_string(slot + 1));
				}
				else
				{
					it->lastSeen = now;
					if (type == T_FRAME && len >= 14)
						std::memcpy(it->mac.bytes, body + 6, 6); // learn the sender's MAC
				}
				break;
			}
		}
		if (!known || type != T_FRAME || len < 14)
			continue;
		if (HostRoute(body, len, &from, pkt))
		{
			InspectRecv(pkt);
			return true;
		}
	}
	return false;
}

bool ForjaLanAdapter::send(NetPacket* pkt)
{
	if (NetAdapter::send(pkt)) // DHCP/DNS to the internal server never leaves the PS4
		return true;
	if (m_socket < 0 || pkt->size < 14)
		return true;
	InspectSend(pkt);
	const u8* frame = reinterpret_cast<const u8*>(pkt->buffer);
	if (m_isHost)
		HostRoute(frame, pkt->size, nullptr, nullptr);
	else if (m_slot.load() >= 0)
		SendTo(m_hostAddr, T_FRAME, frame, static_cast<size_t>(pkt->size));
	return true;
}
