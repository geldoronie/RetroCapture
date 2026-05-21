#include "PasswordHash.h"

extern "C" {
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}

#include <array>
#include <cstdint>

namespace PasswordHash
{
    std::string sha256Hex(const std::string &input)
    {
        struct AVSHA *ctx = av_sha_alloc();
        if (!ctx) return {};
        av_sha_init(ctx, 256);
        av_sha_update(ctx, reinterpret_cast<const uint8_t *>(input.data()), input.size());

        std::array<uint8_t, 32> digest{};
        av_sha_final(ctx, digest.data());
        av_free(ctx);

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.resize(64);
        for (size_t i = 0; i < digest.size(); ++i)
        {
            out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0xF];
            out[i * 2 + 1] = kHex[(digest[i] >> 0) & 0xF];
        }
        return out;
    }
}
