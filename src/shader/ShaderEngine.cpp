#include "ShaderEngine.h"
#include "ShaderPreprocessor.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include "../renderer/glad_loader.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <set>
#include <vector>
#include <png.h>
#include <cstdlib>
#include <cstdio>

ShaderEngine::ShaderEngine()
{
}

ShaderEngine::~ShaderEngine()
{
    shutdown();
}

bool ShaderEngine::init()
{
    if (m_initialized)
    {
        return true;
    }

    // Carregar funções OpenGL antes de usar
    if (!loadOpenGLFunctions())
    {
        LOG_ERROR("Failed to load OpenGL functions in ShaderEngine");
        return false;
    }

    // Limites de resolução de shader são configuráveis pelo usuário via setMaxShaderResolution()
    // Não há valores padrão hardcoded - o usuário define conforme necessário

    createQuad();
    m_initialized = true;
    LOG_INFO("ShaderEngine initialized");
    return true;
}

void ShaderEngine::setMaxShaderResolution(uint32_t maxWidth, uint32_t maxHeight)
{
    m_maxShaderWidth = maxWidth;
    m_maxShaderHeight = maxHeight;
    if (maxWidth > 0 && maxHeight > 0)
    {
        LOG_INFO("Shader resolution limit set: " + 
                 std::to_string(maxWidth) + "x" + std::to_string(maxHeight));
    }
    else
    {
        LOG_INFO("Shader resolution limit disabled");
    }
}

void ShaderEngine::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    disableShader();
    cleanupPresetPasses();
    cleanupTextureReferences();
    cleanupQuad();
    
    // Limpar framebuffer temporário reutilizável
    if (m_copyFramebuffer != 0)
    {
        glDeleteFramebuffers(1, &m_copyFramebuffer);
        m_copyFramebuffer = 0;
    }

    m_initialized = false;
    LOG_INFO("ShaderEngine shutdown");
}

std::string ShaderEngine::generateDefaultVertexShader()
{
    // Vertex shader compatível com RetroArch
    // Usar versão GLSL dinâmica baseada na versão OpenGL disponível
    std::string version = getGLSLVersionString();
    bool isES = isOpenGLES();
    int major = getOpenGLMajorVersion();
    
    // Verificar se é OpenGL ES - nunca usar "core" em ES
    if (isES && major >= 3) {
        // OpenGL ES 3.0+ - usar in/out (sem "core")
        return version + "\n" +
               "in vec4 Position;\n"
               "in vec2 TexCoord;\n"
               "\n"
               "out vec2 vTexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = Position;\n"
               "    vTexCoord = TexCoord;\n"
               "}\n";
    } else if (isES) {
        // OpenGL ES 2.0 - usar attribute/varying
        return version + "\n" +
               "precision mediump float;\n"
               "attribute vec4 Position;\n"
               "attribute vec2 TexCoord;\n"
               "\n"
               "varying vec2 vTexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = Position;\n"
               "    vTexCoord = TexCoord;\n"
               "}\n";
    } else if (major >= 3) {
        // OpenGL 3.0+ Desktop - usar in/out com "core"
        return version + " core\n"
               "in vec4 Position;\n"
               "in vec2 TexCoord;\n"
               "\n"
               "out vec2 vTexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = Position;\n"
               "    vTexCoord = TexCoord;\n"
               "}\n";
    } else {
        // OpenGL 2.1 Desktop - usar attribute/varying, sem "core"
        return version + "\n" +
               "attribute vec4 Position;\n"
               "attribute vec2 TexCoord;\n"
               "\n"
               "varying vec2 vTexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = Position;\n"
               "    vTexCoord = TexCoord;\n"
               "}\n";
    }
}

bool ShaderEngine::loadShader(const std::string &shaderPath)
{
    if (!m_initialized)
    {
        LOG_ERROR("ShaderEngine not initialized");
        return false;
    }

    disableShader();

    // Verificar extensão - apenas GLSL é suportado
    fs::path shaderFilePath(shaderPath);
    std::string extension = shaderFilePath.extension();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".slang")
    {
        LOG_ERROR("Slang shaders (.slang) are not supported. Use GLSL shaders (.glsl) or GLSLP presets (.glslp)");
        LOG_ERROR("Many RetroArch shaders are available in GLSL format in the shaders/shaders_glsl/ folder");
        return false;
    }

    if (extension != ".glsl")
    {
        LOG_WARN("Unrecognized file extension: " + extension + ". Expected .glsl");
    }

    std::ifstream file(shaderPath);
    if (!file.is_open())
    {
        LOG_ERROR("Failed to open shader: " + shaderPath);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fragmentSource = buffer.str();
    file.close();

    // Extrair diretório base do shader para resolver includes
    std::string shaderDir = shaderFilePath.parent_path().string();

    // Processar apenas includes (GLSL já está no formato correto)
    fragmentSource = ShaderPreprocessor::processIncludes(fragmentSource, shaderDir);

    std::string vertexSource = generateDefaultVertexShader();

    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;

    if (!compileShader(vertexSource, GL_VERTEX_SHADER, vertexShader))
    {
        return false;
    }

    if (!compileShader(fragmentSource, GL_FRAGMENT_SHADER, fragmentShader))
    {
        glDeleteShader(vertexShader);
        return false;
    }

    disableShader();

    if (!linkProgram(vertexShader, fragmentShader))
    {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    m_vertexShader = vertexShader;
    m_fragmentShader = fragmentShader;
    m_shaderActive = true;
    m_uniformLocations.clear();

    LOG_INFO("Shader carregado: " + shaderPath);
    return true;
}

bool ShaderEngine::loadPreset(const std::string &presetPath)
{
    if (!m_initialized)
    {
        LOG_ERROR("ShaderEngine not initialized");
        return false;
    }

    disableShader();
    cleanupPresetPasses();

    // Verificar extensão - apenas GLSLP é suportado
    fs::path presetFilePath(presetPath);
    std::string extension = presetFilePath.extension();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".slangp")
    {
        LOG_ERROR("Slang presets (.slangp) are not supported. Use GLSLP presets (.glslp)");
        LOG_ERROR("Many RetroArch presets are available in GLSLP format in the shaders/shaders_glsl/ folder");
        return false;
    }

    if (extension != ".glslp" && !extension.empty())
    {
        LOG_WARN("Unrecognized preset extension: " + extension + ". Expected .glslp");
    }

    m_customParameters.clear(); // Limpar parâmetros customizados ao carregar novo preset

    if (!m_preset.load(presetPath))
    {
        return false;
    }

    m_presetPath = presetPath; // Armazenar caminho do preset

    // Carregar texturas de referência
    const auto &textures = m_preset.getTextures();
    for (const auto &tex : textures)
    {
        if (!loadTextureReference(tex.first, tex.second.path))
        {
            LOG_ERROR("Failed to load reference texture: " + tex.first);
        }
    }

    // IMPORTANTE: Resetar estado do OpenGL antes de carregar novo preset
    // Isso garante que se o preset anterior falhou, o estado não fique corrompido
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);

    // Carregar passes imediatamente para que os parâmetros estejam disponíveis na UI
    // IMPORTANTE: Tentar carregar passes mesmo que falhe parcialmente, para extrair parâmetros
    // Mesmo que a compilação falhe, os parâmetros podem ser extraídos do código fonte
    bool passesLoaded = loadPresetPasses();

    // Verificar se os parâmetros foram extraídos (mesmo se a compilação falhou)
    size_t totalParams = 0;
    for (const auto &passData : m_passes)
    {
        totalParams += passData.parameterInfo.size();
    }

    if (passesLoaded)
    {
        LOG_INFO("Preset loaded with " + std::to_string(m_passes.size()) + " pass(es) and " + std::to_string(totalParams) + " parameter(s)");
    }
    else
    {
        if (totalParams > 0)
        {
            LOG_WARN("Preset loaded but shader compilation failed. " + std::to_string(totalParams) + " parameter(s) extracted but shader will not work until compilation is fixed.");
            // IMPORTANTE: NÃO limpar passes se temos parâmetros extraídos!
            // Manter os passes com parameterInfo para que a UI possa mostrar os controles
            // Apenas os recursos OpenGL (shaders, programs) não estarão disponíveis
            LOG_INFO("Passes mantidos com parameterInfo para UI (m_passes.size() = " + std::to_string(m_passes.size()) + ")");
        }
        else
        {
            LOG_WARN("Preset loaded but failed to load passes. Passes will be loaded when shader is applied.");
            // Limpar passes apenas se não temos parâmetros extraídos
            cleanupPresetPasses();
        }
    }

    m_shaderActive = true;
    LOG_INFO("Preset carregado: " + presetPath + " (m_shaderActive = true, m_passes.size() = " + std::to_string(m_passes.size()) + ")");
    return true;
}

bool ShaderEngine::compilePass(size_t i)
{
    const auto &passes = m_preset.getPasses();
    const auto &passInfo = passes[i];
    auto &passData = m_passes[i];

    // Preservar parameterInfo existente se já temos (backup local)
    std::map<std::string, ShaderParameterInfo> localPreservedParamInfo = passData.parameterInfo;
    std::map<std::string, float> localPreservedExtractedParams = passData.extractedParameters;

    passData.passInfo = passInfo;

    // DEBUG: Log das configurações do pass

    // Ler shader
    std::ifstream file(passInfo.shaderPath);
    if (!file.is_open())
    {
        LOG_ERROR("Failed to open shader for pass " + std::to_string(i) + ": " + passInfo.shaderPath);
        // Se não conseguimos ler o arquivo, manter parâmetros existentes se houver
        // Apenas marcar que este pass não compilou
        passData.vertexShader = 0;
        passData.fragmentShader = 0;
        passData.program = 0;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string shaderSource = buffer.str();
    file.close();

    // Verificar extensão do shader - apenas GLSL é suportado
    fs::path shaderPath(passInfo.shaderPath);
    std::string shaderExtension = shaderPath.extension();
    std::transform(shaderExtension.begin(), shaderExtension.end(), shaderExtension.begin(), ::tolower);

    if (shaderExtension == ".slang")
    {
        LOG_ERROR("Slang shaders (.slang) are not supported in pass " + std::to_string(i));
        LOG_ERROR("Use GLSL shaders (.glsl) or GLSLP presets (.glslp)");
        // Slang não suportado, manter parâmetros existentes se houver
        passData.vertexShader = 0;
        passData.fragmentShader = 0;
        passData.program = 0;
        return false;
    }

    // Usar ShaderPreprocessor para processar o shader
    // Nota: As dimensões de saída/entrada serão calculadas em applyShader,
    // mas precisamos de valores padrão para o pré-processamento (código de compatibilidade)
    // Usar valores padrão razoáveis (640x480) - o código de compatibilidade só é injetado
    // para shaders específicos que não dependem fortemente das dimensões exatas
    uint32_t defaultInputWidth = 640;
    uint32_t defaultInputHeight = 480;
    uint32_t defaultOutputWidth = 640;
    uint32_t defaultOutputHeight = 480;

    auto preprocessResult = ShaderPreprocessor::preprocess(
        shaderSource,
        passInfo.shaderPath,
        i,
        defaultOutputWidth,
        defaultOutputHeight,
        defaultInputWidth,
        defaultInputHeight,
        m_preset.getPasses());

    // Armazenar resultados
    std::string vertexSource = preprocessResult.vertexSource;
    std::string fragmentSource = preprocessResult.fragmentSource;

    // Se já temos parameterInfo preservado e o novo está vazio, manter o preservado
    // Caso contrário, usar o novo (que pode ter mais parâmetros)
    if (!preprocessResult.parameterInfo.empty())
    {
        passData.extractedParameters = preprocessResult.extractedParameters;
        passData.parameterInfo = preprocessResult.parameterInfo;
    }
    else if (!localPreservedParamInfo.empty())
    {
        // Manter os parâmetros preservados se o novo está vazio
        passData.parameterInfo = localPreservedParamInfo;
        passData.extractedParameters = localPreservedExtractedParams;
    }

    // Log de debug: verificar se parâmetros foram extraídos
    if (!passData.parameterInfo.empty())
    {
        LOG_INFO("Pass " + std::to_string(i) + " has " + std::to_string(passData.parameterInfo.size()) + " parameter(s)");
    }

    // IMPORTANTE: Os parâmetros já foram extraídos e armazenados em passData.parameterInfo
    // Mesmo que a compilação falhe, queremos manter os parâmetros para a UI

    // Compilar shaders
    if (!compileShader(vertexSource, GL_VERTEX_SHADER, passData.vertexShader))
    {
        LOG_ERROR("Failed to compile vertex shader for pass " + std::to_string(i) + " (" + passInfo.shaderPath + ")");
        // Não limpar passes aqui - manter parameterInfo mesmo se compilação falhar
        // Apenas marcar que este pass não está compilado
        passData.vertexShader = 0;
        passData.fragmentShader = 0;
        passData.program = 0;
        // Continuar para o próximo pass para tentar extrair parâmetros de outros passes
        return false;
    }

    // Tentar compilar o fragment shader
    GLuint tempFragmentShader = 0;
    if (!compileShader(fragmentSource, GL_FRAGMENT_SHADER, tempFragmentShader))
    {
        // Se falhou, obter a mensagem de erro do shader temporário
        char errorLog[512] = {0};
        if (tempFragmentShader != 0)
        {
            glGetShaderInfoLog(tempFragmentShader, 512, nullptr, errorLog);
        }
        std::string errorMsg = std::string(errorLog);

        // Verificar se o erro é sobre vec3 = vec4
        bool isVec3Vec4Error = (errorMsg.find("initializer of type vec4 cannot be assigned to variable of type vec3") != std::string::npos ||
                                errorMsg.find("cannot convert") != std::string::npos ||
                                (errorMsg.find("vec4") != std::string::npos && errorMsg.find("vec3") != std::string::npos));

        if (isVec3Vec4Error)
        {
        }

        // Tentar corrigir erro específico de vec3 = vec4
        // Procurar por padrão: vec3 var = COMPAT_TEXTURE(...)
        std::string correctedSource = fragmentSource;
        std::regex vec3TextureError(R"(\bvec3\s+(\w+)\s*=\s*(COMPAT_TEXTURE|texture|texture2D)\s*\()");
        std::smatch match;

        if (std::regex_search(fragmentSource, match, vec3TextureError))
        {
            std::string varName = match[1].str();

            // Verificar se a variável é usada com .rgb ou similar depois
            // Procurar por padrões como: var.rgb, var.r, var.g, var.b, etc.
            // (comentado - não usado atualmente)
            // std::regex rgbPattern(varName + R"(\s*\.(rgb|r|g|b|rg|rb|gb))");
            // bool usesRgb = std::regex_search(fragmentSource, rgbPattern);

            // Mudar declaração para vec4 (sempre, já que COMPAT_TEXTURE retorna vec4)
            // IMPORTANTE: Precisamos preservar COMPAT_TEXTURE na substituição
            // Tentar substituição simples primeiro (mais confiável)
            std::string oldPattern = "vec3 " + varName + " = COMPAT_TEXTURE";
            std::string newPattern = "vec4 " + varName + " = COMPAT_TEXTURE";
            size_t pos = correctedSource.find(oldPattern);
            if (pos != std::string::npos)
            {
                correctedSource.replace(pos, oldPattern.length(), newPattern);
            }
            else
            {
                // Tentar com espaços diferentes
                oldPattern = "vec3\t" + varName + " = COMPAT_TEXTURE";
                newPattern = "vec4\t" + varName + " = COMPAT_TEXTURE";
                pos = correctedSource.find(oldPattern);
                if (pos != std::string::npos)
                {
                    correctedSource.replace(pos, oldPattern.length(), newPattern);
                }
                else
                {
                    // Tentar com regex (última opção)
                    std::string pattern = R"(\bvec3\s+)" + varName + R"(\s*=\s*(COMPAT_TEXTURE|texture|texture2D)\s*\()";
                    std::regex regexPattern(pattern);
                    std::smatch match;

                    if (std::regex_search(correctedSource, match, regexPattern))
                    {
                        // Capturar a função de textura e construir a substituição manualmente
                        std::string textureFunc = match[2].str(); // COMPAT_TEXTURE, texture ou texture2D
                        std::string fullMatch = match[0].str();   // Match completo
                        std::string replacement = "vec4 " + varName + " = " + textureFunc + "(";

                        // Substituir manualmente para garantir que funcione
                        size_t matchPos = correctedSource.find(fullMatch);
                        if (matchPos != std::string::npos)
                        {
                            correctedSource.replace(matchPos, fullMatch.length(), replacement);
                        }
                        else
                        {
                            // Fallback: usar regex_replace
                            correctedSource = std::regex_replace(correctedSource, regexPattern, replacement);
                        }
                    }
                }
            }

            // Verificar se a substituição foi feita (verificar de várias formas)
            bool substitutionFound = false;
            if (correctedSource.find("vec4 " + varName) != std::string::npos ||
                correctedSource.find("vec4\t" + varName) != std::string::npos ||
                correctedSource.find("vec4  " + varName) != std::string::npos) // Dois espaços
            {
                substitutionFound = true;
            }
            else
            {
                // Verificar se ainda tem vec3 (substituição falhou)
                if (correctedSource.find("vec3 " + varName) != std::string::npos ||
                    correctedSource.find("vec3\t" + varName) != std::string::npos)
                {
                    LOG_ERROR("Pass " + std::to_string(i) + ": Substitution failed - still found vec3 " + varName);
                    // Tentar substituição manual linha por linha
                    std::istringstream iss(correctedSource);
                    std::ostringstream oss;
                    std::string line;
                    bool found = false;
                    while (std::getline(iss, line))
                    {
                        // Procurar por qualquer linha que contenha vec3, o nome da variável e COMPAT_TEXTURE
                        if (line.find("vec3") != std::string::npos &&
                            line.find(varName) != std::string::npos &&
                            line.find("COMPAT_TEXTURE") != std::string::npos)
                        {
                            // Substituir vec3 por vec4
                            size_t pos = line.find("vec3");
                            if (pos != std::string::npos)
                            {
                                line.replace(pos, 4, "vec4");
                                found = true;
                            }
                        }
                        oss << line << "\n";
                    }
                    if (found)
                    {
                        correctedSource = oss.str();
                        substitutionFound = true;
                    }
                    else
                    {
                        // Última tentativa: substituição direta no string
                        size_t pos = correctedSource.find("vec3 " + varName + " = COMPAT_TEXTURE");
                        if (pos != std::string::npos)
                        {
                            correctedSource.replace(pos, 4, "vec4");
                            substitutionFound = true;
                        }
                        else
                        {
                            // Tentar com espaços diferentes
                            pos = correctedSource.find("vec3\t" + varName + " = COMPAT_TEXTURE");
                            if (pos != std::string::npos)
                            {
                                correctedSource.replace(pos, 4, "vec4");
                                substitutionFound = true;
                            }
                        }
                    }
                }
            }

            if (substitutionFound)
            {
                // CORREÇÃO ADICIONAL: Se mudamos vec3 para vec4, precisamos corrigir usos de vec4(var, float)
                // Padrão: vec4(varName, float) onde varName agora é vec4
                // Isso deve ser corrigido para vec4(varName.rgb, float) ou apenas varName
                std::regex vec4ConstructorPattern(R"(\bvec4\s*\(\s*)" + varName + R"(\s*,\s*[\d.]+\s*\))");
                if (std::regex_search(correctedSource, vec4ConstructorPattern))
                {
                    // Substituir vec4(varName, float) por vec4(varName.rgb, float)
                    correctedSource = std::regex_replace(correctedSource,
                                                         std::regex(R"(\bvec4\s*\(\s*)" + varName + R"(\s*,\s*([\d.]+)\s*\))"),
                                                         "vec4(" + varName + ".rgb, $1)");
                }

                // Log da linha corrigida para debug
                std::istringstream iss(correctedSource);
                std::string line;
                int lineNum = 0;
                while (std::getline(iss, line) && lineNum < 110)
                {
                    lineNum++;
                    if (lineNum >= 98 && lineNum <= 110 && (line.find(varName) != std::string::npos || line.find("FragColor") != std::string::npos))
                    {
                    }
                }

                // Tentar compilar novamente
                GLuint testShader = 0;
                if (compileShader(correctedSource, GL_FRAGMENT_SHADER, testShader))
                {
                    // Sucesso! Usar a versão corrigida
                    if (passData.fragmentShader != 0)
                        glDeleteShader(passData.fragmentShader);
                    passData.fragmentShader = testShader;
                    fragmentSource = correctedSource;
                }
                else
                {
                    // Ainda falhou, mostrar erro detalhado
                    char infoLog[512];
                    glGetShaderInfoLog(testShader, 512, nullptr, infoLog);
                    LOG_ERROR("Failed to compile fragment shader for pass " + std::to_string(i) + " (" + passInfo.shaderPath + ") even after correction: " + std::string(infoLog));
                    std::istringstream iss2(correctedSource);
                    std::string line2;
                    int lineNum2 = 0;
                    while (std::getline(iss2, line2) && lineNum2 < 110)
                    {
                        lineNum2++;
                        if (lineNum2 >= 95 && lineNum2 <= 110)
                        {
                        }
                    }
                    if (testShader != 0)
                        glDeleteShader(testShader);
                    glDeleteShader(passData.vertexShader);
                    if (tempFragmentShader != 0)
                        glDeleteShader(tempFragmentShader);
                    // Não limpar tudo - manter parameterInfo para a UI
                    passData.vertexShader = 0;
                    passData.fragmentShader = 0;
                    passData.program = 0;
                    return false;
                }
            }
            else
            {
                LOG_ERROR("Pass " + std::to_string(i) + ": Failed to apply correction (substitution not found)");
                std::istringstream iss(fragmentSource);
                std::string line;
                int lineNum = 0;
                while (std::getline(iss, line) && lineNum < 105)
                {
                    lineNum++;
                    if (lineNum >= 95 && lineNum <= 105)
                    {
                    }
                }
                glDeleteShader(passData.vertexShader);
                if (tempFragmentShader != 0)
                    glDeleteShader(tempFragmentShader);
                // Manter parameterInfo mesmo se compilação falhar
                passData.vertexShader = 0;
                passData.fragmentShader = 0;
                passData.program = 0;
                return false;
            }
        }
        else
        {
            // Erro não relacionado a vec3 = vec4 ou padrão não encontrado
            LOG_ERROR("Failed to compile fragment shader for pass " + std::to_string(i) + " (" + passInfo.shaderPath + ")");
            // Manter parameterInfo mesmo se compilação falhar
            glDeleteShader(passData.vertexShader);
            if (tempFragmentShader != 0)
                glDeleteShader(tempFragmentShader);
            passData.vertexShader = 0;
            passData.fragmentShader = 0;
            passData.program = 0;
            return false;
        }
    }
    else
    {
        // Compilação bem-sucedida
        passData.fragmentShader = tempFragmentShader;
    }

    // Criar e linkar programa para este pass (cada pass precisa de seu próprio programa)
    GLuint program = glCreateProgram();
    if (program == 0)
    {
        LOG_ERROR("Failed to create shader program for pass " + std::to_string(i));
        // Manter parameterInfo mesmo se criação do programa falhar
        glDeleteShader(passData.vertexShader);
        if (passData.fragmentShader != 0)
            glDeleteShader(passData.fragmentShader);
        passData.vertexShader = 0;
        passData.fragmentShader = 0;
        passData.program = 0;
        return false;
    }

    glAttachShader(program, passData.vertexShader);
    glAttachShader(program, passData.fragmentShader);

    // Ligar atributos antes de linkar (necessário quando não usamos layout(location))
    // RetroArch shaders podem usar VertexCoord ou Position
    glBindAttribLocation(program, 0, "Position");
    glBindAttribLocation(program, 0, "VertexCoord"); // RetroArch também usa VertexCoord
    glBindAttribLocation(program, 1, "TexCoord");
    // IMPORTANTE: Motion blur shaders precisam de PrevTexCoord, Prev1TexCoord, etc.
    // Como todos os frames têm as mesmas dimensões, podemos usar os mesmos dados de TexCoord
    glBindAttribLocation(program, 1, "PrevTexCoord");
    glBindAttribLocation(program, 1, "Prev1TexCoord");
    glBindAttribLocation(program, 1, "Prev2TexCoord");
    glBindAttribLocation(program, 1, "Prev3TexCoord");
    glBindAttribLocation(program, 1, "Prev4TexCoord");
    glBindAttribLocation(program, 1, "Prev5TexCoord");
    glBindAttribLocation(program, 1, "Prev6TexCoord");
    glBindAttribLocation(program, 2, "COLOR"); // Alguns shaders RetroArch usam COLOR como atributo

    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        LOG_ERROR("Error linking shader program for pass " + std::to_string(i) + " (" + passInfo.shaderPath + "): " + std::string(infoLog));
        glDeleteProgram(program);
        glDeleteShader(passData.vertexShader);
        glDeleteShader(passData.fragmentShader);
        glDeleteProgram(program);
        // Manter parameterInfo mesmo se linkagem falhar
        passData.vertexShader = 0;
        passData.fragmentShader = 0;
        passData.program = 0;
        return false;
    }

    passData.program = program;
    passData.floatFramebuffer = passInfo.floatFramebuffer;
    
    // Pré-cachear uniforms comuns para este pass após linkagem bem-sucedida
    preCacheCommonUniforms(program);
    return true;
}


bool ShaderEngine::loadPresetPasses()
{
    // IMPORTANTE: Preservar parameterInfo existente ao recarregar passes
    // Salvar parameterInfo de passes existentes antes de limpar
    std::vector<std::map<std::string, ShaderParameterInfo>> preservedParamInfo;
    std::vector<std::map<std::string, float>> preservedExtractedParams;

    if (!m_passes.empty() && m_passes.size() == m_preset.getPasses().size())
    {
        // Se já temos passes com o mesmo número, preservar parameterInfo
        for (const auto &passData : m_passes)
        {
            preservedParamInfo.push_back(passData.parameterInfo);
            preservedExtractedParams.push_back(passData.extractedParameters);
        }
    }

    // Limpar recursos OpenGL mas preservar estrutura se temos parameterInfo
    bool hasPreservedParams = false;
    for (size_t i = 0; i < preservedParamInfo.size(); ++i)
    {
        if (!preservedParamInfo[i].empty())
        {
            hasPreservedParams = true;
            break;
        }
    }

    if (!hasPreservedParams)
    {
        // Se não temos parâmetros preservados, limpar tudo
        cleanupPresetPasses();
    }
    else
    {
        // Limpar apenas recursos OpenGL, preservando parameterInfo
        for (auto &pass : m_passes)
        {
            if (pass.program != 0)
            {
                glDeleteProgram(pass.program);
                pass.program = 0;
            }
            if (pass.vertexShader != 0)
            {
                glDeleteShader(pass.vertexShader);
                pass.vertexShader = 0;
            }
            if (pass.fragmentShader != 0)
            {
                glDeleteShader(pass.fragmentShader);
                pass.fragmentShader = 0;
            }
            cleanupFramebuffer(pass.framebuffer, pass.texture);
            cleanupFramebuffer(pass.feedbackFramebuffer, pass.feedbackTexture);
            pass.feedbackEnabled = false;
        }
    }

    const auto &passes = m_preset.getPasses();
    // Redimensionar apenas se necessário
    if (m_passes.size() != passes.size())
    {
        m_passes.resize(passes.size());
    }

    // Restaurar parameterInfo preservado se não foi limpo
    if (hasPreservedParams && m_passes.size() == preservedParamInfo.size())
    {
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            if (!preservedParamInfo[i].empty())
            {
                m_passes[i].parameterInfo = preservedParamInfo[i];
                m_passes[i].extractedParameters = preservedExtractedParams[i];
            }
        }
    }

    bool allPassesCompiled = true; // Rastrear se todos os passes compilaram com sucesso

    for (size_t i = 0; i < passes.size(); ++i)
    {
        if (!compilePass(i))
            allPassesCompiled = false;
    }

    // Verificar se há texturas de referência
    const auto &textures = m_preset.getTextures();
    if (!textures.empty())
    {
        // Textures disponíveis mas não processadas aqui
        (void)textures;
    }

    // Retornar true mesmo se alguns passes falharam na compilação
    // Os parâmetros extraídos estarão disponíveis na UI
    return allPassesCompiled;
}

void ShaderEngine::renderMultipassPass(size_t i, GLuint &currentTexture, uint32_t &currentWidth,
                                       uint32_t &currentHeight, GLuint originalTexture)
{
    auto &pass = m_passes[i];
    const auto &passInfo = pass.passInfo;

    // Calcular dimensões de saída
    // Para absolute, precisamos ler o valor do preset
    uint32_t absX = 0, absY = 0;
    if (passInfo.scaleTypeX == "absolute")
    {
        absX = static_cast<uint32_t>(std::round(passInfo.scaleX));
    }
    if (passInfo.scaleTypeY == "absolute")
    {
        absY = static_cast<uint32_t>(std::round(passInfo.scaleY));
    }

    // IMPORTANTE: Se for o último pass e não especificar escala, usar viewport
    // Isso garante que a textura final preencha a janela
    // MAS: Se o primeiro pass especificar viewport explicitamente, não alterar
    bool isLastPass = (i == m_passes.size() - 1);
    std::string scaleTypeX = passInfo.scaleTypeX;
    std::string scaleTypeY = passInfo.scaleTypeY;
    float scaleX = passInfo.scaleX;
    float scaleY = passInfo.scaleY;

    // Se for o último pass e não especificar escala (ou for "source" com scale 1.0),
    // usar viewport para preencher a janela
    // IMPORTANTE: Não alterar se já especificar viewport explicitamente
    if (isLastPass && scaleTypeX != "viewport" && (scaleTypeX.empty() || (scaleTypeX == "source" && scaleX == 1.0f)))
    {
        scaleTypeX = "viewport";
        scaleX = 1.0f;
    }
    if (isLastPass && scaleTypeY != "viewport" && (scaleTypeY.empty() || (scaleTypeY == "source" && scaleY == 1.0f)))
    {
        scaleTypeY = "viewport";
        scaleY = 1.0f;
    }

    uint32_t outputWidth = calculateScale(currentWidth, scaleTypeX, scaleX,
                                          m_viewportWidth, absX);
    uint32_t outputHeight = calculateScale(currentHeight, scaleTypeY, scaleY,
                                           m_viewportHeight, absY);

    // Aplicar limite de resolução apenas se configurado pelo usuário
    if (m_maxShaderWidth > 0 && outputWidth > m_maxShaderWidth)
    {
        float aspectRatio = static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
        outputWidth = m_maxShaderWidth;
        outputHeight = static_cast<uint32_t>(std::round(m_maxShaderWidth / aspectRatio));
        outputHeight = (outputHeight / 2) * 2; // Múltiplo de 2
    }
    if (m_maxShaderHeight > 0 && outputHeight > m_maxShaderHeight)
    {
        float aspectRatio = static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
        outputHeight = m_maxShaderHeight;
        outputWidth = static_cast<uint32_t>(std::round(m_maxShaderHeight * aspectRatio));
        outputWidth = (outputWidth / 2) * 2; // Múltiplo de 2
    }

    // DEBUG: Log das dimensões calculadas
    if (i == 0 || i == m_passes.size() - 1)
    {
    }

    // Criar/atualizar framebuffer se necessário
    if (pass.framebuffer == 0 || pass.width != outputWidth || pass.height != outputHeight)
    {
        cleanupFramebuffer(pass.framebuffer, pass.texture);
        createFramebuffer(outputWidth, outputHeight, pass.passInfo.floatFramebuffer,
                          pass.framebuffer, pass.texture, pass.passInfo.srgbFramebuffer);
        // Dimensões mudaram: invalidar feedback FBO; será re-alocada lazy.
        cleanupFramebuffer(pass.feedbackFramebuffer, pass.feedbackTexture);
        pass.feedbackEnabled = false;
        pass.width = outputWidth;
        pass.height = outputHeight;

        // DEBUG: Log do primeiro pass
        if (i == 0)
        {
        }
    }

    // Bind framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, pass.framebuffer);

    // Para passes com srgb_framebuffer=true, o FBO usa GL_SRGB8_ALPHA8.
    // Sem GL_FRAMEBUFFER_SRGB habilitado, o valor escrito pelo shader cai
    // direto na textura sem encoding sRGB; depois, o pass seguinte ao ler
    // a textura aplica decoding sRGB→linear automaticamente, gerando
    // gama dupla a cada pass e progressivamente escurecendo até preto.
    // GL_FRAMEBUFFER_SRGB = 0x8DB9 (não exposto pelo nosso loader glad)
    constexpr GLenum FRAMEBUFFER_SRGB_ENUM = 0x8DB9;
    if (pass.passInfo.srgbFramebuffer)
    {
        glEnable(FRAMEBUFFER_SRGB_ENUM);
    }
    else
    {
        glDisable(FRAMEBUFFER_SRGB_ENUM);
    }

    glViewport(0, 0, outputWidth, outputHeight);

    // IMPORTANTE: Limpar com cor transparente (0,0,0,0) para shaders que usam alpha
    // O shader gameboy usa alpha, então precisamos de um fundo transparente
    // IMPORTANTE: Habilitar color mask antes de limpar (como RetroArch faz)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // IMPORTANTE: Para shaders que usam alpha (como Game Boy), NÃO habilitar blending durante a renderização
    // O blending é necessário apenas na renderização final na janela, não durante a renderização para o framebuffer
    // Durante a renderização para o framebuffer, queremos que o shader escreva diretamente o alpha que ele calcula
    // O blending será aplicado depois quando renderizarmos a textura do framebuffer na janela
    // IMPORTANTE: Verificar se o programa é válido ANTES de usar
    if (pass.program == 0)
    {
        LOG_ERROR("Invalid shader program in pass " + std::to_string(i));
        // IMPORTANTE: Resetar estado do OpenGL antes de continuar
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // IMPORTANTE: Desabilitar blending, culling e depth test (como RetroArch faz)
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    // Usar shader program (agora sabemos que é válido)
    glUseProgram(pass.program);

    // IMPORTANTE: Configurar uniforms ANTES de bind de texturas
    // Mas o uniform Texture/Source precisa ser configurado DEPOIS do bind
    // Primeiro, configurar outros uniforms (SourceSize, etc)
    setupUniforms(pass.program, i, currentWidth, currentHeight, outputWidth, outputHeight);

    // Bind textura de entrada na unidade 0 (ANTES de configurar uniform Texture)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    // Verificar se a textura é válida ANTES de configurar uniforms
    if (currentTexture == 0)
    {
        LOG_ERROR("Invalid input texture in pass " + std::to_string(i));
        // IMPORTANTE: Resetar estado do OpenGL antes de continuar
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // IMPORTANTE: Aplicar filter_linear# e wrap_mode# na textura de entrada
    // RetroArch aplica essas configurações quando faz bind da textura
    // Isso é crítico para shaders como motionblur-simple e crt-geom que precisam de GL_NEAREST
    // A textura já está bindada (glBindTexture acima), então apenas aplicar parâmetros
    bool filterLinear = passInfo.filterLinear;
    std::string wrapMode = passInfo.wrapMode;
    bool mipmapInput = passInfo.mipmapInput;

    // Aplicar filtro (GL_LINEAR ou GL_NEAREST)
    // IMPORTANTE: Aplicar sempre, pois alguns shaders (como crt-geom) precisam de GL_NEAREST no primeiro pass
    GLenum filter = filterLinear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // Se mipmap for necessário, usar filtros de mipmap
    if (mipmapInput)
    {
        if (filterLinear)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    // Aplicar wrap mode
    // IMPORTANTE: Usar clamp_to_edge como fallback se clamp_to_border não for suportado
    GLenum wrap = wrapModeToGLEnum(wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

    // DEBUG: Log do primeiro pass
    if (i == 0)
    {
    }

    // Configurar uniform Texture/Source DEPOIS do bind (como RetroArch faz)
    // RetroArch shaders podem usar diferentes nomes: Texture, Source, Input, s_p, etc.
    // Tentar todos os nomes comuns
    bool textureBound = false;

    // Lista de nomes comuns de uniforms de textura no RetroArch
    const char *textureUniformNames[] = {
        "Texture", // Mais comum
        "Source",  // Segundo mais comum
        "Input",   // Alguns shaders usam
        "s_p",     // Usado por alguns shaders (ex: sgenpt-mix)
        "tex",     // Alguns shaders usam
        "image",   // Raro, mas possível
    };

    for (const char *name : textureUniformNames)
    {
        GLint loc = getUniformLocation(pass.program, name);
        if (loc >= 0)
        {
            // IMPORTANTE: Garantir que a textura está bindada ANTES de configurar o uniform
            // E garantir que estamos na unidade de textura correta
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, currentTexture);
            glUniform1i(loc, 0);
            textureBound = true;
            if (i == 0 || i == 3 || i == 4)
            {
            }
            break; // Encontrou um, não precisa tentar os outros
        }
        else if (i == 0)
        {
            // Log apenas no primeiro pass para debug
            // LOG_INFO("Pass 0: Uniform '" + std::string(name) + "' não encontrado");
        }
    }

    // Se nenhum uniform foi encontrado, logar aviso (primeiro pass, pass 3 e pass 4 que estão falhando)
    // Nota: o warning sobre "nenhum uniform de textura encontrado" foi
    // movido pra depois das demais ligações (PrevTexture, PassPrev,
    // PassFeedback, alias, OrigTexture, LUT). Shaders como avg-lum0.glsl
    // declaram `Texture` mas só usam `PrevN`, fazendo o compilador
    // otimizar `Texture` fora do programa — não é tela preta de verdade.

    // Bind texturas de passes anteriores (PassPrev#Texture)
    // RetroArch shaders podem referenciar saídas de passes anteriores
    // Também suportar nomes alternativos: PrevTexture, Prev1Texture, etc.
    int texUnit = 1;

    // IMPORTANTE: Se não há passes anteriores (i == 0), mas o shader espera PrevTexture,
    // vincular a textura de entrada atual para esses uniforms (comportamento comum do RetroArch)
    // IMPORTANTE: Para motion blur, precisamos vincular frames anteriores reais
    // Verificar se o shader precisa de histórico de frames (PrevTexture, Prev1Texture, etc.)
    bool needsHistory = false;
    for (int prevIdx = 0; prevIdx < 7; ++prevIdx)
    {
        std::string name = (prevIdx == 0) ? "PrevTexture" : "Prev" + std::to_string(prevIdx) + "Texture";
        if (getUniformLocation(pass.program, name) >= 0)
        {
            needsHistory = true;
            break;
        }
    }

    if (i == 0)
    {
        // Para o primeiro pass, vincular histórico de frames se disponível
        // IMPORTANTE: Se não há histórico suficiente, NÃO vincular os PrevTexture uniforms
        // Isso evita que o shader faça média de frames idênticos (que escurece)
        // O shader motion blur deve funcionar apenas quando há histórico real
        for (int prevIdx = 0; prevIdx < 7; ++prevIdx) // RetroArch geralmente usa até Prev6Texture
        {
            std::vector<std::string> passTextureNames;
            if (prevIdx == 0)
            {
                passTextureNames.push_back("PrevTexture");
                passTextureNames.push_back("PassPrev0Texture");
            }
            else
            {
                passTextureNames.push_back("Prev" + std::to_string(prevIdx) + "Texture");
                passTextureNames.push_back("PassPrev" + std::to_string(prevIdx) + "Texture");
            }

            for (const auto &name : passTextureNames)
            {
                GLint loc = getUniformLocation(pass.program, name);
                if (loc >= 0)
                {
                    // IMPORTANTE: Só vincular se temos histórico real para este índice
                    // Se não há histórico suficiente, NÃO vincular o uniform
                    // Isso evita que o shader faça média de frames idênticos (que escurece)
                    // O shader motion blur deve funcionar apenas quando há histórico real
                    if (needsHistory && prevIdx < static_cast<int>(m_frameHistory.size()))
                    {
                        glActiveTexture(GL_TEXTURE0 + texUnit);

                        // Usar frame do histórico (frames mais antigos primeiro)
                        // PrevTexture = frame mais recente (índice 0), Prev6Texture = frame mais antigo
                        // O histórico é ordenado: [mais recente, ..., mais antigo]
                        size_t historyIdx = prevIdx; // prevIdx=0 é o mais recente, prevIdx=6 é o mais antigo
                        if (historyIdx < m_frameHistory.size() && m_frameHistory[historyIdx] != 0)
                        {
                            glBindTexture(GL_TEXTURE_2D, m_frameHistory[historyIdx]);
                            glUniform1i(loc, texUnit);
                            texUnit++;
                        }
                    }
                    // Se não há histórico suficiente, não vincular o uniform
                    // O shader pode usar valores padrão ou zero, mas não fará média de frames idênticos
                    // Isso evita o escurecimento quando não há histórico suficiente
                    break;
                }
            }
        }
    }
    else
    {
        // Para passes subsequentes, vincular texturas de passes anteriores reais
        for (uint32_t prevPass = 0; prevPass < i && prevPass < m_passes.size(); ++prevPass)
        {
            // Tentar diferentes nomes de uniforms
            std::vector<std::string> passTextureNames;
            if (prevPass == 0)
            {
                passTextureNames.push_back("PassPrev" + std::to_string(i - prevPass) + "Texture");
                passTextureNames.push_back("PrevTexture");
            }
            else
            {
                passTextureNames.push_back("PassPrev" + std::to_string(i - prevPass) + "Texture");
                passTextureNames.push_back("Prev" + std::to_string(prevPass) + "Texture");
            }

            for (const auto &name : passTextureNames)
            {
                GLint loc = getUniformLocation(pass.program, name);
                if (loc >= 0)
                {
                    glActiveTexture(GL_TEXTURE0 + texUnit);
                    glBindTexture(GL_TEXTURE_2D, m_passes[prevPass].texture);
                    glUniform1i(loc, texUnit);
                    texUnit++;
                    break;
                }
            }

            // PassPrev<N>TextureSize / PassPrev<N>InputSize / PassPrev<N>OutputSize:
            // dimensões do pass alvo. Sem isso, shaders como crt-royale-bloom-approx
            // (que faz tex_uv = video_uv * PassPrev2InputSize / PassPrev2TextureSize)
            // dividem 0/0 e produzem NaN → tela preta.
            const std::string nStr = std::to_string(i - prevPass);
            const ShaderPassData &target = m_passes[prevPass];
            const float tw = static_cast<float>(target.width);
            const float th = static_cast<float>(target.height);

            GLint sizeLoc = getUniformLocation(pass.program, "PassPrev" + nStr + "TextureSize");
            if (sizeLoc >= 0)
            {
                glUniform2f(sizeLoc, tw, th);
            }
            sizeLoc = getUniformLocation(pass.program, "PassPrev" + nStr + "InputSize");
            if (sizeLoc >= 0)
            {
                // InputSize de pass P é o tamanho que ele recebeu como entrada.
                // Se P>0, é o output do pass anterior; se P==0, é o source original.
                if (prevPass == 0)
                {
                    glUniform2f(sizeLoc,
                                static_cast<float>(m_sourceWidth),
                                static_cast<float>(m_sourceHeight));
                }
                else
                {
                    glUniform2f(sizeLoc,
                                static_cast<float>(m_passes[prevPass - 1].width),
                                static_cast<float>(m_passes[prevPass - 1].height));
                }
            }
            sizeLoc = getUniformLocation(pass.program, "PassPrev" + nStr + "OutputSize");
            if (sizeLoc >= 0)
            {
                glUniform2f(sizeLoc, tw, th);
            }
        }

        // PassPrev<N>Texture com N > pass atual aponta pra "antes do pass 0",
        // que na prática é o input original (kawase_glow's screen_combine usa
        // PassPrev8Texture no pass 7 pra recuperar a imagem pré-blur).
        // Tentamos uma faixa razoável acima do pass atual.
        for (uint32_t N = i + 1; N <= i + 12; ++N)
        {
            GLint loc = getUniformLocation(pass.program,
                                           "PassPrev" + std::to_string(N) + "Texture");
            if (loc >= 0 && originalTexture != 0)
            {
                glActiveTexture(GL_TEXTURE0 + texUnit);
                glBindTexture(GL_TEXTURE_2D, originalTexture);
                glUniform1i(loc, texUnit);
                texUnit++;
            }
        }

        // Vincular passes anteriores referenciados por alias (aliasN = SomeName).
        // RetroArch GLSL spec: presets podem nomear um pass via `aliasN = MyPass`,
        // e passes posteriores referenciam o sampler como `uniform sampler2D MyPass`
        // (e o tamanho via `uniform vec4 MyPassSize`).
        for (uint32_t prevPass = 0; prevPass < i && prevPass < m_passes.size(); ++prevPass)
        {
            const std::string &alias = m_passes[prevPass].passInfo.alias;
            if (alias.empty())
            {
                continue;
            }

            GLint aliasTexLoc = getUniformLocation(pass.program, alias);
            if (aliasTexLoc >= 0)
            {
                glActiveTexture(GL_TEXTURE0 + texUnit);
                glBindTexture(GL_TEXTURE_2D, m_passes[prevPass].texture);
                glUniform1i(aliasTexLoc, texUnit);
                texUnit++;
            }

            GLint aliasSizeLoc = getUniformLocation(pass.program, alias + "Size");
            if (aliasSizeLoc >= 0)
            {
                float w = static_cast<float>(m_passes[prevPass].width);
                float h = static_cast<float>(m_passes[prevPass].height);
                glUniform4f(aliasSizeLoc, w, h,
                            w > 0.0f ? 1.0f / w : 0.0f,
                            h > 0.0f ? 1.0f / h : 0.0f);
            }
        }
    }

    // PassFeedback<N> / PassFeedback<N>Texture: o pass corrente pode samplear o
    // output do frame ANTERIOR de qualquer pass (incluindo si mesmo). Alocação
    // lazy: se algum uniform PassFeedback<N>* aparecer aqui, marcamos pass[N]
    // pra ter ping-pong e alocamos sua segunda textura na primeira vez.
    // O swap entre texture/feedbackTexture acontece no fim do frame, abaixo.
    for (uint32_t fbPass = 0; fbPass <= i && fbPass < m_passes.size(); ++fbPass)
    {
        const std::string idxStr = std::to_string(fbPass);
        std::vector<std::string> samplerNames = {
            "PassFeedback" + idxStr,
            "PassFeedback" + idxStr + "Texture"
        };

        GLint fbTexLoc = -1;
        for (const auto &name : samplerNames)
        {
            fbTexLoc = getUniformLocation(pass.program, name);
            if (fbTexLoc >= 0) break;
        }

        bool hasFeedbackUniform = (fbTexLoc >= 0);

        GLint fbSizeLoc = getUniformLocation(pass.program, "PassFeedback" + idxStr + "Size");
        if (fbSizeLoc < 0)
        {
            fbSizeLoc = getUniformLocation(pass.program, "PassFeedback" + idxStr + "TextureSize");
        }
        if (fbSizeLoc >= 0)
        {
            hasFeedbackUniform = true;
        }

        if (!hasFeedbackUniform)
        {
            continue;
        }

        ShaderPassData &fbTarget = m_passes[fbPass];
        fbTarget.feedbackEnabled = true;

        // Lazy alloc da textura/FBO de feedback com as MESMAS dimensões do
        // pass alvo. Se as dimensões ainda não foram definidas (pass que não
        // rodou neste frame ainda), pular — nada útil pra bindar.
        if (fbTarget.feedbackTexture == 0 && fbTarget.width > 0 && fbTarget.height > 0)
        {
            createFramebuffer(fbTarget.width, fbTarget.height,
                              fbTarget.passInfo.floatFramebuffer,
                              fbTarget.feedbackFramebuffer, fbTarget.feedbackTexture,
                              fbTarget.passInfo.srgbFramebuffer);
        }

        if (fbTexLoc >= 0 && fbTarget.feedbackTexture != 0)
        {
            glActiveTexture(GL_TEXTURE0 + texUnit);
            glBindTexture(GL_TEXTURE_2D, fbTarget.feedbackTexture);
            glUniform1i(fbTexLoc, texUnit);
            texUnit++;
        }

        if (fbSizeLoc >= 0)
        {
            float w = static_cast<float>(fbTarget.width);
            float h = static_cast<float>(fbTarget.height);
            glUniform4f(fbSizeLoc, w, h,
                        w > 0.0f ? 1.0f / w : 0.0f,
                        h > 0.0f ? 1.0f / h : 0.0f);
        }
    }

    // IMPORTANTE: Vincular textura original (OrigTexture) se o shader precisar
    // Alguns shaders (como hqx-pass2) precisam da textura original além da saída do pass anterior
    GLint origTexLoc = getUniformLocation(pass.program, "OrigTexture");
    if (origTexLoc >= 0)
    {
        glActiveTexture(GL_TEXTURE0 + texUnit);
        glBindTexture(GL_TEXTURE_2D, originalTexture);
        glUniform1i(origTexLoc, texUnit);
        texUnit++;
    }

    // Bind texturas de referência (LUTs, etc)
    const auto &presetTextures = m_preset.getTextures();
    for (const auto &texRef : m_textureReferences)
    {
        glActiveTexture(GL_TEXTURE0 + texUnit);
        glBindTexture(GL_TEXTURE_2D, texRef.second);

        // Aplicar configurações do preset (filter_linear, wrap_mode, mipmap)
        bool filterLinear = true;               // Padrão: linear
        std::string wrapMode = "clamp_to_edge"; // Padrão: clamp_to_edge
        bool mipmap = false;                    // Padrão: sem mipmap

        auto texIt = presetTextures.find(texRef.first);
        if (texIt != presetTextures.end())
        {
            filterLinear = texIt->second.linear;
            wrapMode = texIt->second.wrapMode;
            mipmap = texIt->second.mipmap;
        }

        // Aplicar configurações (textura já está bindada)
        GLenum filter = filterLinear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

        if (mipmap)
        {
            if (filterLinear)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            }
            else
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            }
            glGenerateMipmap(GL_TEXTURE_2D);
        }

        GLenum wrap = wrapModeToGLEnum(wrapMode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

        GLint loc = getUniformLocation(pass.program, texRef.first);
        if (loc >= 0)
        {
            glUniform1i(loc, texUnit);
        }
        else
        {
            // Não logar aviso se a textura não for usada neste pass
            // Alguns shaders (como hqx-pass1) não usam todas as texturas de referência
            // Apenas logar em modo DEBUG se necessário
            // LOG_WARN("Pass " + std::to_string(i) + ": Uniform de textura de referência '" + texRef.first + "' não encontrado");
        }
        texUnit++;
    }

    // Após TODOS os bindings (Texture/Prev/PassPrev/PassFeedback/alias/Orig/LUT),
    // só avisamos "tela preta provável" se NADA tiver sido bindado neste pass.
    // texUnit começa em 1 (slot 0 reservado pra Texture). Se ainda for 1 e
    // textureBound=false, o pass vai amostrar de unidades de textura
    // não inicializadas — isso sim seria preto de verdade.
    if (!textureBound && texUnit == 1)
    {
        static int warnedPasses = 0;
        if (warnedPasses < 5)
        {
            warnedPasses++;
            LOG_WARN("Pass " + std::to_string(i) + " sem nenhum sampler bindado " +
                     "(Texture/Prev/PassPrev/PassFeedback/alias/Orig/LUT). " +
                     "Provável tela preta. Programa: " + std::to_string(pass.program));
        }
    }

    // Renderizar
    // IMPORTANTE: Garantir que os atributos estão habilitados
    // (alguns drivers podem desabilitar após mudar de programa)
    glBindVertexArray(m_VAO);

    // Garantir que os atributos estão habilitados
    glEnableVertexAttribArray(0); // Position
    glEnableVertexAttribArray(1); // TexCoord

    // IMPORTANTE: Garantir que a textura está bindada antes de renderizar
    // Alguns drivers podem desvincular a textura durante mudanças de estado
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);

    // IMPORTANTE: Não precisamos desabilitar blending aqui porque não habilitamos
    // O blending será aplicado apenas na renderização final na janela

    // DEBUG: Verificar se houve erro OpenGL e verificar se algo foi renderizado
    // Nota: glGetError pode não estar disponível, então vamos apenas verificar o framebuffer

    // Verificar se o framebuffer está completo (todos os passes)
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ERROR("Pass " + std::to_string(i) + ": Incomplete framebuffer after rendering! Status: " + std::to_string(status));
    }

    // Próximo pass usa a saída deste
    currentTexture = pass.texture;
    currentWidth = outputWidth;
    currentHeight = outputHeight;

    // Verificar se a textura de saída é válida
    if (i == 0 && currentTexture == 0)
    {
        LOG_ERROR("Pass 0: Invalid output texture (0)!");
    }
}


GLuint ShaderEngine::renderSinglePass(GLuint inputTexture, uint32_t width, uint32_t height)
{
    // Modo simples (shader único)
    if (m_framebuffer == 0 || m_outputWidth != width || m_outputHeight != height)
    {
        cleanupFramebuffer(m_framebuffer, m_outputTexture);
        createFramebuffer(width, height, false, m_framebuffer, m_outputTexture);
        m_outputWidth = width;
        m_outputHeight = height;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, width, height);

    glUseProgram(m_shaderProgram);

    GLint texLoc = getUniformLocation(m_shaderProgram, "Texture");
    if (texLoc >= 0)
    {
        glUniform1i(texLoc, 0);
    }

    GLint texSizeLoc = getUniformLocation(m_shaderProgram, "TextureSize");
    if (texSizeLoc >= 0)
    {
        glUniform2f(texSizeLoc, static_cast<float>(width), static_cast<float>(height));
    }

    GLint inputSizeLoc = getUniformLocation(m_shaderProgram, "InputSize");
    if (inputSizeLoc >= 0)
    {
        glUniform2f(inputSizeLoc, static_cast<float>(width), static_cast<float>(height));
    }

    GLint outputSizeLoc = getUniformLocation(m_shaderProgram, "OutputSize");
    if (outputSizeLoc >= 0)
    {
        glUniform2f(outputSizeLoc, static_cast<float>(width), static_cast<float>(height));
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);

    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return m_outputTexture;
}


GLuint ShaderEngine::applyShader(GLuint inputTexture, uint32_t width, uint32_t height)
{
    if (!m_shaderActive)
    {
        return inputTexture;
    }

    // Se temos preset, usar modo múltiplos passes
    if (!m_passes.empty() || !m_preset.getPasses().empty())
    {
        // Se passes ainda não foram carregados, carregar agora
        // IMPORTANTE: Verificar se temos passes válidos (program != 0), não apenas parameterInfo
        // Isso garante que se um shader falhou na compilação, tentamos recarregar
        bool hasValidPass = false;

        if (!m_passes.empty())
        {
            for (const auto &pass : m_passes)
            {
                if (pass.program != 0)
                {
                    hasValidPass = true;
                }
            }
        }

        // IMPORTANTE: Recarregar passes se:
        // 1. Não temos passes, OU
        // 2. Não temos passes válidos (mesmo que tenhamos parameterInfo)
        // Isso garante que se um shader falhou na compilação, tentamos recarregar
        if (m_passes.empty() || !hasValidPass)
        {
            // Tentar recarregar passes
            if (!loadPresetPasses())
            {
                // Se loadPresetPasses falhou, verificar novamente se temos passes válidos
                hasValidPass = false;
                if (!m_passes.empty())
                {
                    for (const auto &pass : m_passes)
                    {
                        if (pass.program != 0)
                        {
                            hasValidPass = true;
                            break;
                        }
                    }
                }

                // Se ainda não temos passes válidos, retornar textura original
                if (!hasValidPass)
                {
                    // IMPORTANTE: Resetar estado do OpenGL antes de retornar
                    glUseProgram(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    return inputTexture;
                }
            }
            else
            {
                // Se recarregou com sucesso, verificar novamente se temos passes válidos
                hasValidPass = false;
                for (const auto &pass : m_passes)
                {
                    if (pass.program != 0)
                    {
                        hasValidPass = true;
                        break;
                    }
                }
            }
        }

        // IMPORTANTE: Verificar novamente se temos pelo menos um pass válido antes de tentar renderizar
        if (!hasValidPass)
        {
            // Log apenas ocasionalmente para evitar spam (a cada 60 frames = ~1 segundo a 60fps)
            static int errorLogCounter = 0;
            if (errorLogCounter++ % 60 == 0)
            {
                LOG_ERROR("No valid pass found in preset. Returning original texture. (logged every 60 frames)");
            }
            // IMPORTANTE: Resetar estado do OpenGL antes de retornar
            glUseProgram(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return inputTexture;
        }

        // OTIMIZAÇÃO: Limitar resolução de processamento para ARM
        // Reduzir resolução de processamento para melhorar performance drasticamente
        uint32_t processingWidth = width;
        uint32_t processingHeight = height;
        
        if (m_maxShaderWidth > 0 && m_maxShaderHeight > 0)
        {
            // Se a resolução excede os limites, fazer downscale mantendo aspect ratio
            if (width > m_maxShaderWidth || height > m_maxShaderHeight)
            {
                float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
                
                if (width > m_maxShaderWidth)
                {
                    processingWidth = m_maxShaderWidth;
                    processingHeight = static_cast<uint32_t>(std::round(m_maxShaderWidth / aspectRatio));
                }
                
                if (processingHeight > m_maxShaderHeight)
                {
                    processingHeight = m_maxShaderHeight;
                    processingWidth = static_cast<uint32_t>(std::round(m_maxShaderHeight * aspectRatio));
                }
                
                // Garantir que seja múltiplo de 2 (melhor para texturas)
                processingWidth = (processingWidth / 2) * 2;
                processingHeight = (processingHeight / 2) * 2;
                
                static int logCounter = 0;
                if (logCounter++ % 300 == 0) // Log a cada 5 segundos (300 frames a 60fps)
                {
                    LOG_INFO("Processando shader em " + 
                             std::to_string(processingWidth) + "x" + std::to_string(processingHeight) + 
                             " (original: " + std::to_string(width) + "x" + std::to_string(height) + ")");
                }
            }
        }
        
        m_sourceWidth = processingWidth;
        m_sourceHeight = processingHeight;
        // IMPORTANTE: viewportWidth e viewportHeight devem ser as dimensões da janela,
        // não as dimensões da entrada. Eles devem ser atualizados via setViewport()
        // antes de chamar applyShader(). Se não foram atualizados, usar as dimensões da entrada como fallback.
        // Mas é melhor sempre atualizar via setViewport() antes de aplicar o shader.

        GLuint currentTexture = inputTexture;
        uint32_t currentWidth = processingWidth;
        uint32_t currentHeight = processingHeight;

        // IMPORTANTE: Guardar a textura original para passes que precisam dela (como hq2x)
        // Alguns shaders (como hqx-pass2) precisam tanto da saída do pass anterior quanto da entrada original
        GLuint originalTexture = inputTexture;

        // IMPORTANTE: Para motion blur, precisamos manter um histórico de frames anteriores
        // O histórico deve conter frames JÁ PROCESSADOS (saída do shader), não a entrada
        // Por isso, não inicializamos aqui - o histórico será preenchido após renderizar

        // DEBUG: Verificar se a textura de entrada é válida
        if (inputTexture == 0)
        {
            LOG_ERROR("applyShader: Invalid input texture (0)!");
            return 0;
        }

        // IMPORTANTE: Incrementar FrameCount apenas uma vez por frame (não por pass)
        // Isso permite que shaders animados funcionem corretamente
        // FrameCount é usado por shaders para criar animações baseadas no número de frames
        m_frameCount += 1.0f;
        m_time += 0.016f; // ~60fps (aproximado, será ajustado pelo tempo real se necessário)

        // Aplicar cada pass
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            renderMultipassPass(i, currentTexture, currentWidth, currentHeight, originalTexture);
        }

        // Desvincular framebuffer após todos os passes
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Desabilita GL_FRAMEBUFFER_SRGB pra não vazar pro draw final.
        glDisable(0x8DB9);

        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        // Swap PassFeedback ping-pong: o que foi escrito neste frame vira a
        // textura "anterior" pro próximo frame; a antiga textura "anterior"
        // vira o destino do próximo frame. Aplicado apenas nos passes onde
        // algum sampler PassFeedback<N>* foi detectado durante o bind.
        for (auto &fbPass : m_passes)
        {
            if (!fbPass.feedbackEnabled || fbPass.feedbackTexture == 0)
            {
                continue;
            }
            std::swap(fbPass.texture, fbPass.feedbackTexture);
            std::swap(fbPass.framebuffer, fbPass.feedbackFramebuffer);
        }

        // IMPORTANTE: Atualizar dimensões de saída para uso em maintainAspect
        // Essas são as dimensões FINAIS do último pass do shader
        m_outputWidth = currentWidth;
        m_outputHeight = currentHeight;

        // IMPORTANTE: Resetar viewport para um tamanho grande após os passes
        // Isso garante que a renderização final use o viewport correto
        // O viewport será configurado corretamente em Application::run() antes de renderizar
        // Usar dimensões padrão razoáveis como fallback (será sobrescrito)
        glViewport(0, 0, 1920, 1080);

        // IMPORTANTE: Atualizar histórico de frames para motion blur
        // O histórico deve conter frames JÁ PROCESSADOS (saída do shader)
        // IMPORTANTE: Precisamos copiar o conteúdo para texturas dedicadas, não apenas armazenar referências
        // As texturas dos framebuffers são reutilizadas e sobrescritas a cada frame
        if (currentTexture != 0 && currentWidth > 0 && currentHeight > 0)
        {
            // Encontrar qual framebuffer contém currentTexture
            GLuint sourceFramebuffer = 0;
            for (const auto &pass : m_passes)
            {
                if (pass.texture == currentTexture)
                {
                    sourceFramebuffer = pass.framebuffer;
                    break;
                }
            }

            // Criar uma nova textura para o histórico (se necessário)
            GLuint historyTexture = 0;
            if (m_frameHistory.size() < MAX_FRAME_HISTORY)
            {
                // Criar nova textura para o histórico
                glGenTextures(1, &historyTexture);
                glBindTexture(GL_TEXTURE_2D, historyTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, currentWidth, currentHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            else
            {
                // Reutilizar a textura mais antiga (ring buffer)
                historyTexture = m_frameHistory.back();
                m_frameHistory.pop_back();
                m_frameHistoryWidths.pop_back();
                m_frameHistoryHeights.pop_back();

                // Verificar se precisa redimensionar
                if (m_frameHistoryWidths.empty() || m_frameHistoryHeights.empty() ||
                    m_frameHistoryWidths.back() != currentWidth || m_frameHistoryHeights.back() != currentHeight)
                {
                    glBindTexture(GL_TEXTURE_2D, historyTexture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, currentWidth, currentHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            }

            // Copiar conteúdo do framebuffer para a textura do histórico
            // Usar renderização simples: renderizar a textura atual para um framebuffer temporário
            // vinculado à textura do histórico usando o VAO e shader program existente
            if (sourceFramebuffer != 0 && historyTexture != 0)
            {
                // Criar framebuffer temporário reutilizável se ainda não existe
                if (m_copyFramebuffer == 0)
                {
                    glGenFramebuffers(1, &m_copyFramebuffer);
                }
                
                // Reutilizar framebuffer existente
                glBindFramebuffer(GL_FRAMEBUFFER, m_copyFramebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, historyTexture, 0);

                // Verificar se o framebuffer está completo
                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status == GL_FRAMEBUFFER_COMPLETE)
                {
                    glViewport(0, 0, currentWidth, currentHeight);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);

                    // Renderizar a textura atual para o framebuffer temporário usando o VAO
                    // Usar o shader program do primeiro pass para copiar a textura
                    if (!m_passes.empty() && m_passes[0].program != 0)
                    {
                        // Usar o shader program do primeiro pass temporariamente
                        glUseProgram(m_passes[0].program);
                        glBindVertexArray(m_VAO);

                        // Bind da textura atual
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, currentTexture);

                        // Configurar uniform Texture se existir
                        GLint texLoc = getUniformLocation(m_passes[0].program, "Texture");
                        if (texLoc < 0)
                        {
                            texLoc = getUniformLocation(m_passes[0].program, "Source");
                        }
                        if (texLoc >= 0)
                        {
                            glUniform1i(texLoc, 0);
                        }

                        // Renderizar
                        glEnableVertexAttribArray(0);
                        glEnableVertexAttribArray(1);
                        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

                        glBindVertexArray(0);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glUseProgram(0);
                    }
                    else
                    {
                        // Se não há shader program disponível, a textura ficará vazia
                        // Mas pelo menos não crasha
                        LOG_WARN("Could not copy frame to history (no shader program)");
                    }
                }

                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                // Não deletar o framebuffer - reutilizar no próximo frame
            }

            // IMPORTANTE: Por enquanto, vamos apenas armazenar a referência da textura
            // Isso não é ideal, mas permite que o código compile e funcione
            // O problema é que as texturas dos framebuffers são reutilizadas, então
            // o histórico pode conter texturas sobrescritas
            // Uma implementação completa precisaria copiar o conteúdo usando renderização

            // Adicionar ao histórico
            m_frameHistory.insert(m_frameHistory.begin(), historyTexture);
            m_frameHistoryWidths.insert(m_frameHistoryWidths.begin(), currentWidth);
            m_frameHistoryHeights.insert(m_frameHistoryHeights.begin(), currentHeight);

            // DEBUG: Log apenas quando necessário
            if (m_frameHistory.size() == 1)
            {
            }
            else if (m_frameHistory.size() == MAX_FRAME_HISTORY)
            {
            }
        }

        if (currentTexture == 0)
        {
            LOG_ERROR("applyShader: Invalid final texture (0)! Returning original input texture.");
            return inputTexture;
        }

        return currentTexture;
    }
    else
    {
        return renderSinglePass(inputTexture, width, height);
    }
}

uint32_t ShaderEngine::calculateScale(uint32_t sourceSize, const std::string &scaleType, float scale,
                                      uint32_t viewportSize, uint32_t /*absoluteValue*/)
{
    // Se scaleType está vazio ou não especificado, usar "source" com scale 1.0 (padrão RetroArch)
    if (scaleType.empty() || scaleType == "source")
    {
        // Se scale não foi especificado (0.0), usar 1.0
        if (scale == 0.0f)
        {
            scale = 1.0f;
        }
        return static_cast<uint32_t>(std::round(sourceSize * scale));
    }
    else if (scaleType == "viewport")
    {
        // Se scale não foi especificado (0.0), usar 1.0
        if (scale == 0.0f)
        {
            scale = 1.0f;
        }
        return static_cast<uint32_t>(std::round(viewportSize * scale));
    }
    else if (scaleType == "absolute")
    {
        // Para absolute, o valor está em scale (como "800" no preset)
        return static_cast<uint32_t>(std::round(scale));
    }
    // Fallback: retornar sourceSize (sem escala) - mantém o tamanho original
    return sourceSize;
}

void ShaderEngine::applyCoreSizeUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth, uint32_t outputHeight)
{
    // Texture/Source será configurado DEPOIS do bind da textura (em applyShader)
    // Isso garante que a textura esteja vinculada antes de configurar o uniform

    // SourceSize (vec4 do RetroArch - convertido de params.SourceSize)
    GLint loc = getUniformLocation(program, "SourceSize");
    if (loc >= 0)
    {
        glUniform4f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight),
                    1.0f / static_cast<float>(inputWidth), 1.0f / static_cast<float>(inputHeight));
    }

    // OriginalSize (vec4 do RetroArch - convertido de params.OriginalSize)
    loc = getUniformLocation(program, "OriginalSize");
    if (loc >= 0)
    {
        glUniform4f(loc, static_cast<float>(m_sourceWidth), static_cast<float>(m_sourceHeight),
                    1.0f / static_cast<float>(m_sourceWidth), 1.0f / static_cast<float>(m_sourceHeight));
    }

    // OutputSize (vec4 do RetroArch - convertido de params.OutputSize)
    // IMPORTANTE: Alguns shaders usam vec4, outros usam vec2, e alguns podem usar vec3
    // Vamos tentar detectar o tipo verificando o código do shader primeiro
    // Se não conseguirmos detectar, tentamos vec2 primeiro (mais comum)
    loc = getUniformLocation(program, "OutputSize");
    if (loc >= 0)
    {
        // Verificar o tipo do uniform usando glGetActiveUniform
        // Primeiro, precisamos obter o número de uniforms ativos
        GLint numUniforms = 0;
        glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &numUniforms);

        GLenum uniformType = GL_FLOAT_VEC2; // Default
        bool foundType = false;

        for (GLint i = 0; i < numUniforms; ++i)
        {
            char uniformName[256];
            GLint uniformSize;
            GLenum type;
            glGetActiveUniform(program, i, sizeof(uniformName), nullptr, &uniformSize, &type, uniformName);

            if (std::string(uniformName) == "OutputSize")
            {
                uniformType = type;
                foundType = true;
                break;
            }
        }

        // Configurar baseado no tipo detectado
        if (foundType)
        {
            if (uniformType == GL_FLOAT_VEC2)
            {
                glUniform2f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
            }
            else if (uniformType == GL_FLOAT_VEC3)
            {
                // vec3: usar width, height, 1.0/width (ou similar)
                glUniform3f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight),
                            1.0f / static_cast<float>(outputWidth));
            }
            else if (uniformType == GL_FLOAT_VEC4)
            {
                glUniform4f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight),
                            1.0f / static_cast<float>(outputWidth), 1.0f / static_cast<float>(outputHeight));
            }
            else
            {
                // Fallback: tentar vec2
                glUniform2f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
            }
        }
        else
        {
            // Não encontramos o tipo, tentar vec2 primeiro (mais comum)
            glUniform2f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
        }

        if (passIndex == 0 || passIndex == 4)
        {
            std::string typeStr = "vec2 (fallback)";
            if (foundType)
            {
                if (uniformType == GL_FLOAT_VEC2)
                    typeStr = "vec2";
                else if (uniformType == GL_FLOAT_VEC3)
                    typeStr = "vec3";
                else if (uniformType == GL_FLOAT_VEC4)
                    typeStr = "vec4";
            }
        }
    }
    // Nota: Se OutputSize não for encontrado, pode ser que o shader não o use
    // ou que ele tenha sido otimizado fora pelo compilador GLSL
    // Não é necessariamente um erro, apenas um aviso informativo

}

void ShaderEngine::applyPassSizeUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight)
{
    GLint loc = 0;
    // PassOutputSize# - Tamanhos de saída de passes anteriores (RetroArch injeta isso)
    // Permite que shaders acessem informações de passes anteriores
    for (uint32_t i = 0; i < passIndex && i < m_passes.size(); ++i)
    {
        std::string passOutputName = "PassOutputSize" + std::to_string(i);
        loc = getUniformLocation(program, passOutputName);
        if (loc >= 0)
        {
            // Usar dimensões do pass anterior
            uint32_t prevWidth = m_passes[i].width;
            uint32_t prevHeight = m_passes[i].height;
            if (prevWidth == 0)
                prevWidth = inputWidth; // Fallback
            if (prevHeight == 0)
                prevHeight = inputHeight; // Fallback

            glUniform4f(loc, static_cast<float>(prevWidth), static_cast<float>(prevHeight),
                        1.0f / static_cast<float>(prevWidth), 1.0f / static_cast<float>(prevHeight));
        }
    }

    // PassInputSize# - Tamanhos de entrada de passes anteriores
    for (uint32_t i = 0; i < passIndex && i < m_passes.size(); ++i)
    {
        std::string passInputName = "PassInputSize" + std::to_string(i);
        loc = getUniformLocation(program, passInputName);
        if (loc >= 0)
        {
            // Para o primeiro pass, usar source size; para outros, usar output do pass anterior
            uint32_t prevInputWidth = (i == 0) ? m_sourceWidth : m_passes[i - 1].width;
            uint32_t prevInputHeight = (i == 0) ? m_sourceHeight : m_passes[i - 1].height;
            if (prevInputWidth == 0)
                prevInputWidth = inputWidth;
            if (prevInputHeight == 0)
                prevInputHeight = inputHeight;

            glUniform4f(loc, static_cast<float>(prevInputWidth), static_cast<float>(prevInputHeight),
                        1.0f / static_cast<float>(prevInputWidth), 1.0f / static_cast<float>(prevInputHeight));
        }
    }

    // Variáveis de configuração do pass atual (Scale, Filter, etc)
    // Essas são injetadas pelo RetroArch e podem ser usadas pelos shaders
    if (passIndex < m_passes.size())
    {
        const auto &passInfo = m_passes[passIndex].passInfo;

        // PassScale - Fator de escala do pass atual
        loc = getUniformLocation(program, "PassScale");
        if (loc >= 0)
        {
            // Média dos fatores X e Y
            float avgScale = (passInfo.scaleX + passInfo.scaleY) / 2.0f;
            glUniform1f(loc, avgScale);
        }

        // PassScaleX, PassScaleY - Escalas individuais
        loc = getUniformLocation(program, "PassScaleX");
        if (loc >= 0)
        {
            glUniform1f(loc, passInfo.scaleX);
        }

        loc = getUniformLocation(program, "PassScaleY");
        if (loc >= 0)
        {
            glUniform1f(loc, passInfo.scaleY);
        }

        // PassFilter - 1.0 para Linear, 0.0 para Nearest
        loc = getUniformLocation(program, "PassFilter");
        if (loc >= 0)
        {
            glUniform1f(loc, passInfo.filterLinear ? 1.0f : 0.0f);
        }
    }

}

void ShaderEngine::applyFrameUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight)
{
    GLint loc = 0;
    // FrameCount (uint convertido para float - convertido de params.FrameCount)
    // IMPORTANTE: Aplicar frame_count_mod# do passInfo (não do preset parameters)
    // RetroArch armazena frame_count_mod diretamente no pass
    float frameCountValue = m_frameCount;
    if (passIndex < m_passes.size())
    {
        const auto &passData = m_passes[passIndex];
        if (passData.passInfo.frameCountMod > 0)
        {
            frameCountValue = fmod(m_frameCount, static_cast<float>(passData.passInfo.frameCountMod));
        }
    }

    loc = getUniformLocation(program, "FrameCount");
    if (loc >= 0)
    {
        // FrameCount pode ser declarado como int ou float
        // Verificar o tipo do uniform e usar a função apropriada
        GLenum uniformType = GL_FLOAT;
        GLint uniformSize = 0;
        char uniformName[256];
        GLsizei nameLength = 0;

        // Encontrar o índice do uniform
        GLint uniformCount = 0;
        glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
        bool foundType = false;
        for (GLint i = 0; i < uniformCount; ++i)
        {
            glGetActiveUniform(program, i, 256, &nameLength, &uniformSize, &uniformType, uniformName);
            if (std::string(uniformName) == "FrameCount")
            {
                foundType = true;
                break;
            }
        }

        if (foundType && uniformType == GL_INT)
        {
            // Se é int, usar glUniform1i
            glUniform1i(loc, static_cast<GLint>(frameCountValue));
        }
        else
        {
            // Se é float ou não encontramos o tipo, usar glUniform1f (padrão)
            glUniform1f(loc, frameCountValue);
        }
    }
    // Remover aviso - é normal que uniforms não usados sejam otimizados pelo compilador

    // MVPMatrix (mat4 do RetroArch - matriz identidade)
    // RetroArch shaders podem usar MVPMatrix * VertexCoord
    // IMPORTANTE: Sem isso, o shader não renderiza nada!
    // Usar matriz identidade normal (sem inversão Y aqui)
    loc = getUniformLocation(program, "MVPMatrix");
    if (loc >= 0)
    {
        // Matriz identidade (sem transformação)
        float mvp[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
        glUniformMatrix4fv(loc, 1, GL_FALSE, mvp);
    }

    // FrameDirection (int do RetroArch)
    loc = getUniformLocation(program, "FrameDirection");
    if (loc >= 0)
    {
        glUniform1i(loc, 1); // Sempre 1 (forward)
    }

    // OriginalHistorySize0-7: dimensões dos frames anteriores.
    // Convenção slang: OriginalHistory0 = source atual, OriginalHistory1..N = N frames atrás.
    // m_frameHistoryWidths/Heights armazena o histórico real (mais recente primeiro).
    for (int i = 0; i <= 7; ++i)
    {
        std::string historyName = "OriginalHistorySize" + std::to_string(i);
        loc = getUniformLocation(program, historyName);
        if (loc < 0)
        {
            continue;
        }

        float w;
        float h;
        if (i == 0)
        {
            // OriginalHistory0 = source atual
            w = static_cast<float>(inputWidth);
            h = static_cast<float>(inputHeight);
        }
        else
        {
            size_t historyIdx = static_cast<size_t>(i - 1);
            if (historyIdx < m_frameHistoryWidths.size() &&
                historyIdx < m_frameHistoryHeights.size())
            {
                w = static_cast<float>(m_frameHistoryWidths[historyIdx]);
                h = static_cast<float>(m_frameHistoryHeights[historyIdx]);
            }
            else
            {
                // Sem histórico ainda neste slot: usar input atual como fallback seguro
                // (evita div-by-zero em shaders que fazem 1.0 / OriginalHistorySize.xy).
                w = static_cast<float>(inputWidth);
                h = static_cast<float>(inputHeight);
            }
        }

        glUniform4f(loc, w, h,
                    w > 0.0f ? 1.0f / w : 0.0f,
                    h > 0.0f ? 1.0f / h : 0.0f);
    }

}

void ShaderEngine::applyShaderParameterUniforms(GLuint program, uint32_t passIndex)
{
    GLint loc = 0;
    // Parâmetros extraídos de #pragma parameter (injetados pelo RetroArch)
    // Esses são parâmetros configuráveis que o RetroArch injeta como uniforms
    if (passIndex < m_passes.size())
    {
        const auto &extractedParams = m_passes[passIndex].extractedParameters;
        if (passIndex == 0 && !extractedParams.empty())
        {
        }
        for (const auto &param : extractedParams)
        {
            loc = getUniformLocation(program, param.first);
            if (loc >= 0)
            {
                // Verificar se há valor customizado (do usuário), do preset, ou usar valor padrão do #pragma parameter
                float value = param.second;
                auto customIt = m_customParameters.find(param.first);
                if (customIt != m_customParameters.end())
                {
                    value = customIt->second;
                }
                else
                {
                    const auto &presetParams = m_preset.getParameters();
                    auto presetIt = presetParams.find(param.first);
                    if (presetIt != presetParams.end())
                    {
                        value = presetIt->second;
                    }
                }
                glUniform1f(loc, value);
            }
            else if (passIndex == 0)
            {
                // Log apenas se não encontrou (pode ser normal se o shader não usa)
                // LOG_WARN("Pass 0: Uniform de parâmetro '" + param.first + "' não encontrado");
            }
        }
    }

    // Parâmetros customizados do shader com valores padrão (fallback para shaders antigos)
    // Esses valores serão usados se o shader não definir seus próprios
    loc = getUniformLocation(program, "BLURSCALEX");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.30f);
    }

    loc = getUniformLocation(program, "LOWLUMSCAN");
    if (loc >= 0)
    {
        glUniform1f(loc, 6.0f);
    }

    loc = getUniformLocation(program, "HILUMSCAN");
    if (loc >= 0)
    {
        glUniform1f(loc, 8.0f);
    }

    loc = getUniformLocation(program, "BRIGHTBOOST");
    if (loc >= 0)
    {
        glUniform1f(loc, 1.25f);
    }

    loc = getUniformLocation(program, "MASK_DARK");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.25f);
    }

    loc = getUniformLocation(program, "MASK_FADE");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.8f);
    }

    // Parâmetros comuns de outros shaders (valores seguros/padrão)
    loc = getUniformLocation(program, "RESSWITCH_ENABLE");
    if (loc >= 0)
    {
        glUniform1f(loc, 1.0f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_TRESHOLD");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.1f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_BAR_STR");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.6f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_BAR_SIZE");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.5f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_BAR_SMOOTH");
    if (loc >= 0)
    {
        glUniform1f(loc, 1.0f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_SHAKE_MAX");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.25f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_ROT_MAX");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.2f);
    }

    loc = getUniformLocation(program, "RESSWITCH_GLITCH_WOB_MAX");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.1f);
    }

    // Parâmetros do shader Grade/Afterglow
    loc = getUniformLocation(program, "AS");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.20f); // Afterglow Strength
    }

    loc = getUniformLocation(program, "asat");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.33f); // Afterglow saturation
    }

    loc = getUniformLocation(program, "PR");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.32f); // Persistence Red
    }

    loc = getUniformLocation(program, "PG");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.32f); // Persistence Green
    }

    loc = getUniformLocation(program, "PB");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.32f); // Persistence Blue
    }

}

void ShaderEngine::applyAlternateSizeUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth, uint32_t outputHeight)
{
    GLint loc = 0;
    // Parâmetros de resolução e escala
    loc = getUniformLocation(program, "internal_res");
    if (loc >= 0)
    {
        glUniform1f(loc, 1.0f); // Internal Resolution
    }

    loc = getUniformLocation(program, "auto_res");
    if (loc >= 0)
    {
        glUniform1f(loc, 0.0f); // Auto resolution
    }

    // TextureSize (vec2 alternativo)
    // IMPORTANTE: Para shaders como interlacing.glsl que escalam a altura (scale_y = 2.0),
    // TextureSize pode precisar refletir a altura da SAÍDA, não da entrada
    // O shader interlacing.glsl usa: y = 2.000001 * TextureSize.y * vTexCoord.y
    // Onde vTexCoord.y é a coordenada na textura de SAÍDA (0.0 a 1.0)
    // Se TextureSize.y é a altura da entrada (224) mas a saída é 448, o cálculo fica errado
    // Vamos tentar usar a altura da SAÍDA quando o pass escala a altura
    loc = getUniformLocation(program, "TextureSize");
    if (loc >= 0)
    {
        // Para shaders que escalam a altura (como interlacing.glsl com scale_y = 2.0),
        // usar a altura da SAÍDA em TextureSize.y para que o cálculo de y funcione corretamente
        // Isso é necessário porque vTexCoord.y é baseado na textura de saída
        float textureSizeY = static_cast<float>(inputHeight);

        // IMPORTANTE: Para shaders como interlacing.glsl que escalam a altura (scale_y = 2.0),
        // o shader lê COMPAT_TEXTURE(Source, vTexCoord) onde Source é a entrada.
        // Ajustamos o vertex shader para que vTexCoord.y seja baseado na entrada (replicando linhas).
        // O shader calcula o padrão de interlace usando: y = 2.000001 * TextureSize.y * vTexCoord.y
        // Como ajustamos o vertex shader para que vTexCoord.y seja baseado na entrada,
        // mas o padrão de interlace precisa ser baseado na SAÍDA, precisamos ajustar TextureSize.y
        // para ser a altura da SAÍDA quando o pass escala a altura. Isso permite que o cálculo
        // do interlace funcione corretamente para determinar quais linhas da SAÍDA devem ser escuras.
        // O ajuste no vertex shader garante que a leitura da textura de entrada seja correta.
        if (outputHeight != inputHeight && passIndex == 3)
        {
            textureSizeY = static_cast<float>(outputHeight);
        }
        // Para Pass 4 (box-center.glsl), TextureSize deve ser o tamanho da textura de entrada (1280x448)
        // O shader usa TextureSize para calcular vTexCoord baseado na textura de entrada
        // Se TextureSize for diferente do tamanho real da textura, vTexCoord pode sair de [0,1]

        glUniform2f(loc, static_cast<float>(inputWidth), textureSizeY);
    }
    // Nota: Se TextureSize não for encontrado, pode ser que o shader não o use
    // ou que ele tenha sido otimizado fora pelo compilador GLSL
    // Isso é normal e não é um erro - não gerar avisos

    // InputSize (vec2 alternativo)
    loc = getUniformLocation(program, "InputSize");
    if (loc >= 0)
    {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
    }
    // Nota: Se InputSize não for encontrado, pode ser que o shader não o use
    // ou que ele tenha sido otimizado fora pelo compilador GLSL
    // Isso é normal e não é um erro - não gerar avisos

    // VideoSize (tamanho original)
    loc = getUniformLocation(program, "IN.video_size");
    if (loc >= 0)
    {
        glUniform2f(loc, static_cast<float>(m_sourceWidth), static_cast<float>(m_sourceHeight));
    }

    // TextureSize (alternativo)
    loc = getUniformLocation(program, "IN.texture_size");
    if (loc >= 0)
    {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
    }

    // OutputSize (alternativo como vec2 - alguns shaders como crt-geom usam vec2 OutputSize)
    // IMPORTANTE: Se o shader declarou OutputSize como vec2, precisamos usar glUniform2f
    // Mas se declarou como vec4, precisamos usar glUniform4f
    // O problema é que não podemos saber qual é sem verificar o tipo do uniform
    // Vamos tentar configurar como vec2 também (muitos shaders usam vec2)
    // NOTA: Se o shader espera vec4 e passamos vec2, pode causar problemas
    // Mas se espera vec2 e passamos vec4, também pode causar problemas
    // A solução é verificar o tipo do uniform, mas isso requer glGetActiveUniform
    // Por enquanto, vamos configurar ambos (vec4 acima e vec2 aqui se necessário)

    // OutputSize como vec2 (IN.output_size é um formato alternativo)
    loc = getUniformLocation(program, "IN.output_size");
    if (loc >= 0)
    {
        glUniform2f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
    }

    // IMPORTANTE: Se o shader declarou OutputSize como vec2 (não vec4), precisamos usar glUniform2f
    // Mas já configuramos como vec4 acima. O problema é que se o shader espera vec2,
    // passar vec4 pode não funcionar corretamente.
    // Vamos adicionar uma verificação: se o uniform OutputSize existe mas não aceitou vec4,
    // tentar como vec2. Mas isso é complicado sem verificar o tipo.
    // Por enquanto, vamos assumir que se o shader usa vec2 OutputSize, ele também terá
    // uma declaração explícita e o glUniform4f acima pode não funcionar.
    // Mas na prática, muitos drivers OpenGL são tolerantes com isso.

}

void ShaderEngine::applyGlobalPresetUniforms(GLuint program)
{
    GLint loc = 0;
    // Frame count e time
    // IMPORTANTE: NÃO incrementar aqui! FrameCount deve ser incrementado apenas uma vez por frame
    // no início de applyShader(), não a cada chamada de setupUniforms() (que é chamado para cada pass)
    // m_frameCount += 1.0f; // MOVED TO applyShader()
    // m_time += 0.016f; // ~60fps

    loc = getUniformLocation(program, "IN.frame_count");
    if (loc >= 0)
    {
        glUniform1f(loc, m_frameCount);
    }

    loc = getUniformLocation(program, "FRAMEINDEX");
    if (loc >= 0)
    {
        glUniform1f(loc, m_frameCount);
    }

    loc = getUniformLocation(program, "TIME");
    if (loc >= 0)
    {
        glUniform1f(loc, m_time);
    }

    // Parâmetros globais do preset
    const auto &params = m_preset.getParameters();
    for (const auto &param : params)
    {
        loc = getUniformLocation(program, param.first);
        if (loc >= 0)
        {
            glUniform1f(loc, param.second);
        }
    }
}


void ShaderEngine::setupUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight,
                                 uint32_t outputWidth, uint32_t outputHeight)
{
    applyCoreSizeUniforms(program, passIndex, inputWidth, inputHeight, outputWidth, outputHeight);
    applyPassSizeUniforms(program, passIndex, inputWidth, inputHeight);
    applyFrameUniforms(program, passIndex, inputWidth, inputHeight);
    applyShaderParameterUniforms(program, passIndex);
    applyAlternateSizeUniforms(program, passIndex, inputWidth, inputHeight, outputWidth, outputHeight);
    applyGlobalPresetUniforms(program);
}

bool ShaderEngine::loadTextureReference(const std::string &name, const std::string &path)
{
    // Verificar se a textura já foi carregada
    if (m_textureReferences.find(name) != m_textureReferences.end())
    {
        LOG_WARN("Texture '" + name + "' already loaded");
        return true;
    }

    // Verificar se o arquivo existe
    if (!fs::exists(path))
    {
        LOG_ERROR("Texture file not found: " + path);
        return false;
    }

    // Abrir arquivo PNG
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        LOG_ERROR("Failed to open texture file: " + path);
        return false;
    }

    // Verificar assinatura PNG
    png_byte header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
    {
        fclose(fp);
        LOG_ERROR("File is not a valid PNG: " + path);
        return false;
    }

    // Inicializar estruturas libpng
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        fclose(fp);
        LOG_ERROR("Failed to create PNG read structure");
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(fp);
        LOG_ERROR("Failed to create PNG info structure");
        return false;
    }

    // Configurar tratamento de erros
    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        LOG_ERROR("Error processing PNG: " + path);
        return false;
    }

    // Configurar I/O
    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);

    // Ler informações do PNG
    png_read_info(png_ptr, info_ptr);

    uint32_t width = png_get_image_width(png_ptr, info_ptr);
    uint32_t height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    LOG_INFO("Carregando textura PNG: " + name + " (" + std::to_string(width) + "x" + std::to_string(height) +
             ", bitDepth=" + std::to_string(bit_depth) + ", colorType=" + std::to_string(color_type) + ")");

    // Converter para formato RGBA 8-bit se necessário
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY)
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);

    // Atualizar informações após conversões
    png_read_update_info(png_ptr, info_ptr);

    // Alocar buffer para dados da imagem
    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    int rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    std::vector<uint8_t> imageData(rowbytes * height);

    for (uint32_t y = 0; y < height; y++)
    {
        row_pointers[y] = imageData.data() + y * rowbytes;
    }

    // Ler dados da imagem
    png_read_image(png_ptr, row_pointers);

    // Ler informações finais
    png_read_end(png_ptr, info_ptr);

    // Limpar estruturas libpng
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(fp);
    free(row_pointers);

    // Criar textura OpenGL
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Carregar dados na textura primeiro
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, imageData.data());

    // Aplicar configurações do preset (filter_linear, wrap_mode, mipmap)
    // Buscar configurações da textura no preset
    const auto &textures = m_preset.getTextures();
    bool filterLinear = true;               // Padrão: linear
    std::string wrapMode = "clamp_to_edge"; // Padrão: clamp_to_edge
    bool mipmap = false;                    // Padrão: sem mipmap

    if (textures.find(name) != textures.end())
    {
        const auto &texInfo = textures.at(name);
        filterLinear = texInfo.linear;
        wrapMode = texInfo.wrapMode;
        mipmap = texInfo.mipmap;
    }

    // Aplicar configurações (textura já está bindada)
    // Aplicar filtro (GL_LINEAR ou GL_NEAREST)
    GLenum filter = filterLinear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // Se mipmap for necessário, usar filtros de mipmap
    if (mipmap)
    {
        if (filterLinear)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    // Aplicar wrap mode
    GLenum wrap = wrapModeToGLEnum(wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

    glBindTexture(GL_TEXTURE_2D, 0);

    m_textureReferences[name] = texture;

    return true;
}

void ShaderEngine::cleanupTextureReferences()
{
    for (auto &tex : m_textureReferences)
    {
        if (tex.second != 0)
        {
            glDeleteTextures(1, &tex.second);
        }
    }
    m_textureReferences.clear();
}

void ShaderEngine::cleanupPresetPasses()
{
    for (auto &pass : m_passes)
    {
        if (pass.program != 0)
        {
            glDeleteProgram(pass.program);
        }
        if (pass.vertexShader != 0)
        {
            glDeleteShader(pass.vertexShader);
        }
        if (pass.fragmentShader != 0)
        {
            glDeleteShader(pass.fragmentShader);
        }
        cleanupFramebuffer(pass.framebuffer, pass.texture);
        cleanupFramebuffer(pass.feedbackFramebuffer, pass.feedbackTexture);
        pass.feedbackEnabled = false;
    }
    // IMPORTANTE: Limpar apenas recursos OpenGL, mas preservar parameterInfo
    // para que os parâmetros estejam disponíveis na UI mesmo se a compilação falhar
    // Os parameterInfo serão limpos apenas quando um novo preset for carregado
    m_passes.clear();

    // Limpar histórico de frames
    // IMPORTANTE: Não deletar as texturas aqui, pois elas pertencem aos framebuffers dos passes
    m_frameHistory.clear();
    m_frameHistoryWidths.clear();
    m_frameHistoryHeights.clear();
}

bool ShaderEngine::compileShader(const std::string &source, GLenum type, GLuint &shader)
{
    shader = glCreateShader(type);
    const char *src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::string errorMsg = std::string(infoLog);
        LOG_ERROR("Error compiling shader: " + errorMsg);

        // Tentar corrigir automaticamente se o erro for sobre vec3 = vec4
        if (errorMsg.find("initializer of type vec4 cannot be assigned to variable of type vec3") != std::string::npos ||
            errorMsg.find("cannot convert") != std::string::npos)
        {
            // Este erro será tratado no nível superior (loadPresetPasses)
            // onde temos acesso ao código fonte completo
        }

        glDeleteShader(shader);
        shader = 0;
        return false;
    }

    return true;
}

bool ShaderEngine::linkProgram(GLuint vertexShader, GLuint fragmentShader)
{
    m_shaderProgram = glCreateProgram();
    if (m_shaderProgram == 0)
    {
        LOG_ERROR("Failed to create shader program");
        return false;
    }

    glAttachShader(m_shaderProgram, vertexShader);
    glAttachShader(m_shaderProgram, fragmentShader);
    glLinkProgram(m_shaderProgram);

    GLint success;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        LOG_ERROR("Error linking shader program: " + std::string(infoLog));
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
        return false;
    }

    // Pré-cachear uniforms comuns após linkagem bem-sucedida
    preCacheCommonUniforms(m_shaderProgram);

    return true;
}

void ShaderEngine::disableShader()
{
    if (m_shaderProgram != 0)
    {
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
    }
    if (m_vertexShader != 0)
    {
        glDeleteShader(m_vertexShader);
        m_vertexShader = 0;
    }
    if (m_fragmentShader != 0)
    {
        glDeleteShader(m_fragmentShader);
        m_fragmentShader = 0;
    }
    cleanupFramebuffer(m_framebuffer, m_outputTexture);
    m_shaderActive = false;
    m_uniformLocations.clear();
}

GLint ShaderEngine::getUniformLocation(GLuint program, const std::string &name)
{
    std::string key = std::to_string(program) + "_" + name;
    auto it = m_uniformLocations.find(key);
    if (it != m_uniformLocations.end())
    {
        return it->second;
    }

    GLint location = glGetUniformLocation(program, name.c_str());
    if (location >= 0)
    {
        m_uniformLocations[key] = location;
    }
    return location;
}

void ShaderEngine::preCacheCommonUniforms(GLuint program)
{
    // Lista de uniforms comuns usados frequentemente
    // Pré-cachear após linkagem para evitar chamadas glGetUniformLocation repetidas
    const std::vector<std::string> commonUniforms = {
        "Texture", "Source", "InputTexture", "OrigTexture",
        "SourceSize", "InputSize", "OutputSize", "OriginalSize",
        "IN.texture_size", "IN.video_size", "IN.output_size",
        "IN.frame_count", "FRAMEINDEX", "TIME",
        "brightness", "contrast", "flipY"
    };
    
    for (const auto& uniformName : commonUniforms)
    {
        getUniformLocation(program, uniformName);
    }
}

void ShaderEngine::createFramebuffer(uint32_t width, uint32_t height, bool floatBuffer, GLuint &fb, GLuint &tex, bool srgbBuffer)
{
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Usar RGBA para garantir compatibilidade e preservar alpha
    // IMPORTANTE: Para shaders que usam alpha (como Game Boy), precisamos preservar o alpha
    // GL_RGBA (0x1908) é equivalente a GL_RGBA8 e garante 8 bits por canal incluindo alpha
    // Para float buffers, usar GL_RGBA32F que também preserva alpha
    // Para sRGB buffers, usar GL_SRGB8_ALPHA8
    GLenum internalFormat = GL_RGBA;
    if (floatBuffer)
    {
        internalFormat = GL_RGBA32F;
    }
    else if (srgbBuffer)
    {
        internalFormat = GL_SRGB8_ALPHA8;
    }

    GLenum format = GL_RGBA;
    GLenum type = floatBuffer ? GL_FLOAT : GL_UNSIGNED_BYTE;

    // IMPORTANTE: Garantir que o alpha seja preservado na textura
    // Criar a textura com dados nulos (nullptr) para garantir que o alpha seja inicializado corretamente
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, nullptr);

    // IMPORTANTE: Verificar se o formato suporta alpha
    // Se o driver não suportar GL_RGBA, pode ser necessário usar GL_RGBA8 explicitamente
    // Mas GL_RGBA deve funcionar na maioria dos casos
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ERROR("Incomplete framebuffer! Status: " + std::to_string(status));
        cleanupFramebuffer(fb, tex);
        return;
    }

    // DEBUG: Log do framebuffer criado

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderEngine::cleanupFramebuffer(GLuint &fb, GLuint &tex)
{
    if (tex != 0)
    {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    if (fb != 0)
    {
        glDeleteFramebuffers(1, &fb);
        fb = 0;
    }
}

void ShaderEngine::createQuad()
{
    // Quad em coordenadas de clip space (vec4 Position: x, y, z, w)
    // Position é vec4 para compatibilidade com RetroArch
    // IMPORTANTE: Coordenadas de textura normais (não invertidas) para shaders
    // A inversão será feita pela MVPMatrix se necessário
    float vertices[] = {
        // Position (vec4: x, y, z, w) + TexCoord (vec2)
        -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, // bottom-left
        1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f,  // bottom-right
        1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f,   // top-right
        -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f   // top-left
    };

    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0};

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);

    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Atributo 0: Position (vec4: x, y, z, w)
    // Também usado para VertexCoord (RetroArch shaders podem usar qualquer um)
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    // Se o shader usar VertexCoord no mesmo location, ele receberá os mesmos dados
    // (não precisamos fazer bind separado, apenas garantir que o location 0 está correto)

    // Atributo 1: TexCoord (vec2)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Ligar atributos aos nomes do shader (necessário quando não usamos layout(location))
    // Isso será feito ao linkar o programa, mas precisamos garantir que os nomes estejam corretos

    glBindVertexArray(0);
}

void ShaderEngine::cleanupQuad()
{
    if (m_VAO != 0)
    {
        glDeleteVertexArrays(1, &m_VAO);
        m_VAO = 0;
    }
    if (m_VBO != 0)
    {
        glDeleteBuffers(1, &m_VBO);
        m_VBO = 0;
    }
    if (m_EBO != 0)
    {
        glDeleteBuffers(1, &m_EBO);
        m_EBO = 0;
    }
}

void ShaderEngine::setUniform(const std::string &name, float value)
{
    // Implementação para modo simples
    if (m_shaderActive && m_shaderProgram != 0)
    {
        GLint loc = getUniformLocation(m_shaderProgram, name);
        if (loc >= 0)
        {
            glUniform1f(loc, value);
        }
    }
}

void ShaderEngine::setUniform(const std::string &name, float x, float y)
{
    if (m_shaderActive && m_shaderProgram != 0)
    {
        GLint loc = getUniformLocation(m_shaderProgram, name);
        if (loc >= 0)
        {
            glUniform2f(loc, x, y);
        }
    }
}

void ShaderEngine::setUniform(const std::string &name, float x, float y, float z, float w)
{
    if (m_shaderActive && m_shaderProgram != 0)
    {
        GLint loc = getUniformLocation(m_shaderProgram, name);
        if (loc >= 0)
        {
            glUniform4f(loc, x, y, z, w);
        }
    }
}

std::string ShaderEngine::processIncludes(const std::string &source, const std::string &basePath)
{
    std::string result = source;
    std::regex includeRegex(R"(#include\s+["<]([^">]+)[">])");
    std::smatch match;

    // Processar todos os includes
    while (std::regex_search(result, match, includeRegex))
    {
        std::string includePath = match[1].str();
        std::string fullPath;

        // Tentar resolver o caminho
        if (includePath[0] == '/')
        {
            // Caminho absoluto
            fullPath = includePath;
        }
        else
        {
            // Caminho relativo - tentar várias localizações
            fs::path currentPath = fs::current_path();

            // 1. Relativo ao diretório do shader atual
            if (!basePath.empty())
            {
                fs::path base(basePath);
                fs::path resolved = base / includePath;
                if (fs::exists(resolved))
                {
                    fullPath = resolved.string();
                }
            }

            // 2. Em shaders/shaders_slang/
            if (fullPath.empty())
            {
                fs::path slangPath = currentPath / "shaders" / "shaders_slang" / includePath;
                if (fs::exists(slangPath))
                {
                    fullPath = slangPath.string();
                }
            }

            // 3. Relativo ao diretório atual
            if (fullPath.empty())
            {
                fs::path relPath = currentPath / includePath;
                if (fs::exists(relPath))
                {
                    fullPath = relPath.string();
                }
            }

            // 4. Tentar com caminho relativo do shader (subindo diretórios)
            if (fullPath.empty() && !basePath.empty())
            {
                fs::path base(basePath);
                // Remover "../" do início
                std::string cleanPath = includePath;
                while (cleanPath.find("../") == 0)
                {
                    cleanPath = cleanPath.substr(3);
                    base = base.parent_path();
                }
                fs::path resolved = base / cleanPath;
                if (fs::exists(resolved))
                {
                    fullPath = resolved.string();
                }
            }
        }

        if (!fullPath.empty() && fs::exists(fullPath))
        {
            // Carregar arquivo incluído
            std::ifstream includeFile(fullPath);
            if (includeFile.is_open())
            {
                std::stringstream includeBuffer;
                includeBuffer << includeFile.rdbuf();
                std::string includeContent = includeBuffer.str();
                includeFile.close();

                // Processar includes recursivamente no arquivo incluído
                fs::path includeFilePath(fullPath);
                std::string includeDir = includeFilePath.parent_path().string();
                includeContent = ShaderPreprocessor::processIncludes(includeContent, includeDir);

                // Substituir o #include pelo conteúdo
                result = std::regex_replace(result, includeRegex, includeContent, std::regex_constants::format_first_only);
                LOG_INFO("Included file: " + fullPath);
            }
            else
            {
                LOG_WARN("Failed to open included file: " + fullPath);
                // Remover o #include que falhou
                result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
            }
        }
        else
        {
            LOG_WARN("Included file not found: " + includePath + " (base: " + basePath + ")");
            // Remover o #include que falhou
            result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
        }
    }

    return result;
}

void ShaderEngine::setViewport(uint32_t width, uint32_t height)
{
    // Aplicar limite de resolução apenas se configurado pelo usuário
    if (m_maxShaderWidth > 0 && m_maxShaderHeight > 0)
    {
        // Se o viewport excede os limites configurados, fazer downscale mantendo aspect ratio
        if (width > m_maxShaderWidth || height > m_maxShaderHeight)
        {
            float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
            uint32_t limitedWidth = width;
            uint32_t limitedHeight = height;
            
            if (width > m_maxShaderWidth)
            {
                limitedWidth = m_maxShaderWidth;
                limitedHeight = static_cast<uint32_t>(std::round(m_maxShaderWidth / aspectRatio));
            }
            
            if (limitedHeight > m_maxShaderHeight)
            {
                limitedHeight = m_maxShaderHeight;
                limitedWidth = static_cast<uint32_t>(std::round(m_maxShaderHeight * aspectRatio));
            }
            
            // Garantir múltiplo de 2
            limitedWidth = (limitedWidth / 2) * 2;
            limitedHeight = (limitedHeight / 2) * 2;
            
            m_viewportWidth = limitedWidth;
            m_viewportHeight = limitedHeight;
            
            static int logCounter = 0;
            if (logCounter++ % 300 == 0) // Log a cada 5 segundos
            {
                LOG_INFO("Viewport limitado para " + 
                         std::to_string(limitedWidth) + "x" + std::to_string(limitedHeight) + 
                         " (original: " + std::to_string(width) + "x" + std::to_string(height) + 
                         ", limite configurado: " + std::to_string(m_maxShaderWidth) + "x" + std::to_string(m_maxShaderHeight) + ")");
            }
        }
        else
        {
            m_viewportWidth = width;
            m_viewportHeight = height;
        }
    }
    else
    {
        // Sem limite configurado - usar resolução original
        m_viewportWidth = width;
        m_viewportHeight = height;
    }
}

GLenum ShaderEngine::wrapModeToGLEnum(const std::string &wrapMode)
{
    if (wrapMode == "repeat" || wrapMode == "REPEAT")
    {
        return GL_REPEAT;
    }
    else if (wrapMode == "mirrored_repeat" || wrapMode == "MIRRORED_REPEAT")
    {
        return GL_MIRRORED_REPEAT;
    }
    else if (wrapMode == "clamp_to_border" || wrapMode == "CLAMP_TO_BORDER")
    {
        return GL_CLAMP_TO_BORDER;
    }
    else if (wrapMode == "clamp_to_edge" || wrapMode == "CLAMP_TO_EDGE")
    {
        return GL_CLAMP_TO_EDGE;
    }
    // Padrão: clamp_to_edge
    return GL_CLAMP_TO_EDGE;
}

void ShaderEngine::applyTextureSettings(GLuint texture, bool filterLinear, const std::string &wrapMode, bool generateMipmap)
{
    if (texture == 0)
        return;

    // IMPORTANTE: A textura já deve estar bindada quando esta função é chamada
    // Apenas aplicar os parâmetros sem fazer bind/unbind
    // Isso evita desvincular a textura acidentalmente

    // Aplicar filtro (GL_LINEAR ou GL_NEAREST)
    GLenum filter = filterLinear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // Se mipmap for necessário, usar filtros de mipmap
    if (generateMipmap)
    {
        if (filterLinear)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    // Aplicar wrap mode
    GLenum wrap = wrapModeToGLEnum(wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}

std::vector<ShaderEngine::ShaderParameter> ShaderEngine::getShaderParameters() const
{
    std::vector<ShaderParameter> params;

    if (!m_shaderActive)
    {
        static int logCount = 0;
        if (++logCount % 60 == 0) // Log a cada 60 chamadas
        {
            LOG_WARN("getShaderParameters: shader is not active");
        }
        return params;
    }

    if (m_passes.empty())
    {
        static int logCount = 0;
        if (++logCount % 60 == 0) // Log a cada 60 chamadas
        {
            LOG_WARN("getShaderParameters: shader ativo mas passes vazios (shader ativo: " +
                     std::string(m_shaderActive ? "sim" : "não") + ", passes: " +
                     std::to_string(m_passes.size()) + ", presetPath: " + m_presetPath);
        }
        return params;
    }

    // Coletar parâmetros de todos os passes (principalmente do pass 0)
    // Usar um map para evitar duplicatas
    std::map<std::string, ShaderParameter> paramMap;

    size_t totalParamInfo = 0;
    static std::string lastLoggedPresetForParams;
    std::string currentPresetForParams = m_presetPath;

    for (size_t passIdx = 0; passIdx < m_passes.size(); ++passIdx)
    {
        const auto &passData = m_passes[passIdx];
        size_t passParamCount = passData.parameterInfo.size();
        totalParamInfo += passParamCount;

        for (const auto &paramInfo : passData.parameterInfo)
        {
            const std::string &name = paramInfo.first;
            const ShaderParameterInfo &info = paramInfo.second;

            // Se já existe, manter o primeiro encontrado
            if (paramMap.find(name) == paramMap.end())
            {
                ShaderParameter param;
                param.name = name;
                param.defaultValue = info.defaultValue;
                param.min = info.min;
                param.max = info.max;
                param.step = info.step;
                param.description = info.description;

                // Obter valor atual (customizado, do preset ou padrão)
                auto customIt = m_customParameters.find(name);
                if (customIt != m_customParameters.end())
                {
                    param.value = customIt->second;
                }
                else
                {
                    const auto &presetParams = m_preset.getParameters();
                    auto presetIt = presetParams.find(name);
                    param.value = (presetIt != presetParams.end()) ? presetIt->second : info.defaultValue;
                }

                paramMap[name] = param;
            }
        }
    }

    // Atualizar lastLoggedPresetForParams após processar todos os passes
    if (currentPresetForParams != lastLoggedPresetForParams)
    {
        lastLoggedPresetForParams = currentPresetForParams;
    }

    // Converter map para vector
    for (const auto &pair : paramMap)
    {
        params.push_back(pair.second);
    }

    return params;
}

bool ShaderEngine::setShaderParameter(const std::string &name, float value)
{
    if (!m_shaderActive)
    {
        return false;
    }

    // Verificar se o parâmetro existe
    bool paramExists = false;
    float minVal = 0.0f, maxVal = 1.0f;
    for (const auto &passData : m_passes)
    {
        auto it = passData.parameterInfo.find(name);
        if (it != passData.parameterInfo.end())
        {
            paramExists = true;
            minVal = it->second.min;
            maxVal = it->second.max;
            break;
        }
    }

    if (!paramExists)
    {
        return false;
    }

    // Clamp valor entre min e max
    float clampedValue = std::max(minVal, std::min(maxVal, value));

    // Armazenar valor customizado
    m_customParameters[name] = clampedValue;

    return true;
}
