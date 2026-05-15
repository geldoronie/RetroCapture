#pragma once

#include <string>

/**
 * Token-based authorization helpers shared by every HTTP endpoint
 * that needs to gate access on a configurable secret.
 *
 * The wire format is straightforward: the client sends one of
 *
 *   Authorization: Bearer <hex-sha256-of-password>
 *
 * or, as a fallback for tools that can't set arbitrary headers
 *
 *   GET /raw?token=<hex-sha256-of-password>
 *
 * and the server compares the received token against the hash the
 * user configured. Empty configured hash means "no auth required",
 * which is the default for unprotected streams (this matches the
 * directory's passwordRequired=false flag).
 *
 * Constant-time compare is used so a slow byte-by-byte attacker
 * can't time their way to the right hash. Not actually load-bearing
 * for our 64-char hex hashes — but cheap and good hygiene.
 *
 * Used by HTTPTSStreamer for /raw and APIController for /meta.
 */
namespace HttpAuth
{
    /// Returns the bearer token from the request, or empty string if
    /// none was sent. Checks the Authorization header first, then a
    /// ?token=... query parameter on the request line.
    std::string extractBearerToken(const std::string &rawRequest);

    /// Convenience: true iff the request is authorized for
    /// `expectedHash`. Empty `expectedHash` is treated as "no auth
    /// configured" and always returns true.
    bool authorized(const std::string &rawRequest,
                    const std::string &expectedHash);
}
