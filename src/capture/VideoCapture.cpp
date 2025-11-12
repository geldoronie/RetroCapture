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

VideoCapture::VideoCapture() {
}

VideoCapture::~VideoCapture() {
    close();
}

bool VideoCapture::open(const std::string& device) {
    if (m_fd >= 0) {
        LOG_WARN("Dispositivo já aberto, fechando primeiro");
        close();
    }
    
    m_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0) {
        LOG_ERROR("Falha ao abrir dispositivo: " + device);
        return false;
    }
    
    LOG_INFO("Dispositivo aberto: " + device);
    return true;
}

void VideoCapture::close() {
    if (m_streaming) {
        stopCapture();
    }
    
    cleanupBuffers();
    
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        LOG_INFO("Dispositivo fechado");
    }
}

bool VideoCapture::setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat) {
    if (m_fd < 0) {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }
    
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(m_fd, VIDIOC_G_FMT, &fmt) < 0) {
        LOG_ERROR("Falha ao obter formato atual");
        return false;
    }
    
    // Se pixelFormat não especificado, usar o atual ou YUYV
    if (pixelFormat == 0) {
        pixelFormat = fmt.fmt.pix.pixelformat;
        if (pixelFormat == 0) {
            pixelFormat = V4L2_PIX_FMT_YUYV;
        }
    }
    
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelFormat;
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
    
    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Falha ao definir formato");
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

bool VideoCapture::setFramerate(uint32_t fps) {
    if (m_fd < 0) {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }
    
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    // Primeiro, obter os parâmetros atuais
    if (ioctl(m_fd, VIDIOC_G_PARM, &parm) == -1) {
        LOG_WARN("Não foi possível obter parâmetros de streaming (ioctl VIDIOC_G_PARM)");
        return false;
    }
    
    // Verificar se o dispositivo suporta configuração de framerate
    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        LOG_WARN("Dispositivo não suporta configuração de framerate");
        return false;
    }
    
    // Configurar o framerate
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    
    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) == -1) {
        LOG_WARN("Falha ao configurar framerate (ioctl VIDIOC_S_PARM)");
        return false;
    }
    
    // Verificar o framerate real configurado
    uint32_t actualFps = parm.parm.capture.timeperframe.denominator / 
                         parm.parm.capture.timeperframe.numerator;
    
    if (actualFps != fps) {
        LOG_WARN("Framerate configurado: " + std::to_string(actualFps) + 
                 "fps (solicitado: " + std::to_string(fps) + "fps)");
    } else {
        LOG_INFO("Framerate configurado: " + std::to_string(actualFps) + "fps");
    }
    
    return true;
}

bool VideoCapture::initMemoryMapping() {
    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = 4; // 4 buffers
    
    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("Falha ao solicitar buffers");
        return false;
    }
    
    if (req.count < 2) {
        LOG_ERROR("Memória insuficiente");
        return false;
    }
    
    m_buffers.resize(req.count);
    
    for (size_t i = 0; i < m_buffers.size(); ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("Falha ao consultar buffer");
            cleanupBuffers();
            return false;
        }
        
        m_buffers[i].length = buf.length;
        m_buffers[i].start = mmap(nullptr, buf.length,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED,
                                  m_fd, buf.m.offset);
        
        if (m_buffers[i].start == MAP_FAILED) {
            LOG_ERROR("Falha ao mapear buffer");
            cleanupBuffers();
            return false;
        }
    }
    
    LOG_INFO("Memory mapping inicializado com " + std::to_string(m_buffers.size()) + " buffers");
    return true;
}

void VideoCapture::cleanupBuffers() {
    for (auto& buffer : m_buffers) {
        if (buffer.start && buffer.start != MAP_FAILED) {
            munmap(buffer.start, buffer.length);
        }
    }
    m_buffers.clear();
}

bool VideoCapture::startCapture() {
    if (m_fd < 0) {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }
    
    if (m_streaming) {
        LOG_WARN("Captura já iniciada");
        return true;
    }
    
    if (m_buffers.empty()) {
        if (!initMemoryMapping()) {
            return false;
        }
    }
    
    // Enfileirar todos os buffers
    for (size_t i = 0; i < m_buffers.size(); ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("Falha ao enfileirar buffer");
            stopCapture();
            return false;
        }
    }
    
    // Iniciar streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Falha ao iniciar streaming");
        return false;
    }
    
    m_streaming = true;
    LOG_INFO("Captura iniciada");
    return true;
}

void VideoCapture::stopCapture() {
    if (!m_streaming) {
        return;
    }
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(m_fd, VIDIOC_STREAMOFF, &type);
    
    m_streaming = false;
    LOG_INFO("Captura parada");
}

bool VideoCapture::captureFrame(Frame& frame) {
    if (!m_streaming) {
        return false;
    }
    
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    // Tentar obter um frame (non-blocking)
    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN) {
            LOG_ERROR("Erro ao capturar frame");
        }
        return false;
    }
    
    // Preencher frame
    frame.data = static_cast<uint8_t*>(m_buffers[buf.index].start);
    frame.size = buf.length;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat;
    
    // Reenfileirar o buffer
    if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Falha ao reenfileirar buffer");
        return false;
    }
    
    return true;
}

bool VideoCapture::captureLatestFrame(Frame& frame) {
    if (!m_streaming) {
        return false;
    }
    
    // Descartar todos os frames antigos e pegar apenas o mais recente
    Frame tempFrame;
    bool gotFrame = false;
    
    // Loop para descartar frames antigos até não haver mais frames disponíveis
    while (captureFrame(tempFrame)) {
        // Guardar o último frame capturado
        frame = tempFrame;
        gotFrame = true;
        // Continuar tentando até não haver mais frames
    }
    
    return gotFrame;
}

std::vector<uint32_t> VideoCapture::getSupportedFormats() {
    std::vector<uint32_t> formats;
    
    if (m_fd < 0) {
        return formats;
    }
    
    struct v4l2_fmtdesc fmtDesc = {};
    fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    while (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0) {
        formats.push_back(fmtDesc.pixelformat);
        fmtDesc.index++;
    }
    
    return formats;
}

bool VideoCapture::setControl(uint32_t controlId, int32_t value) {
    if (m_fd < 0) {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }
    
    struct v4l2_control ctrl = {};
    ctrl.id = controlId;
    ctrl.value = value;
    
    if (ioctl(m_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Falha ao definir controle V4L2 (ID: " + std::to_string(controlId) + 
                 ", valor: " + std::to_string(value) + "): " + strerror(errno));
        return false;
    }
    
    return true;
}

bool VideoCapture::getControl(uint32_t controlId, int32_t& value) {
    if (m_fd < 0) {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }
    
    struct v4l2_control ctrl = {};
    ctrl.id = controlId;
    
    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0) {
        LOG_WARN("Falha ao obter controle V4L2 (ID: " + std::to_string(controlId) + ")");
        return false;
    }
    
    value = ctrl.value;
    return true;
}

bool VideoCapture::getControl(uint32_t controlId, int32_t& value, int32_t& min, int32_t& max, int32_t& step) {
    if (m_fd < 0) {
        LOG_ERROR("Dispositivo não está aberto");
        return false;
    }
    
    // Primeiro, obter informações do controle (min, max, step)
    struct v4l2_queryctrl queryctrl = {};
    queryctrl.id = controlId;
    
    if (ioctl(m_fd, VIDIOC_QUERYCTRL, &queryctrl) < 0) {
        // Controle não disponível
        return false;
    }
    
    // Verificar se o controle está desabilitado
    if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
        return false;
    }
    
    min = queryctrl.minimum;
    max = queryctrl.maximum;
    step = queryctrl.step;
    
    // Agora obter o valor atual
    struct v4l2_control ctrl = {};
    ctrl.id = controlId;
    
    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0) {
        return false;
    }
    
    value = ctrl.value;
    return true;
}

bool VideoCapture::setBrightness(int32_t value) {
    return setControl(V4L2_CID_BRIGHTNESS, value);
}

bool VideoCapture::setContrast(int32_t value) {
    return setControl(V4L2_CID_CONTRAST, value);
}

bool VideoCapture::setSaturation(int32_t value) {
    return setControl(V4L2_CID_SATURATION, value);
}

bool VideoCapture::setHue(int32_t value) {
    return setControl(V4L2_CID_HUE, value);
}

bool VideoCapture::setGain(int32_t value) {
    return setControl(V4L2_CID_GAIN, value);
}

bool VideoCapture::setExposure(int32_t value) {
    return setControl(V4L2_CID_EXPOSURE_ABSOLUTE, value);
}

bool VideoCapture::setSharpness(int32_t value) {
    return setControl(V4L2_CID_SHARPNESS, value);
}

bool VideoCapture::setGamma(int32_t value) {
    return setControl(V4L2_CID_GAMMA, value);
}

bool VideoCapture::setWhiteBalanceTemperature(int32_t value) {
    return setControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, value);
}

bool VideoCapture::convertYUYVtoRGB(const Frame& /*input*/, std::vector<uint8_t>& /*output*/) {
    // Esta função não é mais usada - a conversão é feita em Application
    return false;
}

