#pragma once

#include "glad_loader.h"
#include <cstdint>
#include <vector>
#include <mutex>

/**
 * Gerencia PBOs (Pixel Buffer Objects) para leitura assíncrona de pixels do framebuffer.
 * Usa double-buffering: enquanto um PBO é lido, o outro é preenchido.
 */
class PBOManager {
public:
    PBOManager();
    ~PBOManager();
    
    /**
     * Inicializar PBOs com tamanho específico.
     * @param width Largura da imagem
     * @param height Altura da imagem
     * @return true se inicializado com sucesso
     */
    bool init(uint32_t width, uint32_t height);
    
    /**
     * Limpar recursos (deletar PBOs).
     */
    void cleanup();
    
    /**
     * Verificar se PBOs estão inicializados.
     */
    bool isInitialized() const { return m_initialized; }
    
    /**
     * Iniciar leitura assíncrona do framebuffer para o PBO atual.
     * @param x Coordenada X do viewport
     * @param y Coordenada Y do viewport
     * @param width Largura da região a ler
     * @param height Altura da região a ler
     * @return true se iniciado com sucesso
     */
    bool startAsyncRead(GLint x, GLint y, GLsizei width, GLsizei height);
    
    /**
     * Obter dados do PBO que foi lido anteriormente (não bloqueia).
     * @param data Buffer de saída (deve ter tamanho width * height * 3)
     * @param width Largura esperada
     * @param height Altura esperada
     * @return true se dados estão disponíveis e foram copiados
     */
    bool getReadData(uint8_t* data, uint32_t width, uint32_t height);
    
    /**
     * Verificar se há dados prontos para leitura.
     */
    bool hasDataReady() const;
    
    /**
     * Redimensionar PBOs se necessário (chamado quando dimensões mudam).
     */
    void resizeIfNeeded(uint32_t width, uint32_t height);
    
private:
    bool m_initialized;
    uint32_t m_width;
    uint32_t m_height;
    size_t m_bufferSize; // Tamanho do buffer em bytes (width * height * 3)
    
    // Double-buffering: 2 PBOs
    GLuint m_pbo[2];
    int m_currentPBO; // Índice do PBO atual (0 ou 1)
    int m_nextPBO;    // Índice do próximo PBO (1 ou 0)
    
    // Mutex para thread-safety
    mutable std::mutex m_mutex;
    
    /**
     * Calcular tamanho do buffer necessário.
     */
    size_t calculateBufferSize(uint32_t width, uint32_t height) const;
    
    /**
     * Criar PBOs.
     */
    bool createPBOs();
    
    /**
     * Deletar PBOs.
     */
    void deletePBOs();
};

