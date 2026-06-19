#include "FormatNegotiator.h"

#include "../utils/Logger.h"

namespace rc
{
namespace capture
{
std::string FormatNegotiator::fourccToString(uint32_t fourcc)
{
    char s[5] = {0};
    s[0] = static_cast<char>((fourcc >> 0) & 0xFF);
    s[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    s[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    s[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return std::string(s);
}

uint32_t FormatNegotiator::chooseDefaultV4L2Format(bool yuyvSupported, bool mjpegSupported,
                                                   uint32_t deviceCurrentFormat, bool &ok)
{
    ok = true;

    if (yuyvSupported)
    {
        LOG_INFO("YUYV supported, using YUYV as default format");
        return FOURCC_YUYV;
    }

    // Se YUYV não é suportado, pegar o formato atual do dispositivo
    uint32_t pixelFormat = deviceCurrentFormat;
    if (pixelFormat == 0)
    {
        // Se ainda não temos formato, tentar MJPG como último recurso
        if (mjpegSupported)
        {
            LOG_WARN("YUYV not supported, falling back to MJPG (not fully supported yet)");
            return FOURCC_MJPG;
        }
        LOG_ERROR("Nenhum formato suportado encontrado");
        ok = false;
        return 0;
    }
    if (pixelFormat == FOURCC_MJPG)
    {
        LOG_WARN("Device is using MJPG but YUYV is unavailable. MJPG is not fully supported.");
    }
    return pixelFormat;
}

bool FormatNegotiator::isAcceptedFormat(uint32_t requested, uint32_t actual)
{
    if (requested == actual)
    {
        return true;
    }

    LOG_WARN("Formato solicitado '" + fourccToString(requested) +
             "' mas dispositivo retornou '" + fourccToString(actual) + "'");

    // Se foi solicitado YUYV mas retornou MJPG, tentar forçar novamente
    if (requested == FOURCC_YUYV && actual == FOURCC_MJPG)
    {
        LOG_ERROR("Device did not accept YUYV and returned MJPG. YUYV may not be supported.");
        return false;
    }
    return true;
}
} // namespace capture
} // namespace rc
