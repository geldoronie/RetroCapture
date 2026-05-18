#pragma once

#include <string>

/**
 * Tiny wrappers around libavutil/sha — FFmpeg is already linked, so
 * we don't pull in OpenSSL or roll our own implementation just for
 * the stream-password feature (#49 Phase 3).
 *
 * Returns the 64-character lowercase hex digest. Empty input returns
 * the sha256 of the empty string (e2f... etc) — the caller should
 * gate on "is a password configured" before invoking, since hashing
 * an empty password and comparing would be a false-positive trap.
 */
namespace PasswordHash
{
    std::string sha256Hex(const std::string &input);
}
