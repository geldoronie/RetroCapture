#pragma once

#include "glad_loader.h"
#include <cstdint>

/**
 * Rastreia estados OpenGL para evitar mudanças desnecessárias.
 * Reduz overhead de chamadas glBindTexture, glActiveTexture, etc.
 */
class OpenGLStateTracker {
public:
    OpenGLStateTracker();
    
    /**
     * Bind texture apenas se diferente da textura atual.
     * @param texture Texture ID to bind
     * @return true if texture was actually bound, false if already bound
     */
    bool bindTexture(GLenum target, GLuint texture);
    
    /**
     * Set active texture unit apenas se diferente da unidade atual.
     * @param texture Texture unit (GL_TEXTURE0, GL_TEXTURE1, etc.)
     * @return true if texture unit was actually changed, false if already active
     */
    bool setActiveTexture(GLenum texture);
    
    /**
     * Reset tracked state (usar quando contexto OpenGL muda ou é recriado).
     */
    void reset();
    
private:
    GLuint m_currentTexture[32]; // Suporta até 32 unidades de textura
    GLenum m_currentActiveTexture;
    bool m_initialized;
};

