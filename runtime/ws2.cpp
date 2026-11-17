// WS2_32 on top of BSD sockets (LAN games: UDP broadcast discovery, TCP/UDP sessions; online services by host name).
//  * SOCKET handles index a table of host file descriptors;
//  * IPv4 sockaddr has the same layout on Windows and Linux, option/level/ioctl numbers and error codes are translated;
//  * overlapped WSARecv/WSARecvFrom complete on a network thread that polls the pending sockets and signals the
//    OVERLAPPED event (kernel event objects), WSAGetOverlappedResult reads the result back.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rt.h"

#define W "WS2_32.dll"
using namespace rt;

namespace
{
const uint32_t INVALID = 0xffffffffu, SOCKERR = 0xffffffffu;
const uint32_t kBase = 0x4000;   // SOCKET values: kBase + 4*index (non-zero, distinct from small integers)

struct Sock { int fd = -1; int type = 0; bool nonblock = false; };
std::mutex sockMu;
std::map<uint32_t, Sock> socks;
uint32_t nextSock = kBase;

int fdOf(uint32_t s)
{
	std::lock_guard<std::mutex> l(sockMu);
	auto it = socks.find(s);
	return it == socks.end() ? -1 : it->second.fd;
}

int logged = 0;
bool logOn() { return logged++ < 400; }

uint32_t wsaOf(int e)
{
	switch (e)
	{
	case EWOULDBLOCK: return 10035;
	case EINPROGRESS: return 10035;   // non-blocking connect: Windows reports WSAEWOULDBLOCK
	case EALREADY: return 10037;
	case ENOTSOCK: return 10038;
	case EDESTADDRREQ: return 10039;
	case EMSGSIZE: return 10040;
	case EPROTOTYPE: return 10041;
	case ENOPROTOOPT: return 10042;
	case EPROTONOSUPPORT: return 10043;
	case EOPNOTSUPP: return 10045;
	case EAFNOSUPPORT: return 10047;
	case EADDRINUSE: return 10048;
	case EADDRNOTAVAIL: return 10049;
	case ENETDOWN: return 10050;
	case ENETUNREACH: return 10051;
	case ECONNABORTED: return 10053;
	case ECONNRESET: return 10054;
	case ENOBUFS: return 10055;
	case EISCONN: return 10056;
	case ENOTCONN: return 10057;
	case ETIMEDOUT: return 10060;
	case ECONNREFUSED: return 10061;
	case EHOSTUNREACH: return 10065;
	case EINVAL: return 10022;
	case EBADF: return 10009;
	case EACCES: return 10013;
	case EFAULT: return 10014;
	case EMFILE: return 10024;
	default: return 10022;
	}
}
void fail(int e) { setLastError(wsaOf(e)); }

// Windows option numbers -> Linux; returns false when the option is accepted but ignored
bool mapOpt(int wl, int wo, int& l, int& o)
{
	if (wl == 0xffff)
	{
		l = SOL_SOCKET;
		switch (wo)
		{
		case 0x0004: o = SO_REUSEADDR; return true;
		case 0x0008: o = SO_KEEPALIVE; return true;
		case 0x0020: o = SO_BROADCAST; return true;
		case 0x0080: o = SO_LINGER; return true;
		case 0x0100: o = SO_OOBINLINE; return true;
		case 0x1001: o = SO_SNDBUF; return true;
		case 0x1002: o = SO_RCVBUF; return true;
		case 0x1005: o = SO_SNDTIMEO; return true;
		case 0x1006: o = SO_RCVTIMEO; return true;
		case 0x1007: o = SO_ERROR; return true;
		case 0x1008: o = SO_TYPE; return true;
		case 0x0002: o = SO_ACCEPTCONN; return true;
		default: return false;   // SO_EXCLUSIVEADDRUSE, SO_DONTLINGER, ...
		}
	}
	if (wl == 6) { l = IPPROTO_TCP; if (wo == 1) { o = TCP_NODELAY; return true; } return false; }
	if (wl == 0)
	{
		l = IPPROTO_IP;
		switch (wo)
		{
		case 3: o = IP_TOS; return true;
		case 4: o = IP_TTL; return true;
		case 9: o = IP_MULTICAST_IF; return true;
		case 10: o = IP_MULTICAST_TTL; return true;
		case 11: o = IP_MULTICAST_LOOP; return true;
		case 12: o = IP_ADD_MEMBERSHIP; return true;
		case 13: o = IP_DROP_MEMBERSHIP; return true;
		default: return false;
		}
	}
	return false;
}

std::string addrStr(const sockaddr_in& a)
{
	char b[64];
	snprintf(b, sizeof b, "%s:%u", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
	return b;
}
bool loadAddr(uint32_t p, uint32_t len, sockaddr_in& a)
{
	memset(&a, 0, sizeof a);
	if (!p || len < 8) return false;
	memcpy(&a, guest::host(p), std::min<uint32_t>(len, sizeof a));
	a.sin_family = AF_INET;   // Windows AF_INET is 2 as well; the field is little-endian u16 on both
	return true;
}
void storeAddr(uint32_t p, uint32_t lenPtr, const sockaddr_in& a)
{
	if (!p) return;
	uint32_t len = lenPtr ? guest::load<uint32_t>(lenPtr) : 16;
	sockaddr_in w = a;
	w.sin_family = 2;
	memcpy(guest::host(p), &w, std::min<uint32_t>(len, 16));
	if (lenPtr) guest::store<uint32_t>(lenPtr, 16);
}

// local IPv4 interfaces (excluding loopback unless nothing else)
struct Iface { in_addr addr, mask, bcast; uint32_t flags; };
std::vector<Iface> interfaces()
{
	std::vector<Iface> v, lo;
	ifaddrs* ifa = nullptr;
	if (getifaddrs(&ifa) == 0)
	{
		for (ifaddrs* i = ifa; i; i = i->ifa_next)
		{
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
			Iface f{};
			f.addr = ((sockaddr_in*)i->ifa_addr)->sin_addr;
			f.mask = i->ifa_netmask ? ((sockaddr_in*)i->ifa_netmask)->sin_addr : in_addr{0xffffffffu};
			f.bcast.s_addr = f.addr.s_addr | ~f.mask.s_addr;
			f.flags = 1 | ((i->ifa_flags & IFF_BROADCAST) ? 2 : 0) | ((i->ifa_flags & IFF_LOOPBACK) ? 4 : 0) | ((i->ifa_flags & IFF_MULTICAST) ? 0x10 : 0);
			((i->ifa_flags & IFF_LOOPBACK) ? lo : v).push_back(f);
		}
		freeifaddrs(ifa);
	}
	if (v.empty()) v = lo;
	return v;
}

// ---- overlapped receives
struct Pending
{
	uint32_t sock; int fd;
	uint32_t bufs, nbufs, recvdPtr, flagsPtr, from, fromLen, ov;
	bool withFrom;
};
std::mutex ovMu;
std::condition_variable ovCv;
std::vector<Pending> pending;
std::map<uint32_t, uint32_t> ovDone;   // OVERLAPPED address -> completed (1) / failed (2)
std::atomic<bool> netThread{false};

// receive into guest WSABUFs; returns bytes or -1 (errno)
ssize_t recvInto(int fd, uint32_t bufs, uint32_t nbufs, bool withFrom, uint32_t from, uint32_t fromLen, int flags)
{
	iovec iov[16];
	int n = (int)std::min<uint32_t>(nbufs, 16);
	for (int i = 0; i < n; i++)
	{
		iov[i].iov_len = guest::load<uint32_t>(bufs + 8 * i);
		iov[i].iov_base = guest::host(guest::load<uint32_t>(bufs + 8 * i + 4));
	}
	sockaddr_in a{};
	msghdr m{};
	m.msg_iov = iov; m.msg_iovlen = n;
	if (withFrom) { m.msg_name = &a; m.msg_namelen = sizeof a; }
	ssize_t r = ::recvmsg(fd, &m, flags);
	if (r >= 0 && withFrom && from) storeAddr(from, fromLen, a);
	return r;
}

void completeOv(uint32_t ov, uint32_t status, uint32_t bytes)
{
	guest::store<uint32_t>(ov, status);       // Internal
	guest::store<uint32_t>(ov + 4, bytes);    // InternalHigh
	{
		std::lock_guard<std::mutex> l(ovMu);
		ovDone[ov] = status == 0 ? 1 : 2;
	}
	ovCv.notify_all();
	if (uint32_t ev = guest::load<uint32_t>(ov + 16)) eventSet(ev);
}

void netLoop()
{
	for (;;)
	{
		std::vector<Pending> cur;
		{
			std::unique_lock<std::mutex> l(ovMu);
			ovCv.wait_for(l, std::chrono::milliseconds(50), [] { return !pending.empty(); });
			cur = pending;
		}
		if (cur.empty()) continue;
		std::vector<pollfd> pf;
		for (auto& p : cur) pf.push_back({p.fd, POLLIN, 0});
		int r = ::poll(pf.data(), pf.size(), 5);
		if (r <= 0) continue;
		for (size_t i = 0; i < cur.size(); i++)
		{
			if (!pf[i].revents) continue;
			Pending& p = cur[i];
			ssize_t n = recvInto(p.fd, p.bufs, p.nbufs, p.withFrom, p.from, p.fromLen, MSG_DONTWAIT);
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
			{
				std::lock_guard<std::mutex> l(ovMu);
				pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& q) { return q.ov == p.ov; }), pending.end());
			}
			if (p.flagsPtr) guest::store<uint32_t>(p.flagsPtr, 0);
			completeOv(p.ov, n >= 0 ? 0 : 0xc0000000u | wsaOf(errno), n >= 0 ? (uint32_t)n : 0);
		}
	}
}

void wsaRecv(CPU* c, bool withFrom)
{
	// WSARecv(s, bufs, n, recvd*, flags*, ov, completion) / WSARecvFrom(s, bufs, n, recvd*, flags*, from, fromlen*, ov, completion)
	uint32_t s = ARG(0), bufs = ARG(1), nbufs = ARG(2), recvd = ARG(3), flagsP = ARG(4);
	uint32_t from = withFrom ? ARG(5) : 0, fromLen = withFrom ? ARG(6) : 0, ov = withFrom ? ARG(7) : ARG(5);
	int fd = fdOf(s);
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	int fl = flagsP ? (int)(guest::load<uint32_t>(flagsP) & 3) : 0;
	ssize_t n = recvInto(fd, bufs, nbufs, withFrom, from, fromLen, fl | MSG_DONTWAIT);
	if (n >= 0)
	{
		if (recvd) guest::store<uint32_t>(recvd, (uint32_t)n);
		if (flagsP) guest::store<uint32_t>(flagsP, 0);
		if (ov) completeOv(ov, 0, (uint32_t)n);
		SET(0);
		return;
	}
	if ((errno == EAGAIN || errno == EWOULDBLOCK) && ov)
	{
		guest::store<uint32_t>(ov, 0x103);   // STATUS_PENDING
		{
			std::lock_guard<std::mutex> l(ovMu);
			ovDone.erase(ov);
			pending.push_back({s, fd, bufs, nbufs, recvd, flagsP, from, fromLen, ov, withFrom});
		}
		if (!netThread.exchange(true)) std::thread(netLoop).detach();
		ovCv.notify_all();
		setLastError(997);   // WSA_IO_PENDING
		SET(SOCKERR);
		return;
	}
	fail(errno);
	SET(SOCKERR);
}
}   // namespace

IMP_ID(W, "#115", WSAStartup, 8)
{
	uint32_t p = ARG(1);
	memset(guest::host(p), 0, 400);
	guest::store<uint16_t>(p, 0x202);
	guest::store<uint16_t>(p + 2, 0x202);
	putStr(p + 4, "WinSock 2.0");
	putStr(p + 4 + 257, "Running");
	guest::store<uint16_t>(p + 4 + 257 + 129, 32767);   // iMaxSockets
	guest::store<uint16_t>(p + 4 + 257 + 129 + 2, 65467);   // iMaxUdpDg
	log("ws2: WSAStartup(0x%x)", ARG(0));
	SET(0);
}
IMP_ID(W, "#116", WSACleanup, 0) { SET(0); }
IMP_ID(W, "#111", WSAGetLastError, 0) { SET(getLastError()); }

IMP_ID(W, "#23", socket, 12)
{
	int af = (int)ARG(0), type = (int)ARG(1), proto = (int)ARG(2);
	if (af != 2) { setLastError(10047); SET(INVALID); return; }
	int fd = ::socket(AF_INET, type, proto);
	if (fd < 0) { fail(errno); if (logOn()) log("ws2: socket(%d,%d,%d) failed errno %d", af, type, proto, errno); SET(INVALID); return; }
	std::lock_guard<std::mutex> l(sockMu);
	uint32_t s = nextSock; nextSock += 4;
	socks[s] = Sock{fd, type, false};
	if (logOn()) log("ws2: socket(type %d, proto %d) = 0x%x", type, proto, s);
	SET(s);
}
IMP_ID(W, "#3", closesocket, 4)
{
	int fd = -1;
	{
		std::lock_guard<std::mutex> l(sockMu);
		auto it = socks.find(ARG(0));
		if (it != socks.end()) { fd = it->second.fd; socks.erase(it); }
	}
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	// pending overlapped receives on it fail (Windows: WSA_OPERATION_ABORTED)
	std::vector<uint32_t> aborted;
	{
		std::lock_guard<std::mutex> l(ovMu);
		for (auto& p : pending) if (p.fd == fd) aborted.push_back(p.ov);
		pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& q) { return q.fd == fd; }), pending.end());
	}
	for (uint32_t ov : aborted) completeOv(ov, 0xc0000120u, 0);
	::close(fd);
	if (logOn()) log("ws2: closesocket(0x%x)", ARG(0));
	SET(0);
}
IMP_ID(W, "#2", bind, 12)
{
	int fd = fdOf(ARG(0));
	sockaddr_in a;
	if (fd < 0 || !loadAddr(ARG(1), ARG(2), a)) { setLastError(10038); SET(SOCKERR); return; }
	int r = ::bind(fd, (sockaddr*)&a, sizeof a);
	if (logOn()) log("ws2: bind(0x%x, %s) = %d errno %d", ARG(0), addrStr(a).c_str(), r, r ? errno : 0);
	if (r) { fail(errno); SET(SOCKERR); } else SET(0);
}
IMP_ID(W, "#4", connect, 12)
{
	int fd = fdOf(ARG(0));
	sockaddr_in a;
	if (fd < 0 || !loadAddr(ARG(1), ARG(2), a)) { setLastError(10038); SET(SOCKERR); return; }
	int r = ::connect(fd, (sockaddr*)&a, sizeof a);
	if (logOn()) log("ws2: connect(0x%x, %s) = %d errno %d", ARG(0), addrStr(a).c_str(), r, r ? errno : 0);
	if (r) { fail(errno); SET(SOCKERR); } else SET(0);
}
IMP_ID(W, "#1", accept, 12)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(INVALID); return; }
	sockaddr_in a{}; socklen_t al = sizeof a;
	int n = ::accept(fd, (sockaddr*)&a, &al);
	if (n < 0) { fail(errno); SET(INVALID); return; }
	storeAddr(ARG(1), ARG(2), a);
	std::lock_guard<std::mutex> l(sockMu);
	uint32_t s = nextSock; nextSock += 4;
	socks[s] = Sock{n, SOCK_STREAM, false};
	if (logOn()) log("ws2: accept(0x%x) = 0x%x from %s", ARG(0), s, addrStr(a).c_str());
	SET(s);
}
IMP_ID(W, "#13", listen, 8)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	if (::listen(fd, (int)ARG(1))) { fail(errno); SET(SOCKERR); } else SET(0);
}
IMP_ID(W, "#5", getpeername, 12)
{
	int fd = fdOf(ARG(0));
	sockaddr_in a{}; socklen_t al = sizeof a;
	if (fd < 0 || ::getpeername(fd, (sockaddr*)&a, &al)) { fail(fd < 0 ? ENOTSOCK : errno); SET(SOCKERR); return; }
	storeAddr(ARG(1), ARG(2), a);
	SET(0);
}
IMP_ID(W, "#6", getsockname, 12)
{
	int fd = fdOf(ARG(0));
	sockaddr_in a{}; socklen_t al = sizeof a;
	if (fd < 0 || ::getsockname(fd, (sockaddr*)&a, &al)) { fail(fd < 0 ? ENOTSOCK : errno); SET(SOCKERR); return; }
	storeAddr(ARG(1), ARG(2), a);
	SET(0);
}
IMP_ID(W, "#21", setsockopt, 20)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	int wl = (int)ARG(1), wo = (int)ARG(2), l, o;
	uint32_t v = ARG(3), len = ARG(4);
	if (!mapOpt(wl, wo, l, o)) { if (logOn()) log("ws2: setsockopt(level 0x%x, opt 0x%x) ignored", wl, wo); SET(0); return; }
	int r;
	if (o == SO_RCVTIMEO || o == SO_SNDTIMEO)
	{
		uint32_t ms = v ? guest::load<uint32_t>(v) : 0;
		timeval tv{(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
		r = ::setsockopt(fd, l, o, &tv, sizeof tv);
	}
	else if (o == SO_LINGER)
	{
		linger lg{v ? guest::load<uint16_t>(v) : 0, v ? guest::load<uint16_t>(v + 2) : 0};
		r = ::setsockopt(fd, l, o, &lg, sizeof lg);
	}
	else if (l == IPPROTO_IP && (o == IP_ADD_MEMBERSHIP || o == IP_DROP_MEMBERSHIP || o == IP_MULTICAST_IF))
		r = ::setsockopt(fd, l, o, guest::host(v), len);   // ip_mreq / in_addr: same layout
	else
	{
		int iv = 0;
		if (v && len >= 4) iv = (int)guest::load<uint32_t>(v);
		else if (v && len >= 1) iv = guest::load<uint8_t>(v);
		r = ::setsockopt(fd, l, o, &iv, sizeof iv);
	}
	if (logOn()) log("ws2: setsockopt(0x%x, level 0x%x, opt 0x%x) = %d", ARG(0), wl, wo, r);
	if (r) { fail(errno); SET(SOCKERR); } else SET(0);
}
IMP_ID(W, "#7", getsockopt, 20)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	int wl = (int)ARG(1), wo = (int)ARG(2), l, o;
	uint32_t v = ARG(3), lenP = ARG(4);
	if (!mapOpt(wl, wo, l, o)) { if (v) guest::store<uint32_t>(v, 0); SET(0); return; }
	int r;
	if (o == SO_RCVTIMEO || o == SO_SNDTIMEO)
	{
		timeval tv{}; socklen_t tl = sizeof tv;
		r = ::getsockopt(fd, l, o, &tv, &tl);
		if (!r && v) guest::store<uint32_t>(v, (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000));
	}
	else if (o == SO_LINGER)
	{
		linger lg{}; socklen_t ll = sizeof lg;
		r = ::getsockopt(fd, l, o, &lg, &ll);
		if (!r && v) { guest::store<uint16_t>(v, (uint16_t)lg.l_onoff); guest::store<uint16_t>(v + 2, (uint16_t)lg.l_linger); }
	}
	else
	{
		int iv = 0; socklen_t il = sizeof iv;
		r = ::getsockopt(fd, l, o, &iv, &il);
		if (o == SO_ERROR) iv = iv ? (int)wsaOf(iv) : 0;
		if (!r && v) guest::store<uint32_t>(v, (uint32_t)iv);
	}
	if (lenP) guest::store<uint32_t>(lenP, 4);
	if (r) { fail(errno); SET(SOCKERR); } else SET(0);
}
IMP_ID(W, "#10", ioctlsocket, 12)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	uint32_t cmd = ARG(1), arg = ARG(2);
	if (cmd == 0x8004667e)   // FIONBIO
	{
		bool nb = arg && guest::load<uint32_t>(arg);
		int f = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, nb ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
		std::lock_guard<std::mutex> l(sockMu);
		socks[ARG(0)].nonblock = nb;
		SET(0);
		return;
	}
	if (cmd == 0x4004667f)   // FIONREAD
	{
		int n = 0;
		::ioctl(fd, FIONREAD, &n);
		if (arg) guest::store<uint32_t>(arg, (uint32_t)n);
		SET(0);
		return;
	}
	if (logOn()) log("ws2: ioctlsocket(0x%x) unsupported", cmd);
	setLastError(10045);
	SET(SOCKERR);
}
IMP_ID(W, "#16", recv, 16)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	ssize_t r = ::recv(fd, guest::host(ARG(1)), ARG(2), (int)(ARG(3) & 3));
	if (r < 0) { fail(errno); SET(SOCKERR); } else SET((uint32_t)r);
}
IMP_ID(W, "#17", recvfrom, 24)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	sockaddr_in a{}; socklen_t al = sizeof a;
	ssize_t r = ::recvfrom(fd, guest::host(ARG(1)), ARG(2), (int)(ARG(3) & 3), (sockaddr*)&a, &al);
	if (r < 0) { fail(errno); SET(SOCKERR); return; }
	storeAddr(ARG(4), ARG(5), a);
	SET((uint32_t)r);
}
IMP_ID(W, "#19", send, 16)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	ssize_t r = ::send(fd, guest::host(ARG(1)), ARG(2), (int)(ARG(3) & 3) | MSG_NOSIGNAL);
	if (r < 0) { fail(errno); SET(SOCKERR); } else SET((uint32_t)r);
}
IMP_ID(W, "#20", sendto, 24)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	sockaddr_in a;
	ssize_t r;
	if (ARG(4) && loadAddr(ARG(4), ARG(5), a)) r = ::sendto(fd, guest::host(ARG(1)), ARG(2), (int)(ARG(3) & 3) | MSG_NOSIGNAL, (sockaddr*)&a, sizeof a);
	else r = ::send(fd, guest::host(ARG(1)), ARG(2), MSG_NOSIGNAL);
	static int ns = 0;
	if (ns++ < 30) log("ws2: sendto(0x%x, %u bytes, %s) = %d errno %d", ARG(0), ARG(2), ARG(4) ? addrStr(a).c_str() : "-", (int)r, r < 0 ? errno : 0);
	if (r < 0) { fail(errno); SET(SOCKERR); } else SET((uint32_t)r);
}
// fd_set: { u_int fd_count; SOCKET fd_array[64]; }
IMP_ID(W, "#18", select, 20)
{
	uint32_t sets[3] = {ARG(1), ARG(2), ARG(3)}, tv = ARG(4);
	std::vector<pollfd> pf;
	std::vector<std::pair<int, uint32_t>> who;   // (set index, socket)
	for (int k = 0; k < 3; k++)
	{
		if (!sets[k]) continue;
		uint32_t n = std::min<uint32_t>(guest::load<uint32_t>(sets[k]), 64);
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t s = guest::load<uint32_t>(sets[k] + 4 + 4 * i);
			int fd = fdOf(s);
			if (fd < 0) continue;
			pf.push_back({fd, (short)(k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI), 0});
			who.push_back({k, s});
		}
	}
	int ms = tv ? (int)(guest::load<uint32_t>(tv) * 1000 + guest::load<uint32_t>(tv + 4) / 1000) : -1;
	int r = ::poll(pf.data(), pf.size(), ms);
	if (r < 0) { fail(errno); SET(SOCKERR); return; }
	std::vector<uint32_t> ready[3];
	int total = 0;
	for (size_t i = 0; i < pf.size(); i++)
	{
		short ev = pf[i].revents;
		int k = who[i].first;
		bool hit = k == 0 ? (ev & (POLLIN | POLLHUP | POLLERR)) : k == 1 ? (ev & (POLLOUT | POLLERR | POLLHUP)) : (ev & (POLLPRI | POLLERR));
		if (hit) { ready[k].push_back(who[i].second); total++; }
	}
	for (int k = 0; k < 3; k++)
	{
		if (!sets[k]) continue;
		guest::store<uint32_t>(sets[k], (uint32_t)ready[k].size());
		for (size_t i = 0; i < ready[k].size(); i++) guest::store<uint32_t>(sets[k] + 4 + 4 * (uint32_t)i, ready[k][i]);
	}
	SET((uint32_t)total);
}
IMP_ID(W, "#22", shutdown, 8)
{
	int fd = fdOf(ARG(0));
	if (fd < 0) { setLastError(10038); SET(SOCKERR); return; }
	::shutdown(fd, (int)ARG(1));
	SET(0);
}
IMP_ID(W, "#151", __WSAFDIsSet, 8)
{
	uint32_t s = ARG(0), set = ARG(1);
	uint32_t n = set ? std::min<uint32_t>(guest::load<uint32_t>(set), 64) : 0;
	for (uint32_t i = 0; i < n; i++) if (guest::load<uint32_t>(set + 4 + 4 * i) == s) { SET(1); return; }
	SET(0);
}
IMP_ID(W, "#57", gethostname, 8) { putStr(ARG(0), "android"); SET(0); }

// hostent { char* h_name; char** h_aliases; short h_addrtype; short h_length; char** h_addr_list; }
IMP_ID(W, "#52", gethostbyname, 4)
{
	static uint32_t he = gmalloc(16 + 4 + 4 * 17 + 4 * 16 + 256);
	std::string name = ARG(0) ? gstr(ARG(0)) : std::string("android");
	std::vector<in_addr> addrs;
	if (name == "android" || name == "localhost" || name.empty())
	{
		for (auto& f : interfaces()) addrs.push_back(f.addr);
		if (name == "localhost" || addrs.empty()) addrs.push_back(in_addr{htonl(INADDR_LOOPBACK)});
	}
	else
	{
		addrinfo hints{}, *res = nullptr;
		hints.ai_family = AF_INET;
		if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0)
		{
			for (addrinfo* r = res; r && addrs.size() < 16; r = r->ai_next) addrs.push_back(((sockaddr_in*)r->ai_addr)->sin_addr);
			freeaddrinfo(res);
		}
	}
	log("ws2: gethostbyname(%s): %zu address(es)%s", name.c_str(), addrs.size(), addrs.empty() ? "" : (std::string(", first ") + inet_ntoa(addrs[0])).c_str());
	if (addrs.empty()) { setLastError(11001); SET(0); return; }   // WSAHOST_NOT_FOUND
	uint32_t nameP = he + 16, aliases = nameP + 256, list = aliases + 4, data = list + 4 * 17;
	putStr(nameP, name.substr(0, 255));
	guest::store<uint32_t>(aliases, 0);
	size_t n = std::min<size_t>(addrs.size(), 16);
	for (size_t i = 0; i < n; i++)
	{
		memcpy(guest::host(data + 4 * (uint32_t)i), &addrs[i], 4);
		guest::store<uint32_t>(list + 4 * (uint32_t)i, data + 4 * (uint32_t)i);
	}
	guest::store<uint32_t>(list + 4 * (uint32_t)n, 0);
	guest::store<uint32_t>(he, nameP);
	guest::store<uint32_t>(he + 4, aliases);
	guest::store<uint16_t>(he + 8, 2);
	guest::store<uint16_t>(he + 10, 4);
	guest::store<uint32_t>(he + 12, list);
	SET(he);
}
IMP_ID(W, "#51", gethostbyaddr, 12) { setLastError(11001); SET(0); }
IMP_ID(W, "#8", htonl, 4) { SET(__builtin_bswap32(ARG(0))); }
IMP_ID(W, "#14", ntohl, 4) { SET(__builtin_bswap32(ARG(0))); }
IMP_ID(W, "#9", htons, 4) { SET(__builtin_bswap16((uint16_t)ARG(0))); }
IMP_ID(W, "#15", ntohs, 4) { SET(__builtin_bswap16((uint16_t)ARG(0))); }
IMP_ID(W, "#11", inet_addr, 4) { unsigned a, b, c2, d; if (sscanf(gstr(ARG(0)).c_str(), "%u.%u.%u.%u", &a, &b, &c2, &d) == 4) SET(a | b << 8 | c2 << 16 | d << 24); else SET(0xffffffff); }
IMP_ID(W, "#12", inet_ntoa, 4) { static uint32_t buf = gmalloc(32); uint32_t v = ARG(0); putStr(buf, std::to_string(v & 255) + "." + std::to_string(v >> 8 & 255) + "." + std::to_string(v >> 16 & 255) + "." + std::to_string(v >> 24)); SET(buf); }

// WSAEVENTs are manual-reset kernel events
IMP_ID(W, "WSACreateEvent", WSACreateEvent, 0) { SET(eventCreate(true, false)); }
IMP_ID(W, "WSASetEvent", WSASetEvent, 4) { eventSet(ARG(0)); SET(1); }
IMP_ID(W, "WSAResetEvent", WSAResetEvent, 4) { eventReset(ARG(0)); SET(1); }
IMP_ID(W, "WSACloseEvent", WSACloseEvent, 4) { SET(1); }
IMP_ID(W, "WSAWaitForMultipleEvents", WSAWaitForMultipleEvents, 20)
{
	uint32_t n = std::min<uint32_t>(ARG(0), 64), hs[64];
	for (uint32_t i = 0; i < n; i++) hs[i] = guest::load<uint32_t>(ARG(1) + 4 * i);
	int r = waitHandles(hs, (int)n, ARG(2) != 0, ARG(3));
	SET(r < 0 ? 0x102u : (uint32_t)r);   // WSA_WAIT_TIMEOUT / WSA_WAIT_EVENT_0 + i
}
IMP_ID(W, "WSARecv", WSARecv, 28) { wsaRecv(c, false); }
IMP_ID(W, "WSARecvFrom", WSARecvFrom, 36) { wsaRecv(c, true); }
// WSAGetOverlappedResult(s, ov, bytes*, wait, flags*)
IMP_ID(W, "WSAGetOverlappedResult", WSAGetOverlappedResult, 20)
{
	uint32_t ov = ARG(1);
	std::unique_lock<std::mutex> l(ovMu);
	auto done = [&] { return ovDone.count(ov) != 0; };
	if (!done() && guest::load<uint32_t>(ov) == 0x103)
	{
		if (!ARG(3)) { l.unlock(); setLastError(996); SET(0); return; }   // WSA_IO_INCOMPLETE
		ovCv.wait(l, done);
	}
	uint32_t st = guest::load<uint32_t>(ov), bytes = guest::load<uint32_t>(ov + 4);
	l.unlock();
	if (ARG(2)) guest::store<uint32_t>(ARG(2), bytes);
	if (ARG(4)) guest::store<uint32_t>(ARG(4), 0);
	if (st == 0) { SET(1); return; }
	setLastError(st == 0xc0000120u ? 995 : (st & 0xffff));
	SET(0);
}
// WSAIoctl(s, code, in, inLen, out, outLen, returned*, ov, completion)
IMP_ID(W, "WSAIoctl", WSAIoctl, 36)
{
	uint32_t code = ARG(1), out = ARG(4), outLen = ARG(5), ret = ARG(6);
	if (code == 0x4004747f)   // SIO_GET_INTERFACE_LIST: INTERFACE_INFO { flags; sockaddr_gen addr, bcast, mask (24 bytes each) }
	{
		auto ifs = interfaces();
		uint32_t n = std::min<uint32_t>((uint32_t)ifs.size(), outLen / 76);
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t e = out + 76 * i;
			memset(guest::host(e), 0, 76);
			guest::store<uint32_t>(e, ifs[i].flags);
			auto put = [&](uint32_t at, in_addr a) { guest::store<uint16_t>(at, 2); memcpy(guest::host(at + 4), &a, 4); };
			put(e + 4, ifs[i].addr); put(e + 28, ifs[i].bcast); put(e + 52, ifs[i].mask);
		}
		if (ret) guest::store<uint32_t>(ret, 76 * n);
		log("ws2: interface list: %u interface(s)%s", n, n ? (std::string(", first ") + inet_ntoa(ifs[0].addr)).c_str() : "");
		SET(0);
		return;
	}
	if (code == 0x9800000c) { if (ret) guest::store<uint32_t>(ret, 0); SET(0); return; }   // SIO_UDP_CONNRESET: nothing to do
	if (logOn()) log("ws2: WSAIoctl(0x%x) unsupported", code);
	setLastError(10045);
	SET(SOCKERR);
}
