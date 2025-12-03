#include "ShaderPreset.h"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

ShaderPreset::ShaderPreset()
{
}

ShaderPreset::~ShaderPreset()
{
}

bool ShaderPreset::load(const std::string &presetPath)
{
    m_passes.clear();
    m_textures.clear();
    m_parameters.clear();

    // Extrair diretório base do preset (como caminho absoluto)
    std::filesystem::path path(presetPath);
    if (path.is_relative())
    {
        // Se o caminho é relativo, converter para absoluto
        path = std::filesystem::absolute(path);
    }
    m_basePath = path.parent_path().string();
    if (m_basePath.empty())
    {
        m_basePath = std::filesystem::current_path().string();
    }
    m_presetPath = path.string(); // Armazenar caminho completo do preset

    std::ifstream file(presetPath);
    if (!file.is_open())
    {
        LOG_ERROR("Falha ao abrir preset: " + presetPath);
        return false;
    }

    std::string line;
    int numShaders = 0;

    while (std::getline(file, line))
    {
        // Remover espaços em branco
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#')
        {
            continue; // Comentário ou linha vazia
        }

        // Verificar número de shaders
        if (line.find("shaders =") == 0)
        {
            size_t eqPos = line.find('=');
            if (eqPos != std::string::npos)
            {
                std::string value = line.substr(eqPos + 1);
                value.erase(0, value.find_first_not_of(" \t\""));
                value.erase(value.find_last_not_of(" \t\"") + 1);
                numShaders = static_cast<int>(parseFloat(value));
                m_passes.resize(numShaders);
            }
            continue;
        }

        // Verificar linha de texturas (textures = NAME1;NAME2;...)
        if (line.find("textures =") == 0)
        {
            size_t eqPos = line.find('=');
            if (eqPos != std::string::npos)
            {
                std::string value = line.substr(eqPos + 1);
                value.erase(0, value.find_first_not_of(" \t\""));
                value.erase(value.find_last_not_of(" \t\"") + 1);
                // Parsear lista de nomes de texturas separados por ;
                std::istringstream iss(value);
                std::string texName;
                while (std::getline(iss, texName, ';'))
                {
                    // Remover espaços e aspas
                    texName.erase(0, texName.find_first_not_of(" \t\""));
                    texName.erase(texName.find_last_not_of(" \t\"") + 1);
                    if (!texName.empty())
                    {
                        // Criar entrada de textura vazia (o caminho será definido depois)
                        ShaderTexture tex;
                        m_textures[texName] = tex;
                        LOG_INFO("Textura declarada: " + texName);
                    }
                }
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

bool ShaderPreset::parseLine(const std::string &line, int &passIndex)
{
    size_t eqPos = line.find('=');
    if (eqPos == std::string::npos)
    {
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
    if (numPos != std::string::npos)
    {
        int idx = std::stoi(key.substr(numPos));
        if (idx >= static_cast<int>(m_passes.size()))
        {
            m_passes.resize(idx + 1);
        }
        passIndex = idx;

        ShaderPass &pass = m_passes[idx];

        if (key.find("shader") == 0)
        {
            pass.shaderPath = resolvePath(value);
        }
        else if (key.find("filter_linear") == 0)
        {
            // Aceitar "true", "false", true, false (com ou sem aspas)
            std::string lowerValue = value;
            std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
            pass.filterLinear = (lowerValue == "true" || lowerValue == "1");
        }
        else if (key.find("wrap_mode") == 0)
        {
            pass.wrapMode = value;
        }
        else if (key.find("mipmap_input") == 0)
        {
            // Aceitar "true", "false", true, false (com ou sem aspas)
            std::string lowerValue = value;
            std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
            pass.mipmapInput = (lowerValue == "true" || lowerValue == "1");
        }
        else if (key.find("alias") == 0)
        {
            pass.alias = value;
        }
        else if (key.find("float_framebuffer") == 0)
        {
            // Aceitar "true", "false", true, false (com ou sem aspas)
            std::string lowerValue = value;
            std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
            pass.floatFramebuffer = (lowerValue == "true" || lowerValue == "1");
        }
        else if (key.find("srgb_framebuffer") == 0)
        {
            // Aceitar "true", "false", true, false (com ou sem aspas)
            std::string lowerValue = value;
            std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
            pass.srgbFramebuffer = (lowerValue == "true" || lowerValue == "1");
        }
        else if (key.find("scale_type_x") == 0)
        {
            pass.scaleTypeX = value;
        }
        else if (key.find("scale_x") == 0)
        {
            pass.scaleX = parseFloat(value);
        }
        else if (key.find("scale_type_y") == 0)
        {
            pass.scaleTypeY = value;
        }
        else if (key.find("scale_y") == 0)
        {
            pass.scaleY = parseFloat(value);
        }
        else if (key.find("scale_type") == 0 && key.find("scale_type_x") != 0 && key.find("scale_type_y") != 0)
        {
            // scale_type0, scale_type1, etc (sem _x ou _y) - aplicar para ambos X e Y
            pass.scaleTypeX = value;
            pass.scaleTypeY = value;
            LOG_INFO("Pass " + std::to_string(idx) + " scale_type: " + value + " (aplicado para X e Y)");
        }
        else if (key.find("scale") == 0 && key.find("scale_x") != 0 && key.find("scale_y") != 0 &&
                 key.find("scale_type") != 0)
        {
            // scale0, scale1, etc (sem _x ou _y) - aplicar para ambos X e Y
            float scaleVal = parseFloat(value);
            pass.scaleX = scaleVal;
            pass.scaleY = scaleVal;
            LOG_INFO("Pass " + std::to_string(idx) + " scale: " + value + " (aplicado para X e Y)");
        }
    }
    else
    {
        // Texturas ou parâmetros globais
        // Verificar se é uma textura (pode começar com "Sampler" ou qualquer nome como "COLOR_PALETTE", "BACKGROUND", etc)
        bool isTextureName = (m_textures.find(key) != m_textures.end());

        if ((key.find("Sampler") == 0 && key.find("_wrap_mode") == std::string::npos &&
             key.find("_mipmap") == std::string::npos) ||
            isTextureName)
        {
            // É uma textura
            ShaderTexture tex;
            tex.path = resolvePath(value);
            m_textures[key] = tex;
            LOG_INFO("Textura definida: " + key + " -> " + tex.path);
        }
        else if (key.find("Sampler") == 0 && key.find("_wrap_mode") != std::string::npos)
        {
            // Wrap mode de textura
            std::string texName = key.substr(0, key.find("_wrap_mode"));
            if (m_textures.find(texName) != m_textures.end())
            {
                m_textures[texName].wrapMode = value;
            }
        }
        else if (key.find("Sampler") == 0 && key.find("_mipmap") != std::string::npos)
        {
            // Mipmap de textura
            std::string texName = key.substr(0, key.find("_mipmap"));
            if (m_textures.find(texName) != m_textures.end())
            {
                // Aceitar "true", "false", true, false (com ou sem aspas)
                std::string lowerValue = value;
                std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
                m_textures[texName].mipmap = (lowerValue == "true" || lowerValue == "1");
            }
        }
        else if (key.find("_linear") != std::string::npos)
        {
            // Linear filtering de textura (ex: SamplerLUT_linear, noise1_linear)
            std::string texName = key.substr(0, key.find("_linear"));
            if (m_textures.find(texName) != m_textures.end())
            {
                // Aceitar "true", "false", true, false (com ou sem aspas)
                std::string lowerValue = value;
                std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
                m_textures[texName].linear = (lowerValue == "true" || lowerValue == "1");
            }
        }
        else if (key.find("frame_count_mod") == 0)
        {
            // frame_count_mod# - módulo para FrameCount por pass
            // Extrair índice do pass
            size_t numPos = key.find_first_of("0123456789");
            if (numPos != std::string::npos)
            {
                int idx = std::stoi(key.substr(numPos));
                if (idx >= 0 && idx < static_cast<int>(m_passes.size()))
                {
                    m_passes[idx].frameCountMod = static_cast<unsigned int>(parseFloat(value));
                    LOG_INFO("Pass " + std::to_string(idx) + ": frame_count_mod = " + std::to_string(m_passes[idx].frameCountMod));
                }
            }
        }
        else
        {
            // Parâmetro global (uniform)
            m_parameters[key] = parseFloat(value);
        }
    }

    return true;
}

std::string ShaderPreset::resolvePath(const std::string &path)
{
    if (path.empty())
    {
        return path;
    }

    // Se é caminho absoluto, retornar como está
    if (path[0] == '/')
    {
        return path;
    }

    std::filesystem::path currentPath = std::filesystem::current_path();

    // Usar RETROCAPTURE_SHADER_PATH se disponível (para AppImage)
    const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
    std::filesystem::path shaderBasePath;
    if (envShaderPath && std::filesystem::exists(envShaderPath))
    {
        shaderBasePath = std::filesystem::path(envShaderPath);
    }
    else
    {
        shaderBasePath = currentPath / "shaders" / "shaders_glsl";
    }

    std::filesystem::path relPath(path);

    // Primeiro, tentar resolver a partir do basePath (diretório do preset)
    std::filesystem::path basePath(m_basePath);
    std::filesystem::path resolved = (basePath / relPath).lexically_normal();

    // Verificar se o arquivo existe
    if (std::filesystem::exists(resolved))
    {
        return resolved.string();
    }

    // Se o caminho começa com "shaders/", tentar resolver a partir de shaders/shaders_glsl/
    // (caminhos RetroArch são relativos à raiz shaders/)
    if (path.find("shaders/") == 0)
    {
        // Remover "shaders/" do início
        std::string subPath = path.substr(8); // Remove "shaders/"

        // Primeiro, tentar a partir do diretório do preset (pode haver subdiretórios como denoisers/shaders/)
        std::filesystem::path presetGlslPath = basePath / subPath;
        if (std::filesystem::exists(presetGlslPath))
        {
            LOG_INFO("Shader encontrado (shaders/ relativo ao preset): " + presetGlslPath.string());
            return presetGlslPath.string();
        }

        // Se não encontrou, tentar a partir da raiz shaders/shaders_glsl/
        std::filesystem::path glslPath = shaderBasePath / subPath;
        if (std::filesystem::exists(glslPath))
        {
            LOG_INFO("Shader encontrado (shaders/): " + glslPath.string());
            return glslPath.string();
        }
    }

    // Se o caminho contém "../", processar caminhos relativos RetroArch
    std::string cleanPath = path;
    int parentLevels = 0;

    // Contar quantos "../" existem
    while (cleanPath.find("../") == 0)
    {
        cleanPath = cleanPath.substr(3);
        parentLevels++;
    }

    // Se temos "../", calcular o caminho base a partir do diretório do preset
    if (parentLevels > 0)
    {
        // IMPORTANTE: Os presets RetroArch assumem que "../" sobe a partir da raiz shaders/
        // Exemplo: "../crt/shaders/..." a partir de shaders/shaders_glsl/denoisers/
        // deve ir para shaders/shaders_glsl/crt/shaders/...
        // Então sempre tentar primeiro a partir de shaders/shaders_glsl/
        std::filesystem::path glslBase = shaderBasePath;

        // Tentar o caminho exato primeiro
        resolved = (glslBase / cleanPath).lexically_normal();
        if (std::filesystem::exists(resolved))
        {
            LOG_INFO("Shader encontrado (../ em shaders_glsl): " + resolved.string());
            return resolved.string();
        }

        // Se não encontrou, tentar buscar recursivamente em subdiretórios
        // Exemplo: "crt/shaders/crt-hyllian.glsl" pode estar em "crt/shaders/hyllian/crt-hyllian.glsl"
        std::string cleanPathStr = cleanPath;
        std::filesystem::path searchBase = glslBase;

        // Extrair o nome do arquivo
        size_t lastSlash = cleanPathStr.find_last_of('/');
        if (lastSlash != std::string::npos)
        {
            std::string dirPart = cleanPathStr.substr(0, lastSlash);
            std::string filePart = cleanPathStr.substr(lastSlash + 1);

            // Tentar buscar o arquivo recursivamente no diretório
            std::filesystem::path dirPath = searchBase / dirPart;
            if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath))
            {
                for (const auto &entry : std::filesystem::recursive_directory_iterator(dirPath))
                {
                    if (entry.is_regular_file() && entry.path().filename().string() == filePart)
                    {
                        LOG_INFO("Shader encontrado (busca recursiva): " + entry.path().string());
                        return entry.path().string();
                    }
                }
            }
        }

        // Se não encontrou, tentar a partir do diretório do preset (caso seja estrutura diferente)
        std::filesystem::path base = basePath;
        if (std::filesystem::path(basePath).is_relative())
        {
            base = currentPath / basePath;
        }

        // Verificar se o basePath contém "shaders_glsl" - se sim, usar como base
        std::string baseStr = base.string();
        if (baseStr.find("shaders_glsl") != std::string::npos)
        {
            // Encontrar a posição de "shaders_glsl" e usar como base
            size_t pos = baseStr.find("shaders_glsl");
            std::filesystem::path glslBaseFromPath = std::filesystem::path(baseStr.substr(0, pos + 11));
            resolved = (glslBaseFromPath / cleanPath).lexically_normal();
            if (std::filesystem::exists(resolved))
            {
                LOG_INFO("Shader encontrado (../ relativo a shaders_glsl no path): " + resolved.string());
                return resolved.string();
            }
        }

        // Fallback: subir parentLevels níveis a partir do basePath
        for (int i = 0; i < parentLevels; ++i)
        {
            base = base.parent_path();
        }

        // Tentar resolver a partir do caminho calculado (relativo ao preset)
        resolved = (base / cleanPath).lexically_normal();
        if (std::filesystem::exists(resolved))
        {
            LOG_INFO("Shader encontrado (../ relativo ao preset): " + resolved.string());
            return resolved.string();
        }
    }

    // Tentar resolver a partir do diretório de trabalho atual
    resolved = (currentPath / relPath).lexically_normal();
    if (std::filesystem::exists(resolved))
    {
        return resolved.string();
    }

    // Se o caminho limpo começa com "crt/", "xbr/", etc, tentar em shaders/shaders_glsl/
    if (cleanPath.find("crt/") == 0 || cleanPath.find("xbr/") == 0 ||
        cleanPath.find("denoisers/") == 0 || cleanPath.find("guest/") == 0)
    {
        std::filesystem::path glslPath = shaderBasePath / cleanPath;
        if (std::filesystem::exists(glslPath))
        {
            LOG_INFO("Shader encontrado (crt/xbr/etc): " + glslPath.string());
            return glslPath.string();
        }
    }

    // Tentar em shaders/shaders_glsl/ diretamente
    std::filesystem::path glslPath = shaderBasePath / cleanPath;
    if (std::filesystem::exists(glslPath))
    {
        LOG_INFO("Shader encontrado (shaders_glsl): " + glslPath.string());
        return glslPath.string();
    }

    // Retornar o caminho resolvido mesmo que não exista (para mostrar erro mais claro)
    LOG_WARN("Shader não encontrado: " + path + " (tentou: " + resolved.string() + ")");
    return resolved.string();
}

float ShaderPreset::parseFloat(const std::string &value)
{
    try
    {
        return std::stof(value);
    }
    catch (...)
    {
        return 0.0f;
    }
}

bool ShaderPreset::save(const std::string &presetPath, const std::unordered_map<std::string, float> &customParameters) const
{
    return saveAs(presetPath, customParameters);
}

bool ShaderPreset::saveAs(const std::string &presetPath, const std::unordered_map<std::string, float> &customParameters) const
{
    if (m_presetPath.empty())
    {
        LOG_ERROR("Nenhum preset carregado para salvar");
        return false;
    }

    // Ler arquivo original
    std::ifstream inputFile(m_presetPath);
    if (!inputFile.is_open())
    {
        LOG_ERROR("Falha ao abrir preset original para leitura: " + m_presetPath);
        return false;
    }

    // Ler todas as linhas do arquivo original
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(inputFile, line))
    {
        lines.push_back(line);
    }
    inputFile.close();

    // Criar map de parâmetros atualizados (combinar parâmetros originais com customizados)
    std::unordered_map<std::string, float> updatedParameters = m_parameters;
    for (const auto &customParam : customParameters)
    {
        updatedParameters[customParam.first] = customParam.second;
    }

    // Escrever arquivo novo/atualizado
    std::ofstream outputFile(presetPath);
    if (!outputFile.is_open())
    {
        LOG_ERROR("Falha ao criar arquivo de preset: " + presetPath);
        return false;
    }

    // Processar cada linha, atualizando parâmetros quando necessário
    for (const std::string &originalLine : lines)
    {
        std::string processedLine = originalLine;

        // Verificar se é uma linha de parâmetro (formato: paramName = value)
        size_t eqPos = processedLine.find('=');
        if (eqPos != std::string::npos)
        {
            std::string key = processedLine.substr(0, eqPos);
            // Remover espaços
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);

            // Verificar se é um parâmetro que precisa ser atualizado
            if (updatedParameters.find(key) != updatedParameters.end())
            {
                // Atualizar valor do parâmetro
                float newValue = updatedParameters.at(key);
                std::string newValueStr = std::to_string(newValue);
                // Remover zeros desnecessários no final
                size_t dotPos = newValueStr.find('.');
                if (dotPos != std::string::npos)
                {
                    // Remover zeros à direita após o ponto decimal
                    while (newValueStr.size() > dotPos + 1 && newValueStr.back() == '0')
                    {
                        newValueStr.pop_back();
                    }
                    // Se só sobrou o ponto, remover também
                    if (newValueStr.back() == '.')
                    {
                        newValueStr.pop_back();
                    }
                }

                // Reconstruir linha com novo valor
                std::string valuePart = processedLine.substr(eqPos + 1);
                // Preservar espaços e aspas se existirem
                size_t firstNonSpace = valuePart.find_first_not_of(" \t\"");
                if (firstNonSpace != std::string::npos)
                {
                    std::string prefix = valuePart.substr(0, firstNonSpace);
                    std::string suffix = "";
                    size_t lastNonSpace = valuePart.find_last_not_of(" \t\"");
                    if (lastNonSpace != std::string::npos && lastNonSpace < valuePart.size() - 1)
                    {
                        suffix = valuePart.substr(lastNonSpace + 1);
                    }
                    processedLine = key + " = " + prefix + newValueStr + suffix;
                }
                else
                {
                    processedLine = key + " = " + newValueStr;
                }
            }
        }

        outputFile << processedLine << "\n";
    }

    outputFile.close();
    LOG_INFO("Preset salvo: " + presetPath);
    return true;
}
