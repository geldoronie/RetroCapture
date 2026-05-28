#pragma once

#include <string>
#include <vector>

/**
 * Local registry of chat rooms the user has created (#84).
 *
 * Lives at $XDG_DATA_HOME/retrocapture/owned_rooms.json — sibling
 * to identity.json. The two files form the on-disk identity story:
 *   - identity.json   : who you are (rc_<id>, nickname, name, age)
 *   - owned_rooms.json: rooms you minted + the per-room secret the
 *                      server uses to flag you as is_owner on join.
 *
 * Losing identity.json loses your authored history but you stay
 * recognised as the room owner via the secret. Losing
 * owned_rooms.json loses owner privileges but identity stays.
 * Backing both up preserves everything.
 */
struct OwnedRoom
{
    std::string roomId;
    std::string slug;
    std::string title;
    std::string ownerSecret;  // 32 hex chars; never expose past the chat hello
    std::string createdAt;    // ISO-8601 when the local record was created
};

namespace ownedrooms
{

/// Read the registry. Returns empty vector when the file is
/// missing or unparseable.
std::vector<OwnedRoom> loadAll();

/// Look up a single room by its public slug. Returns true and
/// fills `out` on match.
bool findBySlug(const std::string &slug, OwnedRoom &out);

/// Append a room to the registry (writes immediately). Pass the
/// caller's pre-generated secret — the registry doesn't mint it
/// because the secret has to be known to the create-room HTTP
/// caller before the response lands.
bool append(const OwnedRoom &room);

/// Generate a fresh 32-hex-character owner secret (128 bits of
/// entropy via std::random_device).
std::string generateSecret();

/// Path the registry file is persisted at. Surfaced in the UI so
/// users know what to back up.
std::string filePath();

} // namespace ownedrooms
