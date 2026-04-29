#include "PBOManager.h"
#include "../utils/Logger.h"
#include <cstring>

PBOManager::PBOManager()
    : m_initialized(false)
    , m_width(0)
    , m_height(0)
    , m_format(GL_RGB)
    , m_bytesPerPixel(3)
    , m_bufferSize(0)
    , m_currentPBO(0)
    , m_nextPBO(1)
    , m_readsStarted(0)
{
    m_pbo[0] = 0;
    m_pbo[1] = 0;
}

PBOManager::~PBOManager()
{
    cleanup();
}

bool PBOManager::init(uint32_t width, uint32_t height, GLenum format)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    uint32_t bpp = (format == GL_RGBA) ? 4 : 3;

    if (m_initialized)
    {
        if (m_format != format || m_bytesPerPixel != bpp)
        {
            deletePBOs();
            m_format = format;
            m_bytesPerPixel = bpp;
            m_width = width;
            m_height = height;
            m_bufferSize = calculateBufferSize(width, height);
            m_readsStarted = 0;
            if (!createPBOs())
            {
                m_initialized = false;
                return false;
            }
            return true;
        }
        resizeIfNeeded(width, height);
        return true;
    }

    m_format = format;
    m_bytesPerPixel = bpp;
    m_width = width;
    m_height = height;
    m_bufferSize = calculateBufferSize(width, height);

    if (!createPBOs())
    {
        LOG_WARN("PBOs not supported or failed to create - will use synchronous glReadPixels");
        return false;
    }

    m_initialized = true;
    LOG_INFO("PBOs initialized: " + std::to_string(width) + "x" + std::to_string(height) +
             " (" + (format == GL_RGBA ? "RGBA" : "RGB") + ")");
    return true;
}

void PBOManager::cleanup()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized)
    {
        deletePBOs();
        m_initialized = false;
        m_width = 0;
        m_height = 0;
        m_bufferSize = 0;
        m_readsStarted = 0;
    }
}

bool PBOManager::startAsyncRead(GLint x, GLint y, GLsizei width, GLsizei height)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized)
    {
        return false;
    }
    
    // Redimensionar se necessário
    if (width != static_cast<GLsizei>(m_width) || height != static_cast<GLsizei>(m_height))
    {
        resizeIfNeeded(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
    
    // Alternar PBOs: usar o próximo PBO para leitura
    std::swap(m_currentPBO, m_nextPBO);

    // Bind o PBO atual para glReadPixels escrever nele
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_currentPBO]);

    // Iniciar leitura assíncrona (não bloqueia): glReadPixels escreve no PBO
    // mas não espera a transferência completar.
    glReadPixels(x, y, width, height, m_format, GL_UNSIGNED_BYTE, 0);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (m_readsStarted < 2)
    {
        m_readsStarted++;
    }

    return true;
}

bool PBOManager::getReadData(uint8_t* data, uint32_t width, uint32_t height, bool flipY)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized || !data)
    {
        return false;
    }

    if (width != m_width || height != m_height)
    {
        return false;
    }

    // Precisamos de pelo menos 2 startAsyncRead antes de ler m_nextPBO,
    // senão o "PBO anterior" tem conteúdo indefinido (recém-alocado).
    if (m_readsStarted < 2)
    {
        return false;
    }

    // Bind o PBO que foi iniciado no frame anterior (m_nextPBO, devido ao swap em startAsyncRead).
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_nextPBO]);

    void* mappedData = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);

    if (mappedData)
    {
        size_t rowSizeUnpadded = static_cast<size_t>(width) * m_bytesPerPixel;
        size_t rowSizePadded = ((rowSizeUnpadded + 3) / 4) * 4;

        const uint8_t* src = static_cast<const uint8_t*>(mappedData);

        for (uint32_t row = 0; row < height; row++)
        {
            uint32_t srcRow = flipY ? (height - 1 - row) : row;
            const uint8_t* srcPtr = src + (srcRow * rowSizePadded);
            uint8_t* dstPtr = data + (row * rowSizeUnpadded);
            memcpy(dstPtr, srcPtr, rowSizeUnpadded);
        }

        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        return true;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return false;
}

bool PBOManager::hasDataReady() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized)
    {
        return false;
    }
    
    // Verificar se o PBO anterior tem dados prontos
    // Usar glMapBuffer com GL_READ_ONLY para verificar
    // Nota: glMapBuffer pode bloquear, mas como estamos verificando o PBO anterior
    // (que foi lido no frame anterior), ele deve estar pronto
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_nextPBO]);
    
    // Tentar mapear (pode bloquear se dados não estão prontos, mas isso é aceitável)
    void* mappedData = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    
    if (mappedData)
    {
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return true;
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return false;
}

void PBOManager::resizeIfNeeded(uint32_t width, uint32_t height)
{
    if (m_width == width && m_height == height && m_initialized)
    {
        return; // Não precisa redimensionar
    }
    
    deletePBOs();

    m_width = width;
    m_height = height;
    m_bufferSize = calculateBufferSize(width, height);
    m_readsStarted = 0;

    if (!createPBOs())
    {
        LOG_ERROR("Failed to resize PBOs");
        m_initialized = false;
    }
}

size_t PBOManager::calculateBufferSize(uint32_t width, uint32_t height) const
{
    // Padding de 4 bytes por linha (default GL_PACK_ALIGNMENT).
    size_t rowSizeUnpadded = static_cast<size_t>(width) * m_bytesPerPixel;
    size_t rowSizePadded = ((rowSizeUnpadded + 3) / 4) * 4;
    return rowSizePadded * static_cast<size_t>(height);
}

bool PBOManager::createPBOs()
{
    // Criar 2 PBOs para double-buffering
    glGenBuffers(2, m_pbo);
    
    if (m_pbo[0] == 0 || m_pbo[1] == 0)
    {
        LOG_ERROR("Failed to generate PBOs");
        return false;
    }
    
    // Alocar espaço para ambos os PBOs
    for (int i = 0; i < 2; i++)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, m_bufferSize, nullptr, GL_STREAM_READ);
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    return true;
}

void PBOManager::deletePBOs()
{
    if (m_pbo[0] != 0 || m_pbo[1] != 0)
    {
        glDeleteBuffers(2, m_pbo);
        m_pbo[0] = 0;
        m_pbo[1] = 0;
    }
}

