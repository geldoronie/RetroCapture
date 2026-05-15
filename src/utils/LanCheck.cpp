#include "LanCheck.h"

#include <cstdint>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
#endif

namespace
{
    // Host-byte-order IPv4 address.
    bool isPrivateIPv4(uint32_t ip)
    {
        // 10.0.0.0/8
        if ((ip & 0xFF000000u) == 0x0A000000u) return true;
        // 172.16.0.0/12
        if ((ip & 0xFFF00000u) == 0xAC100000u) return true;
        // 192.168.0.0/16
        if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true;
        // 127.0.0.0/8 — loopback
        if ((ip & 0xFF000000u) == 0x7F000000u) return true;
        // 169.254.0.0/16 — link-local (APIPA)
        if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return true;
        return false;
    }
}

namespace LanCheck
{
    bool isLanClient(int clientFd)
    {
        sockaddr_storage addr{};
        socklen_t        len = sizeof(addr);
        if (::getpeername(clientFd, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
        {
            return false;
        }

        if (addr.ss_family == AF_INET)
        {
            const auto *a4 = reinterpret_cast<const sockaddr_in *>(&addr);
            const uint32_t ip = ntohl(a4->sin_addr.s_addr);
            return isPrivateIPv4(ip);
        }
        if (addr.ss_family == AF_INET6)
        {
            const auto *a6 = reinterpret_cast<const sockaddr_in6 *>(&addr);
            const uint8_t *b = a6->sin6_addr.s6_addr;

            // ::1 loopback.
            bool loopback = true;
            for (int i = 0; i < 15; ++i)
            {
                if (b[i] != 0) { loopback = false; break; }
            }
            if (loopback && b[15] == 1) return true;

            // fe80::/10 — link-local.
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;

            // fc00::/7 — unique local addresses.
            if ((b[0] & 0xfe) == 0xfc) return true;

            // ::ffff:0:0/96 — IPv4-mapped IPv6. Decode and reuse the
            // v4 classifier so dual-stack listeners behave the same.
            bool isV4Mapped = true;
            for (int i = 0; i < 10; ++i) if (b[i] != 0) { isV4Mapped = false; break; }
            if (isV4Mapped && b[10] == 0xff && b[11] == 0xff)
            {
                const uint32_t ip = (static_cast<uint32_t>(b[12]) << 24) |
                                    (static_cast<uint32_t>(b[13]) << 16) |
                                    (static_cast<uint32_t>(b[14]) << 8)  |
                                    static_cast<uint32_t>(b[15]);
                return isPrivateIPv4(ip);
            }
        }
        return false;
    }
}
