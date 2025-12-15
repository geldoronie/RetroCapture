#include "VideoCaptureV4L2.h"
#include "../utils/Logger.h"
#include "../utils/V4L2DeviceScanner.h"
#include "../v4l2/V4L2ControlMapper.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <filesystem>

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif

VideoCaptureV4L2::VideoCaptureV4L2()
{
}

VideoCaptureV4L2::~VideoCaptureV4L2()
{
    close();
}

bool VideoCaptureV4L2::open(const std::string &device)
{
    if (m_fd >= 0)
    {
        LOG_WARN("Dispositivo já aberto, fechando primeiro");
        close();
    }

    // Verificar se o arquivo do dispositivo existe antes de tentar abrir
    if (!std::filesystem::exists(device))
    {
        LOG_ERROR("Dispositivo não existe: " + device);
        return false;
    }

    m_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0)
    {
        int err = errno;
        LOG_ERROR("Falha ao abrir dispositivo: " + device + " (errno: " + std::to_string(err) + " - " + strerror(err) + ")");
        return false;
    }

    LOG_INFO("Dispositivo aberto: " + device);
    return true;
}

void VideoCaptureV4L2::close()
{
    if (m_streaming)
    {
        stopCapture();
    }

    cleanupBuffers();

    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
        LOG_INFO("Dispositivo fechado");
    }

    if (!m_dummyMode)
    {
        m_dummyFrameBuffer.clear();
    }
}

bool VideoCaptureV4L2::isOpen() const
{
    return m_fd >= 0 || m_dummyMode;
}

bool VideoCaptureV4L2::setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
    // Em modo dummy, apenas definir dimensões sem abrir dispositivo
    if (m_dummyMode)
    {
        m_width = width;
        m_height = height;
        m_pixelFormat = pixelFormat != 0 ? pixelFormat : V4L2_PIX_FMT_YUYV;

        size_t frameSize = width * height * 2;
        m_dummyFrameBuffer.resize(frameSize, 0);

        LOG_INFO("Formato dummy definido: " + std::to_string(m_width) + "x" +
                 std::to_string(m_height) + " (format: 0x" +
                 std::to_string(m_pixelFormat) + ")");
        return true;
    }

    if (m_fd < 0)
    {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(m_fd, VIDIOC_G_FMT, &fmt) < 0)
    {
        LOG_ERROR("Falha ao obter formato atual");
        return false;
    }

    if (pixelFormat == 0)
    {
        // Quando formato não especificado, sempre tentar YUYV primeiro
        // Verificar se YUYV é suportado
        struct v4l2_fmtdesc fmtDesc = {};
        fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bool yuyvSupported = false;
        bool mjpegSupported = false;

        while (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0)
        {
            if (fmtDesc.pixelformat == V4L2_PIX_FMT_YUYV)
            {
                yuyvSupported = true;
            }
            if (fmtDesc.pixelformat == V4L2_PIX_FMT_MJPEG)
            {
                mjpegSupported = true;
            }
            fmtDesc.index++;
        }

        if (yuyvSupported)
        {
            LOG_INFO("YUYV é suportado, usando YUYV como formato padrão");
            pixelFormat = V4L2_PIX_FMT_YUYV;
        }
        else
        {
            // Se YUYV não é suportado, pegar o formato atual do dispositivo
            pixelFormat = fmt.fmt.pix.pixelformat;
            if (pixelFormat == 0)
            {
                // Se ainda não temos formato, tentar MJPG como último recurso
                if (mjpegSupported)
                {
                    LOG_WARN("YUYV não suportado, usando MJPG (não totalmente suportado ainda)");
                    pixelFormat = V4L2_PIX_FMT_MJPEG;
                }
                else
                {
                    LOG_ERROR("Nenhum formato suportado encontrado");
                    return false;
                }
            }
            else if (pixelFormat == V4L2_PIX_FMT_MJPEG)
            {
                LOG_WARN("Dispositivo está usando MJPG mas YUYV não está disponível. MJPG não é totalmente suportado.");
            }
        }
    }

    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelFormat;
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        LOG_ERROR("Falha ao definir formato");
        return false;
    }

    m_width = fmt.fmt.pix.width;
    m_height = fmt.fmt.pix.height;
    m_pixelFormat = fmt.fmt.pix.pixelformat;

    // Verificar se o formato foi aceito corretamente
    if (m_pixelFormat != pixelFormat)
    {
        char requestedStr[5] = {0};
        requestedStr[0] = (pixelFormat >> 0) & 0xFF;
        requestedStr[1] = (pixelFormat >> 8) & 0xFF;
        requestedStr[2] = (pixelFormat >> 16) & 0xFF;
        requestedStr[3] = (pixelFormat >> 24) & 0xFF;

        char actualStr[5] = {0};
        actualStr[0] = (m_pixelFormat >> 0) & 0xFF;
        actualStr[1] = (m_pixelFormat >> 8) & 0xFF;
        actualStr[2] = (m_pixelFormat >> 16) & 0xFF;
        actualStr[3] = (m_pixelFormat >> 24) & 0xFF;

        LOG_WARN("Formato solicitado '" + std::string(requestedStr) +
                 "' mas dispositivo retornou '" + std::string(actualStr) + "'");

        // Se foi solicitado YUYV mas retornou MJPG, tentar forçar novamente
        if (pixelFormat == V4L2_PIX_FMT_YUYV && m_pixelFormat == V4L2_PIX_FMT_MJPEG)
        {
            LOG_ERROR("Dispositivo não aceitou YUYV e retornou MJPG. YUYV pode não ser suportado.");
            return false;
        }
    }

    // Log do formato de forma mais legível
    char formatStr[5] = {0};
    formatStr[0] = (m_pixelFormat >> 0) & 0xFF;
    formatStr[1] = (m_pixelFormat >> 8) & 0xFF;
    formatStr[2] = (m_pixelFormat >> 16) & 0xFF;
    formatStr[3] = (m_pixelFormat >> 24) & 0xFF;
    LOG_INFO("Formato definido: " + std::to_string(m_width) + "x" +
             std::to_string(m_height) + " (format: 0x" +
             std::to_string(m_pixelFormat) + " = '" + std::string(formatStr) + "')");

    return true;
}

bool VideoCaptureV4L2::setFramerate(uint32_t fps)
{
    if (m_dummyMode)
    {
        LOG_INFO("Framerate dummy configurado: " + std::to_string(fps) + "fps");
        return true;
    }

    if (m_fd < 0)
    {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(m_fd, VIDIOC_G_PARM, &parm) == -1)
    {
        LOG_WARN("Não foi possível obter parâmetros de streaming");
        return false;
    }

    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME))
    {
        LOG_WARN("Dispositivo não suporta configuração de framerate");
        return false;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) == -1)
    {
        LOG_WARN("Falha ao configurar framerate");
        return false;
    }

    uint32_t actualFps = parm.parm.capture.timeperframe.denominator /
                         parm.parm.capture.timeperframe.numerator;

    if (actualFps != fps)
    {
        LOG_WARN("Framerate configurado: " + std::to_string(actualFps) +
                 "fps (solicitado: " + std::to_string(fps) + "fps)");
    }
    else
    {
        LOG_INFO("Framerate configurado: " + std::to_string(actualFps) + "fps");
    }

    return true;
}

bool VideoCaptureV4L2::captureFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        if (!m_streaming || m_dummyFrameBuffer.empty())
        {
            return false;
        }
        generateDummyFrame(frame);
        return true;
    }

    if (m_fd < 0 || !m_streaming)
    {
        return false;
    }

    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        if (errno != EAGAIN)
        {
            LOG_ERROR("Erro ao capturar frame");
        }
        return false;
    }

    // Validar buffer antes de usar
    if (!m_buffers[buf.index].start || m_buffers[buf.index].start == MAP_FAILED)
    {
        LOG_ERROR("Buffer inválido no índice " + std::to_string(buf.index));
        // Ainda assim, tentar reenfileirar para não perder sincronização
        ioctl(m_fd, VIDIOC_QBUF, &buf);
        return false;
    }

    frame.data = static_cast<uint8_t *>(m_buffers[buf.index].start);
    frame.size = buf.length;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat;

    // Validar tamanho do frame
    size_t expectedSize = m_width * m_height * 2; // YUYV: 2 bytes por pixel
    if (buf.length < expectedSize)
    {
        LOG_WARN("Tamanho do buffer menor que o esperado: " + std::to_string(buf.length) +
                 " < " + std::to_string(expectedSize));
    }

    if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
    {
        LOG_ERROR("Falha ao reenfileirar buffer");
        return false;
    }

    return true;
}

bool VideoCaptureV4L2::setControl(const std::string &controlName, int32_t value)
{
    uint32_t controlId = getControlIdFromName(controlName);
    if (controlId == 0)
    {
        LOG_WARN("Controle não encontrado: " + controlName);
        return false;
    }
    return setControl(controlId, value);
}

bool VideoCaptureV4L2::getControl(const std::string &controlName, int32_t &value)
{
    uint32_t controlId = getControlIdFromName(controlName);
    if (controlId == 0)
    {
        LOG_WARN("Controle não encontrado: " + controlName);
        return false;
    }
    return getControl(controlId, value);
}

bool VideoCaptureV4L2::getControlMin(const std::string &controlName, int32_t &minValue)
{
    uint32_t controlId = getControlIdFromName(controlName);
    if (controlId == 0)
    {
        return false;
    }

    int32_t value, max, step;
    return getControl(controlId, value, minValue, max, step);
}

bool VideoCaptureV4L2::getControlMax(const std::string &controlName, int32_t &maxValue)
{
    uint32_t controlId = getControlIdFromName(controlName);
    if (controlId == 0)
    {
        return false;
    }

    int32_t value, min, step;
    return getControl(controlId, value, min, maxValue, step);
}

bool VideoCaptureV4L2::getControlDefault(const std::string &controlName, int32_t &defaultValue)
{
    // V4L2 não expõe valor padrão diretamente, usar valor atual como fallback
    // ou retornar false se não disponível
    uint32_t controlId = getControlIdFromName(controlName);
    if (controlId == 0)
    {
        return false;
    }

    // Tentar obter o valor padrão via VIDIOC_QUERYCTRL
    struct v4l2_queryctrl queryctrl = {};
    queryctrl.id = controlId;

    if (m_fd < 0 || ioctl(m_fd, VIDIOC_QUERYCTRL, &queryctrl) < 0)
    {
        return false;
    }

    defaultValue = queryctrl.default_value;
    return true;
}

std::vector<DeviceInfo> VideoCaptureV4L2::listDevices()
{
    std::vector<DeviceInfo> devices;
    std::vector<std::string> devicePaths = V4L2DeviceScanner::scan();

    for (const auto &path : devicePaths)
    {
        DeviceInfo info;
        info.id = path;

        // Tentar obter nome do dispositivo
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0)
        {
            struct v4l2_capability cap = {};
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) >= 0)
            {
                info.name = std::string(reinterpret_cast<const char *>(cap.card));
                info.driver = std::string(reinterpret_cast<const char *>(cap.driver));
            }
            ::close(fd);
        }

        if (info.name.empty())
        {
            info.name = path; // Fallback para o path
        }

        info.available = true;
        devices.push_back(info);
    }

    return devices;
}

void VideoCaptureV4L2::setDummyMode(bool enabled)
{
    m_dummyMode = enabled;
}

bool VideoCaptureV4L2::isDummyMode() const
{
    return m_dummyMode;
}

// Métodos V4L2 específicos (para compatibilidade)
bool VideoCaptureV4L2::setControl(uint32_t controlId, int32_t value)
{
    if (m_fd < 0)
    {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }

    struct v4l2_control ctrl = {};
    ctrl.id = controlId;
    ctrl.value = value;

    if (ioctl(m_fd, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        LOG_WARN("Falha ao definir controle V4L2 (ID: " + std::to_string(controlId) +
                 ", valor: " + std::to_string(value) + "): " + strerror(errno));
        return false;
    }

    return true;
}

bool VideoCaptureV4L2::getControl(uint32_t controlId, int32_t &value)
{
    if (m_fd < 0)
    {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }

    struct v4l2_control ctrl = {};
    ctrl.id = controlId;

    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0)
    {
        // Não logar como WARN se o controle simplesmente não existe ou não está disponível
        // Apenas retornar false silenciosamente
        return false;
    }

    value = ctrl.value;
    return true;
}

bool VideoCaptureV4L2::getControl(uint32_t controlId, int32_t &value, int32_t &min, int32_t &max, int32_t &step)
{
    if (m_fd < 0)
    {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }

    struct v4l2_queryctrl queryctrl = {};
    queryctrl.id = controlId;

    if (ioctl(m_fd, VIDIOC_QUERYCTRL, &queryctrl) < 0)
    {
        return false;
    }

    if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
    {
        return false;
    }

    min = queryctrl.minimum;
    max = queryctrl.maximum;
    step = queryctrl.step;

    struct v4l2_control ctrl = {};
    ctrl.id = controlId;

    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0)
    {
        return false;
    }

    value = ctrl.value;
    return true;
}

bool VideoCaptureV4L2::setBrightness(int32_t value)
{
    return setControl(V4L2_CID_BRIGHTNESS, value);
}

bool VideoCaptureV4L2::setContrast(int32_t value)
{
    return setControl(V4L2_CID_CONTRAST, value);
}

bool VideoCaptureV4L2::setSaturation(int32_t value)
{
    return setControl(V4L2_CID_SATURATION, value);
}

bool VideoCaptureV4L2::setHue(int32_t value)
{
    return setControl(V4L2_CID_HUE, value);
}

bool VideoCaptureV4L2::setGain(int32_t value)
{
    return setControl(V4L2_CID_GAIN, value);
}

bool VideoCaptureV4L2::setExposure(int32_t value)
{
    return setControl(V4L2_CID_EXPOSURE_ABSOLUTE, value);
}

bool VideoCaptureV4L2::setSharpness(int32_t value)
{
    return setControl(V4L2_CID_SHARPNESS, value);
}

bool VideoCaptureV4L2::setGamma(int32_t value)
{
    return setControl(V4L2_CID_GAMMA, value);
}

bool VideoCaptureV4L2::setWhiteBalanceTemperature(int32_t value)
{
    return setControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, value);
}

bool VideoCaptureV4L2::startCapture()
{
    if (m_dummyMode)
    {
        if (m_streaming)
        {
            return true;
        }

        if (m_dummyFrameBuffer.empty() && m_width > 0 && m_height > 0)
        {
            size_t frameSize = m_width * m_height * 2;
            m_dummyFrameBuffer.resize(frameSize, 0);
        }

        m_streaming = true;
        LOG_INFO("Captura dummy iniciada: " + std::to_string(m_width) + "x" + std::to_string(m_height));
        return true;
    }

    if (m_fd < 0)
    {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }

    if (m_streaming)
    {
        return true;
    }

    if (m_buffers.empty())
    {
        if (!initMemoryMapping())
        {
            return false;
        }
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
        {
            LOG_ERROR("Falha ao enfileirar buffer");
            stopCapture();
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0)
    {
        LOG_ERROR("Falha ao iniciar streaming");
        return false;
    }

    m_streaming = true;
    LOG_INFO("Captura iniciada");
    return true;
}

void VideoCaptureV4L2::stopCapture()
{
    if (!m_streaming)
    {
        return;
    }

    if (m_dummyMode)
    {
        m_streaming = false;
        LOG_INFO("Captura dummy parada");
        return;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (m_fd >= 0)
    {
        ioctl(m_fd, VIDIOC_STREAMOFF, &type);
    }

    m_streaming = false;
    LOG_INFO("Captura parada");
}

void VideoCaptureV4L2::cleanupBuffers()
{
    if (m_streaming)
    {
        stopCapture();
    }

    for (auto &buffer : m_buffers)
    {
        if (buffer.start && buffer.start != MAP_FAILED)
        {
            munmap(buffer.start, buffer.length);
        }
    }
    m_buffers.clear();
}

bool VideoCaptureV4L2::captureLatestFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        if (!m_streaming || m_dummyFrameBuffer.empty())
        {
            return false;
        }
        generateDummyFrame(frame);
        return true;
    }

    if (m_fd < 0 || !m_streaming)
    {
        return false;
    }

    Frame tempFrame;
    bool gotFrame = false;

    while (captureFrame(tempFrame))
    {
        frame = tempFrame;
        gotFrame = true;
    }

    return gotFrame;
}

std::vector<uint32_t> VideoCaptureV4L2::getSupportedFormats()
{
    std::vector<uint32_t> formats;

    if (m_fd < 0)
    {
        return formats;
    }

    struct v4l2_fmtdesc fmtDesc = {};
    fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0)
    {
        formats.push_back(fmtDesc.pixelformat);
        fmtDesc.index++;
    }

    return formats;
}

bool VideoCaptureV4L2::initMemoryMapping()
{
    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = 4;

    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0)
    {
        LOG_ERROR("Falha ao solicitar buffers");
        return false;
    }

    if (req.count < 2)
    {
        LOG_ERROR("Memória insuficiente");
        return false;
    }

    m_buffers.resize(req.count);

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            LOG_ERROR("Falha ao consultar buffer");
            cleanupBuffers();
            return false;
        }

        m_buffers[i].length = buf.length;
        m_buffers[i].start = mmap(nullptr, buf.length,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED,
                                  m_fd, buf.m.offset);

        if (m_buffers[i].start == MAP_FAILED)
        {
            LOG_ERROR("Falha ao mapear buffer");
            cleanupBuffers();
            return false;
        }
    }

    LOG_INFO("Memory mapping inicializado com " + std::to_string(m_buffers.size()) + " buffers");
    return true;
}

void VideoCaptureV4L2::generateDummyFrame(Frame &frame)
{
    if (m_dummyFrameBuffer.empty() || m_width == 0 || m_height == 0)
    {
        return;
    }

    frame.data = m_dummyFrameBuffer.data();
    frame.size = m_dummyFrameBuffer.size();
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat;
}

uint32_t VideoCaptureV4L2::getControlIdFromName(const std::string &controlName)
{
    return V4L2ControlMapper::getControlId(controlName);
}
