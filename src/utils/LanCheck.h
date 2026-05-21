#pragma once

#include <string>

/**
 * @brief True when the peer connected on `clientFd` is on a local
 *        network (RFC1918 / loopback / link-local).
 *
 * Used to gate access to UI surfaces that should never be touched by
 * an internet-facing viewer — the web portal's Configuration tab,
 * for instance. The check trusts only the kernel-reported peer
 * address; we do NOT honour X-Forwarded-For, because that header is
 * trivially forgeable. Side effect: if the host runs behind a
 * reverse tunnel (cloudflared, FRP, ngrok) the peer IP will be the
 * tunnel egress on localhost / LAN, which is reported as LAN. That's
 * a deliberate trade-off — protecting users who run behind tunnels
 * from forged XFF outweighs the loss of the LAN gate in those setups
 * (those users already chose to expose the service).
 *
 * Address families covered:
 *   - IPv4: 10/8, 172.16/12, 192.168/16, 127/8 (loopback), 169.254/16 (APIPA)
 *   - IPv6: ::1, fe80::/10 (link-local), fc00::/7 (ULA),
 *           ::ffff:<v4>/96 (v4-mapped — defers to the IPv4 rules)
 *
 * Returns false on getpeername() failure (safer default — if we
 * can't tell who the peer is, treat as untrusted).
 */
namespace LanCheck
{
    bool isLanClient(int clientFd);

    /**
     * True when the HTTP request was forwarded through an internet-
     * facing reverse proxy (Cloudflare, Tailscale Funnel,
     * generic XFF chain). Independent of the socket-level peer check
     * because tunnels and proxies make the peer IP look local while
     * the *actual* visitor is on the public internet. Treat a hit
     * here as 'not LAN' regardless of what isLanClient() says.
     *
     * Headers checked:
     *   - Cf-Connecting-Ip  (Cloudflare)
     *   - Cf-Ray            (Cloudflare)
     *   - X-Forwarded-For   (generic reverse-proxy chain)
     *   - X-Real-IP         (nginx convention)
     *   - True-Client-Ip    (Akamai / Cloudflare Enterprise)
     */
    bool cameFromInternetProxy(const std::string &rawRequest);

    /**
     * Convenience: true iff the request originates from the local
     * network in BOTH senses — direct peer is on LAN AND no
     * reverse-proxy header indicates an internet origin. This is
     * what UI gates ('hide Configuration to internet visitors')
     * should use.
     */
    bool isLocalRequest(int clientFd, const std::string &rawRequest);
}
