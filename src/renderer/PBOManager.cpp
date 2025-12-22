#include "PBOManager.h"
#include "../utils/Logger.h"
#include <cstring>

PBOManager::PBOManager()
    : m_initialized(false)
    , m_width(0)
    , m_height(0)
    , m_bufferSize(0)
    , m_currentPBO(0)
    , m_nextPBO(1)
{
    m_pbo[0] = 0;
    m_pbo[1] = 0;
}

PBOManager::~PBOManager()
{
    cleanup();
}

bool PBOManager::init(uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized)
    {
        // Já inicializado, apenas redimensionar se necessário
        resizeIfNeeded(width, height);
        return true;
    }
    
    m_width = width;
    m_height = height;
    m_bufferSize = calculateBufferSize(width, height);
    
    // Verificar se PBOs são suportados (OpenGL 2.1+ ou OpenGL ES 3.0+)
    // Se não suportados, createPBOs() falhará e retornaremos false
    if (!createPBOs())
    {
        LOG_WARN("PBOs not supported or failed to create - will use synchronous glReadPixels");
        return false;
    }
    
    m_initialized = true;
    LOG_INFO("PBOs initialized: " + std::to_string(width) + "x" + std::to_string(height));
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
    
    // Iniciar leitura assíncrona (não bloqueia)
    // glReadPixels escreve no PBO, mas não espera a transferência completar
    glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, 0);
    
    // Unbind PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    return true;
}

bool PBOManager::getReadData(uint8_t* data, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized || !data)
    {
        return false;
    }
    
    // Verificar se dimensões correspondem
    if (width != m_width || height != m_height)
    {
        return false;
    }
    
    // Bind o PBO que foi lido anteriormente (o próximo, não o atual)
    // IMPORTANTE: m_nextPBO é o PBO que foi iniciado no frame anterior
    // porque startAsyncRead() faz swap antes de iniciar a leitura
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_nextPBO]);
    
    // Mapear o PBO para leitura
    // NOTA: glMapBuffer pode bloquear se a transferência ainda não completou,
    // mas isso é aceitável pois estamos lendo o PBO do frame anterior
    // que já teve tempo para transferir
    void* mappedData = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    
    if (mappedData)
    {
        // Copiar dados (com padding se necessário)
        size_t readRowSizeUnpadded = static_cast<size_t>(width) * 3;
        size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
        
        const uint8_t* src = static_cast<const uint8_t*>(mappedData);
        
        // glReadPixels retorna de baixo para cima, precisamos inverter
        for (uint32_t row = 0; row < height; row++)
        {
            uint32_t srcRow = height - 1 - row; // Ler de baixo para cima
            uint32_t dstRow = row; // Escrever de cima para baixo
            
            const uint8_t* srcPtr = src + (srcRow * readRowSizePadded);
            uint8_t* dstPtr = data + (dstRow * readRowSizeUnpadded);
            memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
        }
        
        // Desmapear PBO
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        
        return true;
    }
    else
    {
        // Transferência ainda não completou, dados não disponíveis
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }
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
    
    // Deletar PBOs antigos
    deletePBOs();
    
    // Atualizar dimensões
    m_width = width;
    m_height = height;
    m_bufferSize = calculateBufferSize(width, height);
    
    // Recriar PBOs
    if (!createPBOs())
    {
        LOG_ERROR("Failed to resize PBOs");
        m_initialized = false;
    }
}

size_t PBOManager::calculateBufferSize(uint32_t width, uint32_t height) const
{
    // Calcular tamanho com padding (alinhamento de 4 bytes)
    size_t rowSizeUnpadded = static_cast<size_t>(width) * 3;
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

