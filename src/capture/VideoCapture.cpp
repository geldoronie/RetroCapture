#include "VideoCapture.h"
#include "../utils/Logger.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <filesystem>

VideoCapture::VideoCapture()
{
}

VideoCapture::~VideoCapture()
{
    close();
}

bool VideoCapture::open(const std::string &device)
{
    if (m_fd >= 0)
    {
        LOG_WARN("Device already open, closing first");
        close();
    }

    // Verificar se o arquivo do dispositivo existe antes de tentar abrir
    // Isso evita tentar abrir dispositivos que não existem
    if (!std::filesystem::exists(device))
    {
        LOG_ERROR("Device does not exist: " + device);
        return false;
    }

    m_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0)
    {
        int err = errno;
        LOG_ERROR("Failed to abrir dispositivo: " + device + " (errno: " + std::to_string(err) + " - " + strerror(err) + ")");
        return false;
    }

    LOG_INFO("Dispositivo aberto: " + device);
    return true;
}

void VideoCapture::close()
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

    // Limpar buffer dummy quando fechar (mas manter modo dummy se estiver ativo)
    // O buffer será recriado quando setFormat() for chamado novamente
    if (!m_dummyMode)
    {
        m_dummyFrameBuffer.clear();
    }
}

namespace
{
    // #134 — fill a YUYV (YUY2) dummy buffer with a calm neutral dark-gray
    // "no signal" frame. All-zero bytes look black in RGB but in YUYV the
    // chroma bytes default to 0 (= -128 after the 128 bias), which the
    // YUYV→RGB conversion turns into bright GREEN — that read like an
    // error/glitch when no device was present. Set Y to a dark level and
    // U/V to neutral 128. Byte order per 2 px: Y0 U Y1 V.
    void fillDummyNoSignalYUYV(std::vector<uint8_t> &buf)
    {
        for (size_t i = 0; i + 3 < buf.size(); i += 4)
        {
            buf[i]     = 0x18; // Y0 — dark gray
            buf[i + 1] = 0x80; // U  — neutral (128)
            buf[i + 2] = 0x18; // Y1
            buf[i + 3] = 0x80; // V  — neutral (128)
        }
    }
} // namespace

bool VideoCapture::setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
    // Em modo dummy, apenas definir dimensões sem abrir dispositivo
    if (m_dummyMode)
    {
        m_width = width;
        m_height = height;
        m_pixelFormat = pixelFormat != 0 ? pixelFormat : V4L2_PIX_FMT_YUYV;

        // Criar buffer dummy para frame preto (YUYV: 2 bytes por pixel)
        size_t frameSize = width * height * 2;
        m_dummyFrameBuffer.resize(frameSize);
        fillDummyNoSignalYUYV(m_dummyFrameBuffer); // neutral dark "no signal", not green (#134)

        LOG_INFO("Formato dummy definido: " + std::to_string(m_width) + "x" +
                 std::to_string(m_height) + " (format: 0x" +
                 std::to_string(m_pixelFormat) + ")");
        return true;
    }

    if (m_fd < 0)
    {
        LOG_ERROR("Device not open");
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(m_fd, VIDIOC_G_FMT, &fmt) < 0)
    {
        LOG_ERROR("Failed to obter formato atual");
        return false;
    }

    // Se pixelFormat não especificado, usar o atual ou YUYV
    if (pixelFormat == 0)
    {
        pixelFormat = fmt.fmt.pix.pixelformat;
        if (pixelFormat == 0)
        {
            pixelFormat = V4L2_PIX_FMT_YUYV;
        }
    }

    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelFormat;
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        LOG_ERROR("Failed to definir formato");
        return false;
    }

    // O driver pode ajustar os valores
    m_width = fmt.fmt.pix.width;
    m_height = fmt.fmt.pix.height;
    m_pixelFormat = fmt.fmt.pix.pixelformat;

    LOG_INFO("Formato definido: " + std::to_string(m_width) + "x" +
             std::to_string(m_height) + " (format: 0x" +
             std::to_string(m_pixelFormat) + ")");

    return true;
}

bool VideoCapture::setFramerate(uint32_t fps)
{
    // Em modo dummy, apenas aceitar (não há dispositivo real para configurar)
    if (m_dummyMode)
    {
        LOG_INFO("Framerate dummy configurado: " + std::to_string(fps) + "fps (não aplicado em modo dummy)");
        return true;
    }

    if (m_fd < 0)
    {
        LOG_ERROR("Device not open");
        return false;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // Primeiro, obter os parâmetros atuais
    if (ioctl(m_fd, VIDIOC_G_PARM, &parm) == -1)
    {
        LOG_WARN("Could not get streaming parameters (ioctl VIDIOC_G_PARM)");
        return false;
    }

    // Verificar se o dispositivo suporta configuração de framerate
    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME))
    {
        LOG_WARN("Device does not support framerate configuration");
        return false;
    }

    // Configurar o framerate
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) == -1)
    {
        LOG_WARN("Failed to configurar framerate (ioctl VIDIOC_S_PARM)");
        return false;
    }

    // Verificar o framerate real configurado
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

bool VideoCapture::initMemoryMapping()
{
    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = 4; // 4 buffers

    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0)
    {
        LOG_ERROR("Failed to solicitar buffers");
        return false;
    }

    if (req.count < 2)
    {
        LOG_ERROR("Out of memory");
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
            LOG_ERROR("Failed to consultar buffer");
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
            LOG_ERROR("Failed to mapear buffer");
            cleanupBuffers();
            return false;
        }
    }

    LOG_INFO("Memory mapping inicializado com " + std::to_string(m_buffers.size()) + " buffers");
    return true;
}

void VideoCapture::cleanupBuffers()
{
    // Desenfileirar todos os buffers antes de limpar
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

bool VideoCapture::startCapture()
{
    // Em modo dummy, apenas ativar streaming sem dispositivo real
    if (m_dummyMode)
    {
        if (m_streaming)
        {
            LOG_WARN("Dummy capture already started");
            return true;
        }

        // Garantir que o buffer dummy existe
        if (m_dummyFrameBuffer.empty() && m_width > 0 && m_height > 0)
        {
            size_t frameSize = m_width * m_height * 2; // YUYV: 2 bytes por pixel
            m_dummyFrameBuffer.resize(frameSize);
            fillDummyNoSignalYUYV(m_dummyFrameBuffer); // neutral dark "no signal", not green (#134)
        }

        m_streaming = true;
        LOG_INFO("Captura dummy iniciada: " + std::to_string(m_width) + "x" + std::to_string(m_height));
        return true;
    }

    if (m_fd < 0)
    {
        LOG_ERROR("Device not open");
        return false;
    }

    if (m_streaming)
    {
        LOG_WARN("Capture already started");
        return true;
    }

    if (m_buffers.empty())
    {
        if (!initMemoryMapping())
        {
            return false;
        }
    }

    // Enfileirar todos os buffers
    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
        {
            LOG_ERROR("Failed to enfileirar buffer");
            stopCapture();
            return false;
        }
    }

    // Iniciar streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0)
    {
        LOG_ERROR("Failed to iniciar streaming");
        return false;
    }

    m_streaming = true;
    LOG_INFO("Captura iniciada");
    return true;
}

void VideoCapture::stopCapture()
{
    if (!m_streaming)
    {
        return;
    }

    // Em modo dummy, apenas desativar streaming
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

bool VideoCapture::captureFrame(Frame &frame)
{
    // Em modo dummy, retornar frame preto
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

    // Tentar obter um frame (non-blocking)
    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        int err = errno;
        // EAGAIN é normal quando não há frame disponível (non-blocking)
        if (err == EAGAIN)
        {
            return false;
        }
        
        // Erros críticos que indicam que o dispositivo foi desconectado
        if (err == EBADF || err == ENODEV || err == EIO)
        {
            LOG_ERROR("Dispositivo USB desconectado detectado (errno: " + std::to_string(err) + 
                     " - " + strerror(err) + ") - ativando modo dummy");
            
            // Fechar dispositivo graciosamente
            stopCapture();
            cleanupBuffers();
            if (m_fd >= 0)
            {
                ::close(m_fd);
                m_fd = -1;
            }
            
            // Ativar modo dummy para continuar funcionando
            m_dummyMode = true;
            if (m_width > 0 && m_height > 0)
            {
                size_t frameSize = m_width * m_height * 2; // YUYV: 2 bytes por pixel
                m_dummyFrameBuffer.resize(frameSize);
                fillDummyNoSignalYUYV(m_dummyFrameBuffer); // neutral dark "no signal", not green (#134)
                m_streaming = true;
                LOG_INFO("Dummy mode activated automatically after device disconnect");
            }
            
            return false;
        }
        
        // Outros erros
        LOG_ERROR("Failed to capturar frame (errno: " + std::to_string(err) + " - " + strerror(err) + ")");
        return false;
    }

    // Validar buffer antes de usar
    if (buf.index >= m_buffers.size() || 
        !m_buffers[buf.index].start || 
        m_buffers[buf.index].start == MAP_FAILED)
    {
        LOG_ERROR("Invalid buffer at index " + std::to_string(buf.index));
        // Tentar reenfileirar para não perder sincronização (mas pode falhar se dispositivo foi desconectado)
        if (m_fd >= 0)
        {
            ioctl(m_fd, VIDIOC_QBUF, &buf);
        }
        return false;
    }

    // Preencher frame
    frame.data = static_cast<uint8_t *>(m_buffers[buf.index].start);
    frame.size = buf.length;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat;

    // Reenfileirar o buffer
    if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
    {
        int err = errno;
        // Erros críticos que indicam que o dispositivo foi desconectado
        if (err == EBADF || err == ENODEV || err == EIO)
        {
            LOG_ERROR("Dispositivo USB desconectado durante reenfileiramento (errno: " + 
                     std::to_string(err) + " - " + strerror(err) + ") - ativando modo dummy");
            
            // Fechar dispositivo graciosamente
            stopCapture();
            cleanupBuffers();
            if (m_fd >= 0)
            {
                ::close(m_fd);
                m_fd = -1;
            }
            
            // Ativar modo dummy para continuar funcionando
            m_dummyMode = true;
            if (m_width > 0 && m_height > 0)
            {
                size_t frameSize = m_width * m_height * 2; // YUYV: 2 bytes por pixel
                m_dummyFrameBuffer.resize(frameSize);
                fillDummyNoSignalYUYV(m_dummyFrameBuffer); // neutral dark "no signal", not green (#134)
                m_streaming = true;
                LOG_INFO("Dummy mode activated automatically after device disconnect");
            }
        }
        else
        {
            LOG_ERROR("Failed to reenfileirar buffer (errno: " + std::to_string(err) + " - " + strerror(err) + ")");
        }
        return false;
    }

    return true;
}

bool VideoCapture::captureLatestFrame(Frame &frame)
{
    // Em modo dummy, retornar frame preto diretamente
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

    // Descartar todos os frames antigos e pegar apenas o mais recente
    Frame tempFrame;
    bool gotFrame = false;

    // Loop para descartar frames antigos até não haver mais frames disponíveis
    while (captureFrame(tempFrame))
    {
        // Guardar o último frame capturado
        frame = tempFrame;
        gotFrame = true;
        // Continuar tentando até não haver mais frames
    }

    return gotFrame;
}

std::vector<uint32_t> VideoCapture::getSupportedFormats()
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

bool VideoCapture::setControl(uint32_t controlId, int32_t value)
{
    if (m_fd < 0)
    {
        LOG_ERROR("Device not open");
        return false;
    }

    struct v4l2_control ctrl = {};
    ctrl.id = controlId;
    ctrl.value = value;

    if (ioctl(m_fd, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        LOG_WARN("Failed to definir controle V4L2 (ID: " + std::to_string(controlId) +
                 ", valor: " + std::to_string(value) + "): " + strerror(errno));
        return false;
    }

    return true;
}

bool VideoCapture::getControl(uint32_t controlId, int32_t &value)
{
    if (m_fd < 0)
    {
        LOG_ERROR("Device not open");
        return false;
    }

    struct v4l2_control ctrl = {};
    ctrl.id = controlId;

    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0)
    {
        LOG_WARN("Failed to obter controle V4L2 (ID: " + std::to_string(controlId) + ")");
        return false;
    }

    value = ctrl.value;
    return true;
}

bool VideoCapture::getControl(uint32_t controlId, int32_t &value, int32_t &min, int32_t &max, int32_t &step)
{
    if (m_fd < 0)
    {
        LOG_ERROR("Device not open");
        return false;
    }

    // Primeiro, obter informações do controle (min, max, step)
    struct v4l2_queryctrl queryctrl = {};
    queryctrl.id = controlId;

    if (ioctl(m_fd, VIDIOC_QUERYCTRL, &queryctrl) < 0)
    {
        // Controle não disponível
        return false;
    }

    // Verificar se o controle está desabilitado
    if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
    {
        return false;
    }

    min = queryctrl.minimum;
    max = queryctrl.maximum;
    step = queryctrl.step;

    // Agora obter o valor atual
    struct v4l2_control ctrl = {};
    ctrl.id = controlId;

    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0)
    {
        return false;
    }

    value = ctrl.value;
    return true;
}

bool VideoCapture::setBrightness(int32_t value)
{
    return setControl(V4L2_CID_BRIGHTNESS, value);
}

bool VideoCapture::setContrast(int32_t value)
{
    return setControl(V4L2_CID_CONTRAST, value);
}

bool VideoCapture::setSaturation(int32_t value)
{
    return setControl(V4L2_CID_SATURATION, value);
}

bool VideoCapture::setHue(int32_t value)
{
    return setControl(V4L2_CID_HUE, value);
}

bool VideoCapture::setGain(int32_t value)
{
    return setControl(V4L2_CID_GAIN, value);
}

bool VideoCapture::setExposure(int32_t value)
{
    return setControl(V4L2_CID_EXPOSURE_ABSOLUTE, value);
}

bool VideoCapture::setSharpness(int32_t value)
{
    return setControl(V4L2_CID_SHARPNESS, value);
}

bool VideoCapture::setGamma(int32_t value)
{
    return setControl(V4L2_CID_GAMMA, value);
}

bool VideoCapture::setWhiteBalanceTemperature(int32_t value)
{
    return setControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, value);
}

bool VideoCapture::convertYUYVtoRGB(const Frame & /*input*/, std::vector<uint8_t> & /*output*/)
{
    // Esta função não é mais usada - a conversão é feita em Application
    return false;
}

void VideoCapture::generateDummyFrame(Frame &frame)
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
