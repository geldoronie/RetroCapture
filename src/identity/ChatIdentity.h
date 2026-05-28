#pragma once

#include <cstdint>
#include <string>

/**
 * Persistent chat identity (#84).
 *
 * Lives on disk as $XDG_DATA_HOME/retrocapture/identity.json (or the
 * platform-equivalent under Paths::getUserDataDir()). The file is the
 * source of truth for who the user is on chat servers; losing it
 * loses the identity. The user is responsible for backing it up if
 * they care about continuity (the Preferences UI surfaces the
 * disk path so they know what to copy).
 *
 * Lifecycle:
 *   - First launch: file missing → load() returns an empty Identity
 *     with id=="". The Profile window in OSDChat surfaces empty
 *     fields; the user fills name/nickname/age and clicks Save.
 *   - Save() generates the id IF empty, then writes the file. Once
 *     id is non-empty it is immutable — subsequent saves keep the
 *     same value and only refresh name/nickname/age/createdAt.
 *   - Subsequent launches: load() reads the file; the id is sent on
 *     every chat hello as `client_id`.
 *
 * ID shape:
 *   - rc_<12 lowercase-hex chars> (e.g. rc_a1b2c3d4e5f6).
 *   - Generated as SHA-256(name | "\0" | nickname | "\0" | age |
 *     "\0" | iso8601_timestamp | "\0" | random16bytes); take the
 *     first 12 hex chars; prefix `rc_`.
 *   - Server-side validator accepts ^rc_[a-z0-9]{6,32}$.
 */
struct ChatIdentity
{
    std::string id;        // empty when uninitialized; immutable once set
    std::string name;
    std::string nickname;
    int         age       = 0;
    std::string createdAt; // ISO-8601 when id was generated, empty otherwise

    bool isInitialized() const { return !id.empty(); }
};

namespace identity
{

/// Reads the identity file from disk. Returns an empty
/// ChatIdentity when the file is missing or corrupt; the caller can
/// detect that via isInitialized().
ChatIdentity load();

/// Persists `id` to disk. When id.id is empty, generates a new ID
/// from the other fields + a fresh timestamp + random bytes and
/// fills it in BEFORE writing. The caller's copy is mutated to
/// reflect the assigned id/createdAt. Returns false on I/O failure.
bool save(ChatIdentity &id);

/// File path identity is persisted at. Surfaced in the UI so the
/// user knows what to back up.
std::string filePath();

} // namespace identity
