// WS2_32 (ordinal imports). Networking is not implemented yet: sockets fail cleanly so the game runs offline.
#include "rt.h"

#define W "WS2_32.dll"
using namespace rt;

static void wsaErr(uint32_t e) { setLastError(e); }
IMP_ID(W, "#115", WSAStartup, 8) { uint32_t p = ARG(1); memset(guest::host(p), 0, 400); guest::store<uint16_t>(p, ARG(0)); guest::store<uint16_t>(p + 2, 0x202); SET(0); }
IMP_ID(W, "#116", WSACleanup, 0) { SET(0); }
IMP_ID(W, "#111", WSAGetLastError, 0) { SET(10038); }
IMP_ID(W, "#23", socket, 12) { wsaErr(10047); SET(0xffffffff); }
IMP_ID(W, "#3", closesocket, 4) { SET(0); }
IMP_ID(W, "#2", bind, 12) { SET(0xffffffff); }
IMP_ID(W, "#4", connect, 12) { SET(0xffffffff); }
IMP_ID(W, "#1", accept, 12) { SET(0xffffffff); }
IMP_ID(W, "#13", listen, 8) { SET(0xffffffff); }
IMP_ID(W, "#5", getpeername, 12) { SET(0xffffffff); }
IMP_ID(W, "#6", getsockname, 12) { SET(0xffffffff); }
IMP_ID(W, "#7", getsockopt, 20) { SET(0xffffffff); }
IMP_ID(W, "#21", setsockopt, 20) { SET(0xffffffff); }
IMP_ID(W, "#10", ioctlsocket, 12) { SET(0xffffffff); }
IMP_ID(W, "#16", recv, 16) { SET(0xffffffff); }
IMP_ID(W, "#17", recvfrom, 24) { SET(0xffffffff); }
IMP_ID(W, "#19", send, 16) { SET(0xffffffff); }
IMP_ID(W, "#20", sendto, 24) { SET(0xffffffff); }
IMP_ID(W, "#18", select, 20) { SET(0); }
IMP_ID(W, "#22", shutdown, 8) { SET(0); }
IMP_ID(W, "#151", __WSAFDIsSet, 8) { SET(0); }
IMP_ID(W, "#57", gethostname, 8) { putStr(ARG(0), "android"); SET(0); }
IMP_ID(W, "#52", gethostbyname, 4) { SET(0); }
IMP_ID(W, "#51", gethostbyaddr, 12) { SET(0); }
IMP_ID(W, "#8", htonl, 4) { SET(__builtin_bswap32(ARG(0))); }
IMP_ID(W, "#14", ntohl, 4) { SET(__builtin_bswap32(ARG(0))); }
IMP_ID(W, "#9", htons, 4) { SET(__builtin_bswap16((uint16_t)ARG(0))); }
IMP_ID(W, "#15", ntohs, 4) { SET(__builtin_bswap16((uint16_t)ARG(0))); }
IMP_ID(W, "#11", inet_addr, 4) { unsigned a, b, c2, d; if (sscanf(gstr(ARG(0)).c_str(), "%u.%u.%u.%u", &a, &b, &c2, &d) == 4) SET(a | b << 8 | c2 << 16 | d << 24); else SET(0xffffffff); }
IMP_ID(W, "#12", inet_ntoa, 4) { static uint32_t buf = gmalloc(32); uint32_t v = ARG(0); putStr(buf, std::to_string(v & 255) + "." + std::to_string(v >> 8 & 255) + "." + std::to_string(v >> 16 & 255) + "." + std::to_string(v >> 24)); SET(buf); }
IMP_ID(W, "WSACreateEvent", WSACreateEvent, 0) { SET(0x7777); }
IMP_ID(W, "WSASetEvent", WSASetEvent, 4) { SET(1); }
IMP_ID(W, "WSACloseEvent", WSACloseEvent, 4) { SET(1); }
IMP_ID(W, "WSAIoctl", WSAIoctl, 36) { SET(0xffffffff); }
