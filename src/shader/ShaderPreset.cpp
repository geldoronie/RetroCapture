#include "ShaderPreset.h"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

ShaderPreset::ShaderPreset() {
}

ShaderPreset::~ShaderPreset() {
}

bool ShaderPreset::load(const std::string& presetPath) {
    m_passes.clear();
    m_textures.clear();
    m_parameters.clear();
    
    // Extrair diretório base do preset (como caminho absoluto)
    std::filesystem::path path(presetPath);
    if (path.is_relative()) {
        // Se o caminho é relativo, converter para absoluto
        path = std::filesystem::absolute(path);
    }
    m_basePath = path.parent_path().string();
    if (m_basePath.empty()) {
        m_basePath = std::filesystem::current_path().string();
    }
    
    std::ifstream file(presetPath);
    if (!file.is_open()) {
        LOG_ERROR("Falha ao abrir preset: " + presetPath);
        return false;
    }
    
    std::string line;
    int numShaders = 0;
    
    while (std::getline(file, line)) {
        // Remover espaços em branco
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty() || line[0] == '#') {
            continue; // Comentário ou linha vazia
        }
        
        // Verificar número de shaders
        if (line.find("shaders =") == 0) {
            size_t eqPos = line.find('=');
            if (eqPos != std::string::npos) {
                std::string value = line.substr(eqPos + 1);
                value.erase(0, value.find_first_not_of(" \t\""));
                value.erase(value.find_last_not_of(" \t\"") + 1);
                numShaders = static_cast<int>(parseFloat(value));
                m_passes.resize(numShaders);
            }
            continue;
        }
        
        // Parsear linha
        parseLine(line, numShaders);
    }
    
    file.close();
    
    LOG_INFO("Preset carregado: " + std::to_string(m_passes.size()) + " passes, " + 
             std::to_string(m_textures.size()) + " texturas");
    
    return !m_passes.empty();
}

bool ShaderPreset::parseLine(const std::string& line, int& passIndex) {
    size_t eqPos = line.find('=');
    if (eqPos == std::string::npos) {
        return false;
    }
    
    std::string key = line.substr(0, eqPos);
    std::string value = line.substr(eqPos + 1);
    
    // Remover espaços e aspas
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t\""));
    value.erase(value.find_last_not_of(" \t\"") + 1);
    
    // Extrair índice do pass
    size_t numPos = key.find_first_of("0123456789");
    if (numPos != std::string::npos) {
        int idx = std::stoi(key.substr(numPos));
        if (idx >= static_cast<int>(m_passes.size())) {
            m_passes.resize(idx + 1);
        }
        passIndex = idx;
        
        ShaderPass& pass = m_passes[idx];
        
        if (key.find("shader") == 0) {
            pass.shaderPath = resolvePath(value);
        } else if (key.find("filter_linear") == 0) {
            pass.filterLinear = (value == "true");
        } else if (key.find("wrap_mode") == 0) {
            pass.wrapMode = value;
        } else if (key.find("mipmap_input") == 0) {
            pass.mipmapInput = (value == "true");
        } else if (key.find("alias") == 0) {
            pass.alias = value;
        } else if (key.find("float_framebuffer") == 0) {
            pass.floatFramebuffer = (value == "true");
        } else if (key.find("srgb_framebuffer") == 0) {
            pass.srgbFramebuffer = (value == "true");
        } else if (key.find("scale_type_x") == 0) {
            pass.scaleTypeX = value;
        } else if (key.find("scale_x") == 0) {
            pass.scaleX = parseFloat(value);
        } else if (key.find("scale_type_y") == 0) {
            pass.scaleTypeY = value;
        } else if (key.find("scale_y") == 0) {
            pass.scaleY = parseFloat(value);
        }
    } else {
        // Texturas ou parâmetros globais
        if (key.find("Sampler") == 0 && key.find("_wrap_mode") == std::string::npos && 
            key.find("_mipmap") == std::string::npos) {
            // É uma textura
            ShaderTexture tex;
            tex.path = resolvePath(value);
            m_textures[key] = tex;
        } else if (key.find("Sampler") == 0 && key.find("_wrap_mode") != std::string::npos) {
            // Wrap mode de textura
            std::string texName = key.substr(0, key.find("_wrap_mode"));
            if (m_textures.find(texName) != m_textures.end()) {
                m_textures[texName].wrapMode = value;
            }
        } else if (key.find("Sampler") == 0 && key.find("_mipmap") != std::string::npos) {
            // Mipmap de textura
            std::string texName = key.substr(0, key.find("_mipmap"));
            if (m_textures.find(texName) != m_textures.end()) {
                m_textures[texName].mipmap = (value == "true");
            }
        } else if (key.find("_linear") != std::string::npos) {
            // Linear filtering de textura (ex: SamplerLUT_linear, noise1_linear)
            std::string texName = key.substr(0, key.find("_linear"));
            if (m_textures.find(texName) != m_textures.end()) {
                m_textures[texName].linear = (value == "true");
            }
        } else if (key.find("frame_count_mod") == 0) {
            // frame_count_mod# - módulo para FrameCount por pass
            // Armazenar como parâmetro especial
            m_parameters[key] = parseFloat(value);
        } else {
            // Parâmetro global (uniform)
            m_parameters[key] = parseFloat(value);
        }
    }
    
    return true;
}

std::string ShaderPreset::resolvePath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    
    // Se é caminho absoluto, retornar como está
    if (path[0] == '/') {
        return path;
    }
    
    std::filesystem::path currentPath = std::filesystem::current_path();
    std::filesystem::path relPath(path);
    
    // Primeiro, tentar resolver a partir do basePath (diretório do preset)
    std::filesystem::path basePath(m_basePath);
    std::filesystem::path resolved = (basePath / relPath).lexically_normal();
    
    // Verificar se o arquivo existe
    if (std::filesystem::exists(resolved)) {
        return resolved.string();
    }
    
    // Se o caminho começa com "shaders/", tentar resolver a partir de shaders/shaders_glsl/
    // (caminhos RetroArch são relativos à raiz shaders/)
    if (path.find("shaders/") == 0) {
        // Remover "shaders/" do início
        std::string subPath = path.substr(8); // Remove "shaders/"
        
        // Primeiro, tentar a partir do diretório do preset (pode haver subdiretórios como denoisers/shaders/)
        std::filesystem::path presetGlslPath = basePath / subPath;
        if (std::filesystem::exists(presetGlslPath)) {
            LOG_INFO("Shader encontrado (shaders/ relativo ao preset): " + presetGlslPath.string());
            return presetGlslPath.string();
        }
        
        // Se não encontrou, tentar a partir da raiz shaders/shaders_glsl/
        std::filesystem::path glslPath = currentPath / "shaders" / "shaders_glsl" / subPath;
        if (std::filesystem::exists(glslPath)) {
            LOG_INFO("Shader encontrado (shaders/): " + glslPath.string());
            return glslPath.string();
        }
    }
    
    // Se o caminho contém "../", processar caminhos relativos RetroArch
    std::string cleanPath = path;
    int parentLevels = 0;
    
    // Contar quantos "../" existem
    while (cleanPath.find("../") == 0) {
        cleanPath = cleanPath.substr(3);
        parentLevels++;
    }
    
    // Se temos "../", calcular o caminho base a partir do diretório do preset
    if (parentLevels > 0) {
        // IMPORTANTE: Os presets RetroArch assumem que "../" sobe a partir da raiz shaders/
        // Exemplo: "../crt/shaders/..." a partir de shaders/shaders_glsl/denoisers/
        // deve ir para shaders/shaders_glsl/crt/shaders/...
        // Então sempre tentar primeiro a partir de shaders/shaders_glsl/
        std::filesystem::path glslBase = currentPath / "shaders" / "shaders_glsl";
        
        // Tentar o caminho exato primeiro
        resolved = (glslBase / cleanPath).lexically_normal();
        if (std::filesystem::exists(resolved)) {
            LOG_INFO("Shader encontrado (../ em shaders_glsl): " + resolved.string());
            return resolved.string();
        }
        
        // Se não encontrou, tentar buscar recursivamente em subdiretórios
        // Exemplo: "crt/shaders/crt-hyllian.glsl" pode estar em "crt/shaders/hyllian/crt-hyllian.glsl"
        std::string cleanPathStr = cleanPath;
        std::filesystem::path searchBase = glslBase;
        
        // Extrair o nome do arquivo
        size_t lastSlash = cleanPathStr.find_last_of('/');
        if (lastSlash != std::string::npos) {
            std::string dirPart = cleanPathStr.substr(0, lastSlash);
            std::string filePart = cleanPathStr.substr(lastSlash + 1);
            
            // Tentar buscar o arquivo recursivamente no diretório
            std::filesystem::path dirPath = searchBase / dirPart;
            if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
                    if (entry.is_regular_file() && entry.path().filename().string() == filePart) {
                        LOG_INFO("Shader encontrado (busca recursiva): " + entry.path().string());
                        return entry.path().string();
                    }
                }
            }
        }
        
        // Se não encontrou, tentar a partir do diretório do preset (caso seja estrutura diferente)
        std::filesystem::path base = basePath;
        if (std::filesystem::path(basePath).is_relative()) {
            base = currentPath / basePath;
        }
        
        // Verificar se o basePath contém "shaders_glsl" - se sim, usar como base
        std::string baseStr = base.string();
        if (baseStr.find("shaders_glsl") != std::string::npos) {
            // Encontrar a posição de "shaders_glsl" e usar como base
            size_t pos = baseStr.find("shaders_glsl");
            std::filesystem::path glslBaseFromPath = std::filesystem::path(baseStr.substr(0, pos + 11));
            resolved = (glslBaseFromPath / cleanPath).lexically_normal();
            if (std::filesystem::exists(resolved)) {
                LOG_INFO("Shader encontrado (../ relativo a shaders_glsl no path): " + resolved.string());
                return resolved.string();
            }
        }
        
        // Fallback: subir parentLevels níveis a partir do basePath
        for (int i = 0; i < parentLevels; ++i) {
            base = base.parent_path();
        }
        
        // Tentar resolver a partir do caminho calculado (relativo ao preset)
        resolved = (base / cleanPath).lexically_normal();
        if (std::filesystem::exists(resolved)) {
            LOG_INFO("Shader encontrado (../ relativo ao preset): " + resolved.string());
            return resolved.string();
        }
    }
    
    // Tentar resolver a partir do diretório de trabalho atual
    resolved = (currentPath / relPath).lexically_normal();
    if (std::filesystem::exists(resolved)) {
        return resolved.string();
    }
    
    // Se o caminho limpo começa com "crt/", "xbr/", etc, tentar em shaders/shaders_glsl/
    if (cleanPath.find("crt/") == 0 || cleanPath.find("xbr/") == 0 || 
        cleanPath.find("denoisers/") == 0 || cleanPath.find("guest/") == 0) {
        std::filesystem::path glslPath = currentPath / "shaders" / "shaders_glsl" / cleanPath;
        if (std::filesystem::exists(glslPath)) {
            LOG_INFO("Shader encontrado (crt/xbr/etc): " + glslPath.string());
            return glslPath.string();
        }
    }
    
    // Tentar em shaders/shaders_glsl/ diretamente
    std::filesystem::path glslPath = currentPath / "shaders" / "shaders_glsl" / cleanPath;
    if (std::filesystem::exists(glslPath)) {
        LOG_INFO("Shader encontrado (shaders_glsl): " + glslPath.string());
        return glslPath.string();
    }
    
    // Retornar o caminho resolvido mesmo que não exista (para mostrar erro mais claro)
    LOG_WARN("Shader não encontrado: " + path + " (tentou: " + resolved.string() + ")");
    return resolved.string();
}

float ShaderPreset::parseFloat(const std::string& value) {
    try {
        return std::stof(value);
    } catch (...) {
        return 0.0f;
    }
}

