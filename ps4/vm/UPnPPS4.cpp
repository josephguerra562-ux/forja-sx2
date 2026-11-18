// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2: see UPnPPS4.h.
#include "UPnPPS4.h"

#include "common/Console.h"

#include <orbis/libkernel.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
	struct Gateway
	{
		std::string host; // dotted IP of the router's HTTP server
		unsigned short port = 80;
		std::string controlPath;
		std::string serviceType;
	};

	std::mutex s_lock;
	Gateway s_gateway; // remembered from MapUdpPort for UnmapUdpPort

	bool WaitFd(int fd, short events, int timeoutMs)
	{
		pollfd p{};
		p.fd = fd;
		p.events = events;
		return poll(&p, 1, timeoutMs) > 0 && (p.revents & events);
	}

	std::string Lower(std::string s)
	{
		for (char& c : s)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		return s;
	}

	// "http://192.168.1.1:5000/rootDesc.xml" -> host, port, path
	bool ParseUrl(const std::string& url, std::string* host, unsigned short* port, std::string* path)
	{
		std::string rest = url;
		const size_t scheme = rest.find("://");
		if (scheme != std::string::npos)
			rest = rest.substr(scheme + 3);
		const size_t slash = rest.find('/');
		std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
		*path = slash == std::string::npos ? "/" : rest.substr(slash);
		*port = 80;
		const size_t colon = hostport.find(':');
		if (colon != std::string::npos)
		{
			*port = static_cast<unsigned short>(std::atoi(hostport.c_str() + colon + 1));
			hostport.resize(colon);
		}
		*host = hostport;
		return !host->empty();
	}

	// Blocking HTTP/1.0 exchange with a 3 s budget per step. Returns the whole response (headers + body).
	std::string Http(const std::string& host, unsigned short port, const std::string& request)
	{
		sockaddr_in addr{};
		addr.sin_len = sizeof(addr);
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
			return {};
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return {};
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		std::string response;
		const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
		if (rc == 0 || (errno == EINPROGRESS && WaitFd(fd, POLLOUT, 3000)))
		{
			size_t sent = 0;
			while (sent < request.size() && WaitFd(fd, POLLOUT, 3000))
			{
				const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
				if (n <= 0)
					break;
				sent += static_cast<size_t>(n);
			}
			char buf[2048];
			while (sent == request.size() && WaitFd(fd, POLLIN, 3000))
			{
				const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
				if (n <= 0)
					break;
				response.append(buf, static_cast<size_t>(n));
				if (response.size() > 256 * 1024)
					break;
			}
		}
		::close(fd);
		return response;
	}

	std::string Tag(const std::string& xml, const std::string& name, size_t from = 0)
	{
		const std::string lx = Lower(xml);
		const std::string open = "<" + Lower(name) + ">";
		const size_t a = lx.find(open, from);
		if (a == std::string::npos)
			return {};
		const size_t start = a + open.size();
		const size_t b = lx.find("</" + Lower(name) + ">", start);
		if (b == std::string::npos)
			return {};
		return xml.substr(start, b - start);
	}

	// SSDP M-SEARCH for an Internet Gateway Device; returns the LOCATION URL of the first answer.
	std::string Discover()
	{
		const int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return {};
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		sockaddr_in to{};
		to.sin_len = sizeof(to);
		to.sin_family = AF_INET;
		to.sin_port = htons(1900);
		inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
		static const char* kTargets[] = {
			"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
			"urn:schemas-upnp-org:device:InternetGatewayDevice:2",
			"urn:schemas-upnp-org:service:WANIPConnection:1",
		};
		std::string location;
		for (int round = 0; round < 2 && location.empty(); round++)
		{
			for (const char* st : kTargets)
			{
				char msg[512];
				const int len = std::snprintf(msg, sizeof(msg),
					"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", st);
				sendto(fd, msg, static_cast<size_t>(len), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
			}
			for (int waited = 0; waited < 2500 && location.empty(); waited += 250)
			{
				if (!WaitFd(fd, POLLIN, 250))
					continue;
				char buf[2048];
				const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
				if (n <= 0)
					continue;
				buf[n] = '\0';
				const std::string reply(buf);
				const std::string lr = Lower(reply);
				const size_t at = lr.find("\nlocation:");
				if (at == std::string::npos)
					continue;
				size_t start = at + 10;
				while (start < reply.size() && reply[start] == ' ')
					start++;
				const size_t end = reply.find('\r', start);
				location = reply.substr(start, end == std::string::npos ? std::string::npos : end - start);
			}
		}
		::close(fd);
		return location;
	}

	bool FindService(const std::string& location, Gateway* gw, std::string* error)
	{
		std::string host, path;
		unsigned short port;
		if (!ParseUrl(location, &host, &port, &path))
		{
			*error = "URL del router no válida: " + location;
			return false;
		}
		const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + ":" + std::to_string(port) +
								"\r\nConnection: close\r\n\r\n";
		const std::string xml = Http(host, port, req);
		if (xml.empty())
		{
			*error = "el router no respondió a " + location;
			return false;
		}
		static const char* kServices[] = {
			"urn:schemas-upnp-org:service:WANIPConnection:2",
			"urn:schemas-upnp-org:service:WANIPConnection:1",
			"urn:schemas-upnp-org:service:WANPPPConnection:1",
		};
		const std::string lx = Lower(xml);
		for (const char* svc : kServices)
		{
			const size_t at = lx.find(Lower(svc));
			if (at == std::string::npos)
				continue;
			std::string control = Tag(xml, "controlURL", at);
			if (control.empty())
				continue;
			gw->serviceType = svc;
			gw->host = host;
			gw->port = port;
			if (control.rfind("http", 0) == 0)
			{
				std::string h2, p2;
				unsigned short port2;
				if (ParseUrl(control, &h2, &port2, &p2))
				{
					gw->host = h2;
					gw->port = port2;
					control = p2;
				}
			}
			if (control.empty() || control[0] != '/')
				control = "/" + control;
			gw->controlPath = control;
			return true;
		}
		*error = "el router no ofrece WANIPConnection";
		return false;
	}

	std::string Soap(const Gateway& gw, const char* action, const std::string& args, int* status)
	{
		const std::string body = std::string("<?xml version=\"1.0\"?>\r\n"
											 "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
											 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
											 "<u:") +
								 action + " xmlns:u=\"" + gw.serviceType + "\">" + args + "</u:" + action +
								 "></s:Body></s:Envelope>\r\n";
		const std::string req = "POST " + gw.controlPath + " HTTP/1.0\r\nHost: " + gw.host + ":" + std::to_string(gw.port) +
								"\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gw.serviceType + "#" +
								action + "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
								body;
		const std::string resp = Http(gw.host, gw.port, req);
		*status = 0;
		if (resp.size() > 12 && resp.rfind("HTTP/", 0) == 0)
			*status = std::atoi(resp.c_str() + resp.find(' ') + 1);
		return resp;
	}

	std::string MappingArgs(unsigned short port, const std::string& localIP, int lease)
	{
		const std::string p = std::to_string(port);
		return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p +
			   "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + p +
			   "</NewInternalPort><NewInternalClient>" + localIP +
			   "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Forja SX2 Sala LAN"
			   "</NewPortMappingDescription><NewLeaseDuration>" +
			   std::to_string(lease) + "</NewLeaseDuration>";
	}
} // namespace

UPnP::MapResult UPnP::MapUdpPort(unsigned short port, const std::string& localIP)
{
	MapResult result;
	const std::string location = Discover();
	if (location.empty())
	{
		result.error = "ningún router respondió por UPnP (¿UPnP apagado en el router?)";
		return result;
	}
	Gateway gw;
	if (!FindService(location, &gw, &result.error))
		return result;

	int status = 0;
	Soap(gw, "AddPortMapping", MappingArgs(port, localIP, 0), &status);
	if (status != 200) // some routers refuse permanent leases (error 725): ask for 2 hours, renewed while the room is up
		Soap(gw, "AddPortMapping", MappingArgs(port, localIP, 7200), &status);
	if (status != 200)
	{
		result.error = "el router rechazó abrir el puerto (HTTP " + std::to_string(status) + ")";
		return result;
	}
	result.mapped = true;
	const std::string ext = Soap(gw, "GetExternalIPAddress", "", &status);
	if (status == 200)
		result.externalIP = Tag(ext, "NewExternalIPAddress");
	{
		std::lock_guard lock(s_lock);
		s_gateway = gw;
	}
	return result;
}

void UPnP::UnmapUdpPort(unsigned short port)
{
	Gateway gw;
	{
		std::lock_guard lock(s_lock);
		gw = s_gateway;
		s_gateway = {};
	}
	if (gw.host.empty())
		return;
	int status = 0;
	Soap(gw, "DeletePortMapping",
		"<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
		&status);
	Console.WriteLn("Sala LAN: UPnP port %u closed (HTTP %d)", port, status);
}

bool UPnP::IsUnreachableFromInternet(const std::string& ip)
{
	in_addr a{};
	if (inet_pton(AF_INET, ip.c_str(), &a) != 1)
		return true;
	const unsigned int h = ntohl(a.s_addr);
	return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 22) == (0x64400000u >> 22) || h == 0;
}

void ForjaNotify(const std::string& message)
{
	OrbisNotificationRequest req;
	std::memset(&req, 0, sizeof(req));
	req.type = NotificationRequest;
	req.targetId = -1;
	req.useIconImageUri = 1;
	std::snprintf(req.message, sizeof(req.message), "%s", message.c_str());
	std::snprintf(req.iconUri, sizeof(req.iconUri), "%s", "cxml://psnotification/tex_icon_system");
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	Console.WriteLn("Sala LAN: %s", message.c_str());
}
