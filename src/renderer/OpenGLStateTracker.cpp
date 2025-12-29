#include "OpenGLStateTracker.h"

OpenGLStateTracker::OpenGLStateTracker()
    : m_currentActiveTexture(GL_TEXTURE0)
    , m_initialized(false)
{
    reset();
}

bool OpenGLStateTracker::bindTexture(GLenum target, GLuint texture)
{
    if (target != GL_TEXTURE_2D)
    {
        // Por enquanto, apenas suporta GL_TEXTURE_2D
        glBindTexture(target, texture);
        return true;
    }
    
    // Obter índice da unidade de textura atual (0-31)
    GLenum activeTexture = m_currentActiveTexture;
    int unitIndex = activeTexture - GL_TEXTURE0;
    if (unitIndex < 0 || unitIndex >= 32)
    {
        unitIndex = 0; // Fallback para unidade 0
    }
    
    // IMPORTANTE: Sempre fazer bind se não tivermos certeza do estado
    // O ShaderEngine e outros componentes podem alterar o estado OpenGL,
    // então não podemos confiar completamente no cache
    // Por segurança, sempre fazer bind (o overhead é mínimo comparado ao risco de bug)
    glBindTexture(target, texture);
    m_currentTexture[unitIndex] = texture;
    m_initialized = true;
    return true;
}

bool OpenGLStateTracker::setActiveTexture(GLenum texture)
{
    if (m_initialized && m_currentActiveTexture == texture)
    {
        return false; // Já está ativa, não precisa mudar
    }
    
    glActiveTexture(texture);
    m_currentActiveTexture = texture;
    m_initialized = true;
    return true;
}

void OpenGLStateTracker::reset()
{
    for (int i = 0; i < 32; ++i)
    {
        m_currentTexture[i] = 0;
    }
    m_currentActiveTexture = GL_TEXTURE0;
    m_initialized = false;
}

