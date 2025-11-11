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
    
    // Extrair diretório base do preset
    std::filesystem::path path(presetPath);
    m_basePath = path.parent_path().string();
    if (m_basePath.empty()) {
        m_basePath = ".";
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
    
    // Primeiro, tentar resolver a partir do basePath (diretório do preset)
    std::filesystem::path basePath(m_basePath);
    std::filesystem::path relPath(path);
    std::filesystem::path resolved = (basePath / relPath).lexically_normal();
    
    // Verificar se o arquivo existe
    if (std::filesystem::exists(resolved)) {
        return resolved.string();
    }
    
    // Se não encontrou e o caminho contém "../", tentar mapear para estrutura RetroArch
    std::string pathStr = path;
    
    // Remover todos os "../" do início
    std::string cleanPath = pathStr;
    while (cleanPath.find("../") == 0) {
        cleanPath = cleanPath.substr(3);
    }
    
    // Se o caminho limpo começa com "crt/", mapear para shaders/shaders_slang/crt/
    if (cleanPath.find("crt/") == 0) {
        std::filesystem::path currentPath = std::filesystem::current_path();
        // Mapear crt/... para shaders/shaders_slang/crt/...
        std::filesystem::path slangPath = currentPath / "shaders" / "shaders_slang" / cleanPath;
        
        if (std::filesystem::exists(slangPath)) {
            LOG_INFO("Shader encontrado: " + slangPath.string());
            return slangPath.string();
        } else {
            LOG_WARN("Tentou encontrar: " + slangPath.string() + " mas não existe");
        }
    }
    
    // Tentar resolver a partir do diretório de trabalho atual
    std::filesystem::path currentPath = std::filesystem::current_path();
    resolved = (currentPath / relPath).lexically_normal();
    
    if (std::filesystem::exists(resolved)) {
        return resolved.string();
    }
    
    // Se ainda não encontrou, tentar em shaders/shaders_slang/ diretamente
    if (cleanPath.find("shaders/") != 0 && cleanPath.find("crt/") != 0) {
        std::filesystem::path slangPath = currentPath / "shaders" / "shaders_slang" / cleanPath;
        if (std::filesystem::exists(slangPath)) {
            return slangPath.string();
        }
    }
    
    // Retornar o caminho resolvido mesmo que não exista (para mostrar erro mais claro)
    return resolved.string();
}

float ShaderPreset::parseFloat(const std::string& value) {
    try {
        return std::stof(value);
    } catch (...) {
        return 0.0f;
    }
}

