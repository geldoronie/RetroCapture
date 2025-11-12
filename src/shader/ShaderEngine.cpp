#include "ShaderEngine.h"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
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

    createQuad();
    m_initialized = true;
    LOG_INFO("ShaderEngine inicializado");
    return true;
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

    m_initialized = false;
    LOG_INFO("ShaderEngine encerrado");
}

std::string ShaderEngine::generateDefaultVertexShader()
{
    // Vertex shader compatível com RetroArch
    // Não usar layout(location = X) pois foi removido na conversão
    // Usar vec4 Position para compatibilidade com RetroArch (que usa vec4)
    return R"(#version 330 core
in vec4 Position;
in vec2 TexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = Position;
    vTexCoord = TexCoord;
}
)";
}

bool ShaderEngine::loadShader(const std::string &shaderPath)
{
    if (!m_initialized)
    {
        LOG_ERROR("ShaderEngine não inicializado");
        return false;
    }

    disableShader();

    // Verificar extensão - apenas GLSL é suportado
    std::filesystem::path shaderFilePath(shaderPath);
    std::string extension = shaderFilePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".slang")
    {
        LOG_ERROR("Shaders Slang (.slang) não são suportados. Use shaders GLSL (.glsl) ou presets GLSLP (.glslp)");
        LOG_ERROR("Muitos shaders RetroArch estão disponíveis em formato GLSL na pasta shaders/shaders_glsl/");
        return false;
    }

    if (extension != ".glsl")
    {
        LOG_WARN("Extensão de arquivo não reconhecida: " + extension + ". Esperando .glsl");
    }

    std::ifstream file(shaderPath);
    if (!file.is_open())
    {
        LOG_ERROR("Falha ao abrir shader: " + shaderPath);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fragmentSource = buffer.str();
    file.close();

    // Extrair diretório base do shader para resolver includes
    std::string shaderDir = shaderFilePath.parent_path().string();

    // Processar apenas includes (GLSL já está no formato correto)
    fragmentSource = processIncludes(fragmentSource, shaderDir);

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
        LOG_ERROR("ShaderEngine não inicializado");
        return false;
    }

    disableShader();
    cleanupPresetPasses();

    // Verificar extensão - apenas GLSLP é suportado
    std::filesystem::path presetFilePath(presetPath);
    std::string extension = presetFilePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".slangp")
    {
        LOG_ERROR("Presets Slang (.slangp) não são suportados. Use presets GLSLP (.glslp)");
        LOG_ERROR("Muitos presets RetroArch estão disponíveis em formato GLSLP na pasta shaders/shaders_glsl/");
        return false;
    }

    if (extension != ".glslp" && !extension.empty())
    {
        LOG_WARN("Extensão de preset não reconhecida: " + extension + ". Esperando .glslp");
    }

    if (!m_preset.load(presetPath))
    {
        return false;
    }

    // Carregar texturas de referência
    const auto &textures = m_preset.getTextures();
    LOG_INFO("Carregando " + std::to_string(textures.size()) + " texturas de referência");
    for (const auto &tex : textures)
    {
        LOG_INFO("Carregando textura: " + tex.first + " -> " + tex.second.path);
        if (!loadTextureReference(tex.first, tex.second.path))
        {
            LOG_ERROR("Falha ao carregar textura de referência: " + tex.first);
        }
    }

    m_shaderActive = true;
    LOG_INFO("Preset carregado: " + presetPath);
    return true;
}

bool ShaderEngine::loadPresetPasses()
{
    cleanupPresetPasses();

    const auto &passes = m_preset.getPasses();
    m_passes.resize(passes.size());

    for (size_t i = 0; i < passes.size(); ++i)
    {
        const auto &passInfo = passes[i];
        auto &passData = m_passes[i];
        passData.passInfo = passInfo;

        // DEBUG: Log das configurações do pass
        LOG_INFO("Pass " + std::to_string(i) + " config: scaleTypeX=" + passInfo.scaleTypeX +
                 ", scaleX=" + std::to_string(passInfo.scaleX) +
                 ", scaleTypeY=" + passInfo.scaleTypeY +
                 ", scaleY=" + std::to_string(passInfo.scaleY));

        // Ler shader
        std::ifstream file(passInfo.shaderPath);
        if (!file.is_open())
        {
            LOG_ERROR("Falha ao abrir shader do pass " + std::to_string(i) + ": " + passInfo.shaderPath);
            cleanupPresetPasses();
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string shaderSource = buffer.str();
        file.close();

        // Verificar extensão do shader - apenas GLSL é suportado
        std::filesystem::path shaderPath(passInfo.shaderPath);
        std::string shaderExtension = shaderPath.extension().string();
        std::transform(shaderExtension.begin(), shaderExtension.end(), shaderExtension.begin(), ::tolower);

        if (shaderExtension == ".slang")
        {
            LOG_ERROR("Shaders Slang (.slang) não são suportados no pass " + std::to_string(i));
            LOG_ERROR("Use shaders GLSL (.glsl) ou presets GLSLP (.glslp)");
            cleanupPresetPasses();
            return false;
        }

        // Extrair diretório base do shader para resolver includes
        std::string shaderDir = shaderPath.parent_path().string();

        // Processar includes primeiro
        std::string processedSource = processIncludes(shaderSource, shaderDir);

        // EXTRAIR parâmetros de #pragma parameter ANTES de remover
        // Formato: #pragma parameter paramName "Description" default min max step
        std::map<std::string, float> paramDefaults; // Nome -> valor padrão
        std::regex pragmaParamRegex(R"(#pragma\s+parameter\s+(\w+)\s+"[^"]*"\s+([\d.]+)\s+[\d.]+\s+[\d.]+\s+[\d.]+)");
        auto pragmaBegin = std::sregex_iterator(processedSource.begin(), processedSource.end(), pragmaParamRegex);
        auto pragmaEnd = std::sregex_iterator();
        for (std::sregex_iterator i = pragmaBegin; i != pragmaEnd; ++i)
        {
            std::string paramName = (*i)[1].str();
            std::string defaultValue = (*i)[2].str();
            // Ignorar parâmetros que são apenas labels/títulos (começam com bogus_)
            if (paramName.find("bogus_") == std::string::npos)
            {
                try
                {
                    paramDefaults[paramName] = std::stof(defaultValue);
                }
                catch (...)
                {
                    paramDefaults[paramName] = 0.0f; // Fallback
                }
            }
        }

        // Remover #pragma parameter (substituir por espaços, como RetroArch faz)
        // Isso evita problemas com caracteres especiais em strings
        size_t pragmaPos = processedSource.find("#pragma parameter");
        while (pragmaPos != std::string::npos)
        {
            size_t lineEnd = processedSource.find('\n', pragmaPos);
            if (lineEnd == std::string::npos)
                lineEnd = processedSource.length();

            // Substituir por espaços
            for (size_t j = pragmaPos; j < lineEnd; ++j)
                processedSource[j] = ' ';

            pragmaPos = processedSource.find("#pragma parameter", lineEnd);
        }

        // Armazenar valores padrão para injeção em setupUniforms
        passData.extractedParameters = paramDefaults;

        // SIMPLIFICADO: Usar o MESMO código fonte para vertex e fragment
        // RetroArch faz isso - adiciona defines diferentes antes do código
        // O shader GLSL usa #if defined(VERTEX) / #elif defined(FRAGMENT) internamente
        std::string vertexSource;
        std::string fragmentSource;

        // SIMPLIFICADO: Como RetroArch faz - usar o MESMO código fonte para ambos
        // Adicionar defines diferentes antes do código
        // O shader GLSL usa #if defined(VERTEX) / #elif defined(FRAGMENT) internamente

        // Extrair #version se existir (deve estar na primeira linha)
        std::string versionLine = "";
        std::string codeAfterVersion = processedSource;

        std::regex versionRegex(R"(#version\s+\d+[^\n]*)");
        std::smatch versionMatch;
        if (std::regex_search(processedSource, versionMatch, versionRegex))
        {
            versionLine = versionMatch.str() + "\n";
            codeAfterVersion = std::regex_replace(processedSource, versionRegex, "", std::regex_constants::format_first_only);
        }
        else
        {
            // Adicionar #version 330 se não existir
            versionLine = "#version 330\n";
        }

        // Construir shaders: version + extension + define + código completo
        // RetroArch adiciona: "#define VERTEX\n#define PARAMETER_UNIFORM\n" para vertex
        // e "#define FRAGMENT\n#define PARAMETER_UNIFORM\n" para fragment
        // IMPORTANTE: RetroArch também adiciona defines para compatibilidade
        // Adicionar extensão para inicialização estilo C (GL_ARB_shading_language_420pack)
        // Isso permite inicialização de arrays e estruturas estilo C
        std::string extensionLine = "#extension GL_ARB_shading_language_420pack : require\n";
        vertexSource = versionLine + extensionLine + "#define VERTEX\n#define PARAMETER_UNIFORM\n" + codeAfterVersion;
        fragmentSource = versionLine + extensionLine + "#define FRAGMENT\n#define PARAMETER_UNIFORM\n" + codeAfterVersion;

        // Compilar shaders
        if (!compileShader(vertexSource, GL_VERTEX_SHADER, passData.vertexShader))
        {
            LOG_ERROR("Falha ao compilar vertex shader do pass " + std::to_string(i));
            cleanupPresetPasses();
            return false;
        }

        if (!compileShader(fragmentSource, GL_FRAGMENT_SHADER, passData.fragmentShader))
        {
            LOG_ERROR("Falha ao compilar fragment shader do pass " + std::to_string(i));
            glDeleteShader(passData.vertexShader);
            cleanupPresetPasses();
            return false;
        }

        // Criar e linkar programa para este pass (cada pass precisa de seu próprio programa)
        GLuint program = glCreateProgram();
        if (program == 0)
        {
            LOG_ERROR("Falha ao criar shader program do pass " + std::to_string(i));
            cleanupPresetPasses();
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
            LOG_ERROR("Erro ao linkar shader program do pass " + std::to_string(i) + ": " + std::string(infoLog));
            glDeleteProgram(program);
            glDeleteShader(passData.vertexShader);
            glDeleteShader(passData.fragmentShader);
            cleanupPresetPasses();
            return false;
        }

        passData.program = program;
        passData.floatFramebuffer = passInfo.floatFramebuffer;
    }

    LOG_INFO("Preset passes carregados: " + std::to_string(m_passes.size()));
    return true;
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
        if (m_passes.empty())
        {
            if (!loadPresetPasses())
            {
                return inputTexture;
            }
        }

        m_sourceWidth = width;
        m_sourceHeight = height;
        // IMPORTANTE: viewportWidth e viewportHeight devem ser as dimensões da janela,
        // não as dimensões da entrada. Eles devem ser atualizados via setViewport()
        // antes de chamar applyShader(). Se não foram atualizados, usar as dimensões da entrada como fallback.
        // Mas é melhor sempre atualizar via setViewport() antes de aplicar o shader.

        GLuint currentTexture = inputTexture;
        uint32_t currentWidth = width;
        uint32_t currentHeight = height;

        // IMPORTANTE: Guardar a textura original para passes que precisam dela (como hq2x)
        // Alguns shaders (como hqx-pass2) precisam tanto da saída do pass anterior quanto da entrada original
        GLuint originalTexture = inputTexture;

        // IMPORTANTE: Para motion blur, precisamos manter um histórico de frames anteriores
        // O histórico deve conter frames JÁ PROCESSADOS (saída do shader), não a entrada
        // Por isso, não inicializamos aqui - o histórico será preenchido após renderizar

        // DEBUG: Verificar se a textura de entrada é válida
        if (inputTexture == 0)
        {
            LOG_ERROR("applyShader: Textura de entrada inválida (0)!");
            return 0;
        }
        LOG_INFO("applyShader: inputTexture=" + std::to_string(inputTexture) + " (" +
                 std::to_string(width) + "x" + std::to_string(height) + ")");

        // IMPORTANTE: Incrementar FrameCount apenas uma vez por frame (não por pass)
        // Isso permite que shaders animados funcionem corretamente
        // FrameCount é usado por shaders para criar animações baseadas no número de frames
        m_frameCount += 1.0f;
        m_time += 0.016f; // ~60fps (aproximado, será ajustado pelo tempo real se necessário)

        // Aplicar cada pass
        for (size_t i = 0; i < m_passes.size(); ++i)
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

            // DEBUG: Log das dimensões calculadas
            if (i == 0 || i == m_passes.size() - 1)
            {
                LOG_INFO("Pass " + std::to_string(i) + " escala: " + scaleTypeX + "x" +
                         std::to_string(scaleX) + ", " + scaleTypeY + "x" +
                         std::to_string(scaleY) + " -> " + std::to_string(outputWidth) + "x" +
                         std::to_string(outputHeight));
            }

            // Criar/atualizar framebuffer se necessário
            if (pass.framebuffer == 0 || pass.width != outputWidth || pass.height != outputHeight)
            {
                cleanupFramebuffer(pass.framebuffer, pass.texture);
                createFramebuffer(outputWidth, outputHeight, pass.passInfo.floatFramebuffer,
                                  pass.framebuffer, pass.texture, pass.passInfo.srgbFramebuffer);
                pass.width = outputWidth;
                pass.height = outputHeight;

                // DEBUG: Log do primeiro pass
                if (i == 0)
                {
                    LOG_INFO("Pass 0 framebuffer criado: fb=" + std::to_string(pass.framebuffer) + ", tex=" + std::to_string(pass.texture));
                }
            }

            // Bind framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, pass.framebuffer);

            glViewport(0, 0, outputWidth, outputHeight);

            // DEBUG: Log do viewport do primeiro pass
            if (i == 0)
            {
                LOG_INFO("Pass 0 viewport: " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
                         " (scaleType: " + scaleTypeX + "/" + scaleTypeY + ", viewport: " +
                         std::to_string(m_viewportWidth) + "x" + std::to_string(m_viewportHeight) + ")");
            }

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
            // IMPORTANTE: Desabilitar blending, culling e depth test (como RetroArch faz)
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);

            // Usar shader program
            glUseProgram(pass.program);

            // Verificar se o programa é válido
            if (pass.program == 0)
            {
                LOG_ERROR("Programa de shader inválido no pass " + std::to_string(i));
                continue; // Pular este pass se o programa é inválido
            }

            // DEBUG: Log do programa sendo usado
            if (i == 0)
            {
                LOG_INFO("Pass 0: Usando programa de shader: " + std::to_string(pass.program));
            }

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
                LOG_ERROR("Textura de entrada inválida no pass " + std::to_string(i));
                continue; // Pular este pass se não há textura válida
            }

            // IMPORTANTE: Aplicar filter_linear# e wrap_mode# na textura de entrada
            // RetroArch aplica essas configurações quando faz bind da textura
            // Isso é crítico para shaders como motionblur-simple e crt-geom que precisam de GL_NEAREST
            // A textura já está bindada (glBindTexture acima), então apenas aplicar parâmetros
            bool filterLinear = passInfo.filterLinear;
            std::string wrapMode = passInfo.wrapMode;
            bool mipmapInput = passInfo.mipmapInput;

            // DEBUG: Log das configurações aplicadas no primeiro pass
            if (i == 0)
            {
                LOG_INFO("Pass 0: Aplicando configurações de textura: filter=" + (filterLinear ? std::string("linear") : std::string("nearest")) +
                         ", wrap=" + wrapMode + ", mipmap=" + (mipmapInput ? std::string("yes") : std::string("no")));
            }

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
                LOG_INFO("Pass 0: inputTexture=" + std::to_string(currentTexture) +
                         " (" + std::to_string(currentWidth) + "x" + std::to_string(currentHeight) +
                         "), output será " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
                         ", scaleType: " + scaleTypeX + "/" + scaleTypeY);
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
                    if (i == 0)
                    {
                        LOG_INFO("Pass 0: Uniform de textura de entrada encontrado: '" + std::string(name) + 
                                 "' vinculado à unidade 0, textura ID=" + std::to_string(currentTexture));
                    }
                    break; // Encontrou um, não precisa tentar os outros
                }
                else if (i == 0)
                {
                    // Log apenas no primeiro pass para debug
                    // LOG_INFO("Pass 0: Uniform '" + std::string(name) + "' não encontrado");
                }
            }

            // Se nenhum uniform foi encontrado, logar aviso apenas no primeiro pass
            if (!textureBound && i == 0)
            {
                LOG_WARN("Nenhum uniform de textura encontrado no pass 0 (Texture/Source/Input/s_p/etc)");
                LOG_WARN("Isso pode causar tela preta - o shader precisa de um uniform de textura de entrada");
                LOG_WARN("Pass 0: Programa de shader: " + std::to_string(pass.program));
                
                // Tentar listar todos os uniforms do programa (debug)
                GLint numUniforms = 0;
                // glGetProgramiv(pass.program, GL_ACTIVE_UNIFORMS, &numUniforms);
                // LOG_INFO("Pass 0: Número de uniforms ativos: " + std::to_string(numUniforms));
            }

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
                                    LOG_INFO("Pass 0: Uniform '" + name + "' vinculado ao histórico de frame " + std::to_string(prevIdx) + " (unidade " + std::to_string(texUnit) + ")");
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
                LOG_INFO("Pass " + std::to_string(i) + ": Uniform 'OrigTexture' vinculado à textura original (unidade " + std::to_string(texUnit) + ")");
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
                    LOG_INFO("Pass " + std::to_string(i) + ": Textura de referência '" + texRef.first +
                             "' (texture ID=" + std::to_string(texRef.second) + ") vinculada na unidade " + std::to_string(texUnit));
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

            // DEBUG: Log antes de renderizar
            if (i == 0)
            {
                LOG_INFO("Pass 0: Renderizando para framebuffer " + std::to_string(pass.framebuffer) + 
                         ", textura de entrada ID=" + std::to_string(currentTexture));
            }

            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

            glBindVertexArray(0);

            // IMPORTANTE: Não precisamos desabilitar blending aqui porque não habilitamos
            // O blending será aplicado apenas na renderização final na janela

            // DEBUG: Verificar se houve erro OpenGL (apenas no primeiro pass)
            if (i == 0)
            {
                // Verificar se o framebuffer está completo
                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE)
                {
                    LOG_ERROR("Pass 0: Framebuffer incompleto! Status: " + std::to_string(status));
                }
                else
                {
                    LOG_INFO("Pass 0: Framebuffer completo e válido");
                }
            }

            // Próximo pass usa a saída deste
            currentTexture = pass.texture;
            currentWidth = outputWidth;
            currentHeight = outputHeight;

            // DEBUG: Log do primeiro e último pass
            if (i == 0)
            {
                LOG_INFO("Pass 0 renderizado: input=" + std::to_string(inputTexture) + " (" +
                         std::to_string(currentWidth) + "x" + std::to_string(currentHeight) + "), output=" +
                         std::to_string(currentTexture) + " (" + std::to_string(outputWidth) + "x" +
                         std::to_string(outputHeight) + ")");

                // Verificar se a textura de saída é válida
                if (currentTexture == 0)
                {
                    LOG_ERROR("Pass 0: Textura de saída inválida (0)!");
                }
            }
            if (i == m_passes.size() - 1)
            {
                LOG_INFO("Último pass (" + std::to_string(i) + "): outputTexture=" + std::to_string(currentTexture) +
                         " (" + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) + ")");
            }
        }

        // Desvincular framebuffer após todos os passes
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        // IMPORTANTE: Resetar viewport para um tamanho grande após os passes
        // Isso garante que a renderização final use o viewport correto
        // O viewport será configurado novamente em Application::run() antes de renderizar
        // Mas é bom garantir que não fique com um viewport pequeno
        // Usar um tamanho grande padrão (1920x1080) para garantir que não fique pequeno
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
                // Criar framebuffer temporário para a textura do histórico
                GLuint copyFramebuffer = 0;
                glGenFramebuffers(1, &copyFramebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, copyFramebuffer);
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
                        LOG_WARN("Não foi possível copiar frame para histórico (sem shader program)");
                    }
                }

                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &copyFramebuffer);
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
                LOG_INFO("Primeiro frame copiado para histórico (textura dedicada)");
            }
            else if (m_frameHistory.size() == MAX_FRAME_HISTORY)
            {
                LOG_INFO("Histórico completo: " + std::to_string(MAX_FRAME_HISTORY) + " frames (texturas dedicadas)");
            }
        }

        // DEBUG: Log final antes de retornar
        LOG_INFO("applyShader: Retornando textura final: " + std::to_string(currentTexture) +
                 " (" + std::to_string(currentWidth) + "x" + std::to_string(currentHeight) + ")");

        if (currentTexture == 0)
        {
            LOG_ERROR("applyShader: Textura final inválida (0)! Retornando textura de entrada original.");
            return inputTexture;
        }

        return currentTexture;
    }
    else
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

void ShaderEngine::setupUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight,
                                 uint32_t outputWidth, uint32_t outputHeight)
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
    // IMPORTANTE: Alguns shaders usam vec4, outros usam vec2
    // O problema é que não podemos saber o tipo sem glGetActiveUniform
    // Vamos tentar configurar como vec2 primeiro (muitos shaders modernos usam vec2)
    // Se o shader espera vec4, o OpenGL pode aceitar vec2 nos primeiros componentes
    // Mas o ideal é verificar o tipo do uniform
    loc = getUniformLocation(program, "OutputSize");
    if (loc >= 0)
    {
        // IMPORTANTE: Tentar como vec2 primeiro (shaders como crt-geom usam vec2)
        // Se o shader espera vec4, vamos configurar como vec4 depois
        // Mas na prática, vamos tentar vec2 primeiro pois muitos shaders modernos usam vec2
        glUniform2f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
        if (passIndex == 0)
        {
            LOG_INFO("Pass 0: OutputSize configurado como vec2: " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight));
        }
        
        // Se o shader espera vec4, podemos tentar configurar como vec4 também
        // Mas isso pode causar problemas se o shader espera vec2
        // Por enquanto, vamos usar vec2 apenas
        // Se necessário, podemos adicionar verificação de tipo usando glGetActiveUniform
    }
    // Nota: Se OutputSize não for encontrado, pode ser que o shader não o use
    // ou que ele tenha sido otimizado fora pelo compilador GLSL
    // Não é necessariamente um erro, apenas um aviso informativo

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
        glUniform1f(loc, frameCountValue);
    }

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

    // Texturas de histórico (history buffers) - valores dummy
    // OriginalHistorySize0-7 (frames anteriores)
    // Nota: não implementado ainda, mas precisamos declarar para evitar erros
    for (int i = 0; i <= 7; ++i)
    {
        std::string historyName = "OriginalHistorySize" + std::to_string(i);
        loc = getUniformLocation(program, historyName);
        if (loc >= 0)
        {
            // Usar mesmas dimensões do input atual (já que não temos histórico real)
            glUniform4f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight),
                        1.0f / static_cast<float>(inputWidth), 1.0f / static_cast<float>(inputHeight));
        }
    }

    // Parâmetros extraídos de #pragma parameter (injetados pelo RetroArch)
    // Esses são parâmetros configuráveis que o RetroArch injeta como uniforms
    if (passIndex < m_passes.size())
    {
        const auto &extractedParams = m_passes[passIndex].extractedParameters;
        if (passIndex == 0 && !extractedParams.empty())
        {
            LOG_INFO("Pass 0: " + std::to_string(extractedParams.size()) + " parâmetros extraídos de #pragma parameter");
        }
        for (const auto &param : extractedParams)
        {
            loc = getUniformLocation(program, param.first);
            if (loc >= 0)
            {
                // Verificar se há valor customizado no preset, senão usar valor padrão do #pragma parameter
                const auto &presetParams = m_preset.getParameters();
                auto presetIt = presetParams.find(param.first);
                float value = (presetIt != presetParams.end()) ? presetIt->second : param.second;
                glUniform1f(loc, value);
                if (passIndex == 0)
                {
                    LOG_INFO("Pass 0: Parâmetro '" + param.first + "' = " + std::to_string(value));
                }
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
    loc = getUniformLocation(program, "TextureSize");
    if (loc >= 0)
    {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
        if (passIndex == 0)
        {
            LOG_INFO("Pass 0: TextureSize configurado como vec2: " + std::to_string(inputWidth) + "x" + std::to_string(inputHeight));
        }
    }
    // Nota: Se TextureSize não for encontrado, pode ser que o shader não o use
    // ou que ele tenha sido otimizado fora pelo compilador GLSL
    // Não é necessariamente um erro, apenas um aviso informativo

    // InputSize (vec2 alternativo)
    loc = getUniformLocation(program, "InputSize");
    if (loc >= 0)
    {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
        if (passIndex == 0)
        {
            LOG_INFO("Pass 0: InputSize configurado como vec2: " + std::to_string(inputWidth) + "x" + std::to_string(inputHeight));
        }
    }
    // Nota: Se InputSize não for encontrado, pode ser que o shader não o use
    // ou que ele tenha sido otimizado fora pelo compilador GLSL
    // Não é necessariamente um erro, apenas um aviso informativo

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

bool ShaderEngine::loadTextureReference(const std::string &name, const std::string &path)
{
    // Verificar se a textura já foi carregada
    if (m_textureReferences.find(name) != m_textureReferences.end())
    {
        LOG_WARN("Textura '" + name + "' já foi carregada");
        return true;
    }

    // Verificar se o arquivo existe
    if (!std::filesystem::exists(path))
    {
        LOG_ERROR("Arquivo de textura não encontrado: " + path);
        return false;
    }

    // Abrir arquivo PNG
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        LOG_ERROR("Falha ao abrir arquivo de textura: " + path);
        return false;
    }

    // Verificar assinatura PNG
    png_byte header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
    {
        fclose(fp);
        LOG_ERROR("Arquivo não é PNG válido: " + path);
        return false;
    }

    // Inicializar estruturas libpng
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        fclose(fp);
        LOG_ERROR("Falha ao criar estrutura de leitura PNG");
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(fp);
        LOG_ERROR("Falha ao criar estrutura de informação PNG");
        return false;
    }

    // Configurar tratamento de erros
    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        LOG_ERROR("Erro ao processar PNG: " + path);
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

    LOG_INFO("Textura '" + name + "' carregada com sucesso: " + std::to_string(width) + "x" + std::to_string(height) +
             " (filter=" + (filterLinear ? "linear" : "nearest") + ", wrap=" + wrapMode + ", mipmap=" + (mipmap ? "yes" : "no") + ")");

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
    }
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
        LOG_ERROR("Erro ao compilar shader: " + std::string(infoLog));
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
        LOG_ERROR("Falha ao criar shader program");
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
        LOG_ERROR("Erro ao linkar shader program: " + std::string(infoLog));
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
        return false;
    }

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
        LOG_ERROR("Framebuffer incompleto! Status: " + std::to_string(status));
        cleanupFramebuffer(fb, tex);
        return;
    }

    // DEBUG: Log do framebuffer criado
    LOG_INFO("Framebuffer criado: fb=" + std::to_string(fb) + ", tex=" + std::to_string(tex) + ", size=" + std::to_string(width) + "x" + std::to_string(height));

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

std::string ShaderEngine::convertSlangToGLSL(const std::string &slangSource, bool isVertex, const std::string &basePath)
{
    std::string result = slangSource;

    // Processar #include primeiro (antes de outras conversões)
    result = processIncludes(result, basePath);

    // EXTRAIR parâmetros de #pragma parameter ANTES de qualquer outra conversão
    std::set<std::string> pragmaParams;
    std::regex pragmaParamRegex(R"(#pragma\s+parameter\s+(\w+))");
    auto pragmaBegin = std::sregex_iterator(result.begin(), result.end(), pragmaParamRegex);
    auto pragmaEnd = std::sregex_iterator();
    for (std::sregex_iterator i = pragmaBegin; i != pragmaEnd; ++i)
    {
        std::string paramName = (*i)[1].str();
        // Ignorar parâmetros que são apenas labels/títulos (começam com bogus_)
        if (paramName.find("bogus_") == std::string::npos)
        {
            pragmaParams.insert(paramName);
        }
    }

    // Remover TODAS as linhas #pragma parameter
    result = std::regex_replace(result, std::regex(R"(#pragma\s+parameter[^\n]*\n)"), "");

    // Substituir #version 450 por #version 330
    result = std::regex_replace(result, std::regex("#version\\s+450"), "#version 330");

    // Converter swizzles numéricos para construção vec explícita
    // GLSL 3.30 pode não suportar 0.0.xxx, então convertemos para vec3(0.0)
    // IMPORTANTE: NÃO converter em expressões aritméticas como pow(x, 0.70.xxx-0.325*sat)
    // porque isso criaria pow(x, vec3(0.70)-0.325*sat) que é inválido

    // Estratégia: Converter apenas swizzles que estão isolados ou em contextos seguros

    // Padrão 1: Atribuição direta com ponto e vírgula (vec3 x = 0.0.xxx;)
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.xxx\s*;)"), "= vec3($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.xxxx\s*;)"), "= vec4($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.yyy\s*;)"), "= vec3($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.yyyy\s*;)"), "= vec4($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.zzz\s*;)"), "= vec3($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.zzzz\s*;)"), "= vec4($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.www\s*;)"), "= vec3($1);");
    result = std::regex_replace(result, std::regex(R"(=\s*(\d+\.\d+)\.wwww\s*;)"), "= vec4($1);");

    // Padrão 2: Em mix() quando está isolado (mix(1.0.xxx + scans, 1.0.xxx, c))
    // Converter quando está após operador + ou , e antes de operador ou )
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.xxx\s*([+\-*/\),]))"), "$1 vec3($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.xxxx\s*([+\-*/\),]))"), "$1 vec4($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.yyy\s*([+\-*/\),]))"), "$1 vec3($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.yyyy\s*([+\-*/\),]))"), "$1 vec4($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.zzz\s*([+\-*/\),]))"), "$1 vec3($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.zzzz\s*([+\-*/\),]))"), "$1 vec4($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.www\s*([+\-*/\),]))"), "$1 vec3($2) $3");
    result = std::regex_replace(result, std::regex(R"(([+\s,\(])\s*(\d+\.\d+)\.wwww\s*([+\-*/\),]))"), "$1 vec4($2) $3");

    // Padrão 3: Em operador ternário (clips > 0.0) ? w1 : 1.0.xxx;
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.xxx\s*;)"), ": vec3($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.xxxx\s*;)"), ": vec4($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.yyy\s*;)"), ": vec3($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.yyyy\s*;)"), ": vec4($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.zzz\s*;)"), ": vec3($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.zzzz\s*;)"), ": vec4($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.www\s*;)"), ": vec3($1);");
    result = std::regex_replace(result, std::regex(R"(:\s*(\d+\.\d+)\.wwww\s*;)"), ": vec4($1);");

    // NOTA: NÃO converter swizzles em expressões aritméticas complexas
    // (ex: pow(x, 0.70.xxx-0.325*sat)) porque isso criaria código inválido
    // GLSL 3.30 pode suportar swizzles numéricos em alguns contextos
    // Se não suportar, o compilador vai reportar o erro exato

    // Converter push_constant uniform block para uniforms individuais
    // layout(push_constant) uniform Push { vec4 SourceSize; vec4 OriginalSize; ... } params;
    // vira: uniform vec4 SourceSize; uniform vec4 OriginalSize; ...
    // E substituir params.SourceSize por SourceSize, etc.

    // Primeiro, extrair campos do uniform block e criar uniforms individuais
    std::regex pushConstantRegex(R"(layout\s*\(\s*push_constant\s*\)\s*uniform\s+Push\s*\{([^}]+)\}\s*params\s*;)");
    std::smatch match;

    if (std::regex_search(result, match, pushConstantRegex))
    {
        std::string blockContent = match[1].str();
        std::ostringstream uniforms;

        // Extrair cada campo do block
        std::regex fieldRegex(R"(\s*(\w+)\s+(\w+)\s*;)");
        std::sregex_iterator iter(blockContent.begin(), blockContent.end(), fieldRegex);
        std::sregex_iterator end;

        for (; iter != end; ++iter)
        {
            std::string type = (*iter)[1].str();
            std::string name = (*iter)[2].str();
            uniforms << "uniform " << type << " " << name << ";\n";
        }

        // Substituir todos os params.X por X (fazer isso globalmente após criar os uniforms)
        // Isso garante que funcione também em arquivos incluídos
        std::regex fieldRegex2(R"(\s*(\w+)\s+(\w+)\s*;)");
        std::sregex_iterator iter2(blockContent.begin(), blockContent.end(), fieldRegex2);
        std::sregex_iterator end2;

        for (; iter2 != end2; ++iter2)
        {
            std::string name = (*iter2)[2].str();
            // Substituir params.name por name (globalmente, incluindo em arquivos incluídos)
            // Usar word boundary para evitar substituir partes de outras palavras
            std::string replacePattern = "params\\." + name + "\\b";
            result = std::regex_replace(result, std::regex(replacePattern), name);
        }

        // Remover o uniform block original e adicionar uniforms individuais
        result = std::regex_replace(result, pushConstantRegex, uniforms.str());
    }
    else
    {
        // Fallback: apenas remover push_constant
        result = std::regex_replace(result, std::regex("layout\\(push_constant\\)\\s+uniform"), "uniform");
    }

    // Remover set = X, binding = X dos layouts
    result = std::regex_replace(result, std::regex("set\\s*=\\s*\\d+"), "");
    result = std::regex_replace(result, std::regex("binding\\s*=\\s*\\d+"), "");
    result = std::regex_replace(result, std::regex(",\\s*,"), ",");
    result = std::regex_replace(result, std::regex("layout\\(\\s*,\\s*"), "layout(");
    result = std::regex_replace(result, std::regex("layout\\(\\s*\\)"), "");

    // Remover layout(std140, set = 0, binding = 0) deixando apenas std140 se existir
    result = std::regex_replace(result, std::regex("layout\\(std140[^)]*\\)"), "layout(std140)");

    // EXTRAIR parâmetros do bloco UBO antes de removê-lo
    std::set<std::string> uboParams;
    std::regex uboRegex(R"(layout\s*\([^)]*\)\s*uniform\s+UBO\s*\{([^}]*)\}\s*global\s*;)");
    std::smatch uboMatch;
    if (std::regex_search(result, uboMatch, uboRegex))
    {
        std::string uboContent = uboMatch[1].str();
        // Extrair declarações de variáveis do UBO (exceto MVP e tamanhos específicos do RetroArch)
        std::regex paramRegex(R"((float|uint|int)\s+(\w+)(?:\s*,\s*(\w+))*\s*;)");
        std::sregex_iterator paramsBegin(uboContent.begin(), uboContent.end(), paramRegex);
        std::sregex_iterator paramsEnd;

        for (std::sregex_iterator i = paramsBegin; i != paramsEnd; ++i)
        {
            std::string type = (*i)[1].str();
            std::string paramName = (*i)[2].str();

            // Lista de nomes padrão do RetroArch que devemos ignorar
            bool isBuiltin = (paramName == "MVP" || paramName == "SourceSize" ||
                              paramName == "OutputSize" || paramName == "OriginalSize" ||
                              paramName == "FrameCount" ||
                              paramName.find("OriginalHistorySize") == 0 ||
                              paramName.find("PassOutputSize") == 0 ||
                              paramName.find("PassFeedbackSize") == 0);

            if (!isBuiltin)
            {
                uboParams.insert(paramName);
                // Se houver múltiplos parâmetros na mesma linha (ex: AS, asat)
                if (i->size() > 3 && (*i)[3].length() > 0)
                {
                    uboParams.insert((*i)[3].str());
                }
            }
        }
    }

    // Remover uniform block UBO (se existir)
    result = std::regex_replace(result, uboRegex, "");

    // Converter global.MVP para Position diretamente
    // RetroArch usa global.MVP * Position, mas Position já está em clip space
    result = std::regex_replace(result, std::regex(R"(global\.MVP\s*\*\s*Position)"), "Position");

    // Remover outras referências a global.
    result = std::regex_replace(result, std::regex(R"(global\.)"), "");

    // IMPORTANTE: Substituir params.X por X DEPOIS de todas as conversões
    // Isso garante que funcione mesmo em arquivos incluídos

    // PRIMEIRO: Processar #define que usam params.X
    // Exemplo: #define BLURSCALEX params.BLURSCALEX
    // Coletar todos os parâmetros customizados que precisam ser uniforms
    std::set<std::string> customParams;
    std::regex defineRegex(R"(#define\s+(\w+)\s+params\.(\w+))");
    std::smatch defineMatch;
    std::string processedDefines = result;

    // Encontrar todos os #define que usam params.X
    while (std::regex_search(processedDefines, defineMatch, defineRegex))
    {
        std::string defineName = defineMatch[1].str();
        std::string paramName = defineMatch[2].str();

        // Adicionar à lista de parâmetros customizados
        customParams.insert(paramName);

        // Se o nome do define é igual ao parâmetro, remover o define
        // (não é necessário, pois params.X será substituído por X diretamente)
        if (defineName == paramName)
        {
            processedDefines = std::regex_replace(processedDefines, defineRegex, "", std::regex_constants::format_first_only);
        }
        else
        {
            // Se são diferentes, manter o define mas substituir params.X por X
            std::string replacement = "#define " + defineName + " " + paramName;
            processedDefines = std::regex_replace(processedDefines, defineRegex, replacement, std::regex_constants::format_first_only);
        }
    }
    result = processedDefines;

    // AGORA: Substituir TODOS os params.X por X (genérico, pega tudo)
    // Isso deve ser feito DEPOIS de processar os #define
    result = std::regex_replace(result, std::regex(R"(params\.(\w+))"), "$1");

    // Agora verificar se os uniforms já foram definidos
    // Verificar tanto vec4 quanto vec2 (alguns shaders usam vec2)
    bool hasSourceSize = (result.find("uniform vec4 SourceSize") != std::string::npos ||
                          result.find("uniform vec2 SourceSize") != std::string::npos ||
                          result.find("uniform COMPAT_PRECISION vec4 SourceSize") != std::string::npos ||
                          result.find("uniform COMPAT_PRECISION vec2 SourceSize") != std::string::npos);
    bool hasOriginalSize = (result.find("uniform vec4 OriginalSize") != std::string::npos ||
                            result.find("uniform vec2 OriginalSize") != std::string::npos ||
                            result.find("uniform COMPAT_PRECISION vec4 OriginalSize") != std::string::npos ||
                            result.find("uniform COMPAT_PRECISION vec2 OriginalSize") != std::string::npos);
    bool hasOutputSize = (result.find("uniform vec4 OutputSize") != std::string::npos ||
                          result.find("uniform vec2 OutputSize") != std::string::npos ||
                          result.find("uniform COMPAT_PRECISION vec4 OutputSize") != std::string::npos ||
                          result.find("uniform COMPAT_PRECISION vec2 OutputSize") != std::string::npos);
    bool hasInputSize = (result.find("uniform vec4 InputSize") != std::string::npos ||
                         result.find("uniform vec2 InputSize") != std::string::npos ||
                         result.find("uniform COMPAT_PRECISION vec4 InputSize") != std::string::npos ||
                         result.find("uniform COMPAT_PRECISION vec2 InputSize") != std::string::npos);
    bool hasTextureSize = (result.find("uniform vec4 TextureSize") != std::string::npos ||
                           result.find("uniform vec2 TextureSize") != std::string::npos ||
                           result.find("uniform COMPAT_PRECISION vec4 TextureSize") != std::string::npos ||
                           result.find("uniform COMPAT_PRECISION vec2 TextureSize") != std::string::npos);
    bool hasFrameCount = (result.find("uniform float FrameCount") != std::string::npos ||
                          result.find("uniform uint FrameCount") != std::string::npos ||
                          result.find("uniform int FrameCount") != std::string::npos ||
                          result.find("uniform COMPAT_PRECISION int FrameCount") != std::string::npos);

    // Se não foram definidos mas são usados, adicionar as definições
    // Preferir vec2 para OutputSize e InputSize (mais comum em shaders modernos)
    std::ostringstream missingUniforms;
    if (!hasSourceSize && result.find("SourceSize") != std::string::npos)
    {
        missingUniforms << "uniform vec4 SourceSize;\n";
    }
    if (!hasOriginalSize && result.find("OriginalSize") != std::string::npos)
    {
        missingUniforms << "uniform vec4 OriginalSize;\n";
    }
    if (!hasOutputSize && result.find("OutputSize") != std::string::npos)
    {
        // Preferir vec2 pois muitos shaders modernos usam vec2
        missingUniforms << "uniform vec2 OutputSize;\n";
    }
    if (!hasInputSize && result.find("InputSize") != std::string::npos)
    {
        // Preferir vec2 pois muitos shaders modernos usam vec2
        missingUniforms << "uniform vec2 InputSize;\n";
    }
    if (!hasTextureSize && result.find("TextureSize") != std::string::npos)
    {
        // Preferir vec2 pois muitos shaders modernos usam vec2
        missingUniforms << "uniform vec2 TextureSize;\n";
    }
    // FrameCount é SEMPRE necessário, adicionar se não existe
    if (!hasFrameCount)
    {
        missingUniforms << "uniform float FrameCount;\n";
    }

    // Combinar todos os parâmetros customizados (evitando duplicatas)
    std::set<std::string> allCustomParams;
    allCustomParams.insert(customParams.begin(), customParams.end());
    allCustomParams.insert(uboParams.begin(), uboParams.end());
    allCustomParams.insert(pragmaParams.begin(), pragmaParams.end());

    // Remover palavras que são swizzles GLSL válidos para evitar conflitos
    // (xxx, xxxx, rgb, rgba, xyzw, etc)
    std::set<std::string> glslSwizzles = {
        "x", "y", "z", "w",
        "r", "g", "b", "a",
        "s", "t", "p", "q",
        "xx", "xy", "xz", "xw", "xxx", "xxxx",
        "yy", "yx", "yz", "yw", "yyy", "yyyy",
        "zz", "zx", "zy", "zw", "zzz", "zzzz",
        "ww", "wx", "wy", "wz", "www", "wwww",
        "rr", "rg", "rb", "ra", "rrr", "rrrr",
        "gg", "gr", "gb", "ga", "ggg", "gggg",
        "bb", "br", "bg", "ba", "bbb", "bbbb",
        "aa", "ar", "ag", "ab", "aaa", "aaaa",
        "rgb", "rgba", "bgr", "bgra",
        "xyz", "xyzw", "zyx", "zyxw"};

    for (const auto &swizzle : glslSwizzles)
    {
        allCustomParams.erase(swizzle);
    }

    // DEBUG: Listar todos os parâmetros encontrados
    if (!allCustomParams.empty())
    {
        std::string paramsList;
        for (const auto &p : allCustomParams)
        {
            paramsList += p + ", ";
        }
        LOG_INFO("Parâmetros customizados encontrados: " + paramsList);
    }

    // Adicionar uniforms customizados (sem duplicatas)
    // Se o parâmetro é uma variável local, precisamos adicionar o uniform com prefixo
    // para evitar conflito (ex: internal_res → uniform float u_internal_res)
    std::set<std::string> localVarParams;

    for (const auto &param : allCustomParams)
    {
        std::string uniformDecl = "uniform float " + param;

        // Verificar se é declarado como variável local
        // Procurar por padrões: "float X;" ou "float X," ou "float X ="
        std::regex localVarRegex("\\bfloat\\s+" + param + "\\s*[,;=]");
        bool isLocalVariable = std::regex_search(result, localVarRegex);

        bool isUniformDeclared = (result.find(uniformDecl) != std::string::npos);
        bool isUsed = (result.find(param) != std::string::npos);

        if (!isUniformDeclared && isUsed)
        {
            if (isLocalVariable)
            {
                // Variável local: adicionar uniform com prefixo u_
                // O shader usa params.X que foi substituído, e também declara float X
                missingUniforms << "uniform float u_" << param << ";\n";
                localVarParams.insert(param);
            }
            else
            {
                // Não é variável local: adicionar uniform normal
                missingUniforms << uniformDecl << ";\n";
            }
        }
    }

    // Ajustar substituições para parâmetros que são variáveis locais
    // Substituir X por u_X para evitar conflito com float X = ...
    for (const auto &param : localVarParams)
    {
        LOG_INFO("Processando variável local com prefixo: " + param);

        // Substituir X por u_X em todo o shader (exceto na declaração float X e swizzles .xxx)
        std::string pattern = "\\b" + param + "\\b";
        std::regex paramRegex(pattern);

        std::string temp;
        auto words_begin = std::sregex_iterator(result.begin(), result.end(), paramRegex);
        auto words_end = std::sregex_iterator();

        size_t lastPos = 0;
        for (std::sregex_iterator i = words_begin; i != words_end; ++i)
        {
            size_t matchPos = i->position();

            // Verificar se não está precedido por '.' (swizzle como .xxx)
            bool isSwizzle = false;
            if (matchPos > 0 && result[matchPos - 1] == '.')
            {
                isSwizzle = true;
            }

            // Verificar se não está em "float X" (declaração)
            bool isDeclaration = false;
            if (matchPos >= 6)
            {
                std::string before = result.substr(matchPos - 6, 6);
                if (before == "float ")
                {
                    isDeclaration = true;
                }
            }

            if (isSwizzle || isDeclaration)
            {
                // Pular (não substituir)
                temp += result.substr(lastPos, matchPos - lastPos + param.length());
                lastPos = matchPos + param.length();
                continue;
            }

            // Substituir
            temp += result.substr(lastPos, matchPos - lastPos);
            temp += "u_" + param;
            lastPos = matchPos + param.length();
        }
        temp += result.substr(lastPos);
        result = temp;
    }

    // Adicionar uniforms de texturas de histórico se forem usados
    // OriginalHistory0-7, OriginalHistorySize0-7
    for (int i = 0; i <= 7; ++i)
    {
        std::string historyName = "OriginalHistorySize" + std::to_string(i);
        if (result.find(historyName) != std::string::npos)
        {
            std::string uniformDecl = "uniform vec4 " + historyName;
            if (result.find(uniformDecl) == std::string::npos)
            {
                missingUniforms << uniformDecl << ";\n";
            }
        }
    }

    // Inserir uniforms faltantes após a versão
    if (!missingUniforms.str().empty())
    {
        std::regex versionRegex(R"(#version\s+\d+)");
        result = std::regex_replace(result, versionRegex, "$&\n" + missingUniforms.str(), std::regex_constants::format_first_only);
    }

    // DEBUG: Salvar shader ANTES da separação de stages (apenas pass2 fragment)
    if (basePath.find("crt-guest-advanced-hd-pass2") != std::string::npos && !isVertex)
    {
        std::ofstream out("debug_pass10_before_stages.glsl");
        if (out)
        {
            out << result;
            out.close();
            LOG_INFO("Shader pass 10 ANTES de separar stages salvo");
        }
    }

    // Separar vertex e fragment shaders usando #pragma stage
    if (!isVertex)
    {
        // Fragment shader: remover seções vertex, manter apenas fragment
        std::istringstream stream(result);
        std::string line;
        std::ostringstream fragmentOutput;
        bool inVertexStage = false;
        bool inFragmentStage = false;
        bool hasPragma = false;

        while (std::getline(stream, line))
        {
            if (line.find("#pragma stage vertex") != std::string::npos)
            {
                inVertexStage = true;
                inFragmentStage = false;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma stage fragment") != std::string::npos)
            {
                inVertexStage = false;
                inFragmentStage = true;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma") != std::string::npos && line.find("stage") == std::string::npos)
            {
                // Outro pragma, manter
                fragmentOutput << line << "\n";
                continue;
            }

            if (hasPragma)
            {
                if (inFragmentStage || (!inVertexStage && !inFragmentStage))
                {
                    fragmentOutput << line << "\n";
                }
            }
            else
            {
                // Sem pragma, assumir que é fragment shader
                fragmentOutput << line << "\n";
            }
        }

        if (hasPragma)
        {
            result = fragmentOutput.str();
        }
    }
    else
    {
        // Vertex shader: remover seções fragment, manter apenas vertex
        std::istringstream stream(result);
        std::string line;
        std::ostringstream vertexOutput;
        bool inVertexStage = false;
        bool inFragmentStage = false;
        bool hasPragma = false;

        while (std::getline(stream, line))
        {
            if (line.find("#pragma stage vertex") != std::string::npos)
            {
                inVertexStage = true;
                inFragmentStage = false;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma stage fragment") != std::string::npos)
            {
                inVertexStage = false;
                inFragmentStage = true;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma") != std::string::npos && line.find("stage") == std::string::npos)
            {
                vertexOutput << line << "\n";
                continue;
            }

            if (hasPragma)
            {
                if (inVertexStage || (!inVertexStage && !inFragmentStage))
                {
                    vertexOutput << line << "\n";
                }
            }
            else
            {
                // Sem pragma, assumir que é vertex shader
                vertexOutput << line << "\n";
            }
        }

        if (hasPragma)
        {
            result = vertexOutput.str();
        }
    }

    // Remover pragmas de stage após processamento
    result = std::regex_replace(result, std::regex("#pragma\\s+stage\\s+(vertex|fragment)\\s*"), "");

    // Remover layout(location = X) - não suportado em GLSL 3.30 sem extensões
    // layout(location = 0) in/out -> in/out
    result = std::regex_replace(result, std::regex("layout\\(location\\s*=\\s*\\d+\\)\\s+"), "");

    // Converter uint para float (OpenGL 3.3 não suporta uint uniforms de forma confiável)
    result = std::regex_replace(result, std::regex("uniform\\s+uint\\s+"), "uniform float ");

    // Limpar linhas vazias múltiplas
    result = std::regex_replace(result, std::regex("\n\\s*\n\\s*\n"), "\n\n");

    // DEBUG: Salvar shader convertido para análise (apenas pass2 fragment)
    if (basePath.find("crt-guest-advanced-hd-pass2") != std::string::npos && !isVertex)
    {
        std::ofstream out("debug_pass10_fragment.glsl");
        if (out)
        {
            out << result;
            out.close();
            LOG_INFO("Shader pass 10 fragment salvo em: debug_pass10_fragment.glsl");
            // Mostrar linhas ao redor da 295
            std::istringstream stream(result);
            std::string line;
            int lineNum = 1;
            while (std::getline(stream, line) && lineNum < 300)
            {
                if (lineNum >= 290 && lineNum <= 300)
                {
                    LOG_INFO("Linha " + std::to_string(lineNum) + ": " + line);
                }
                lineNum++;
            }
        }
    }

    return result;
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
            std::filesystem::path currentPath = std::filesystem::current_path();

            // 1. Relativo ao diretório do shader atual
            if (!basePath.empty())
            {
                std::filesystem::path base(basePath);
                std::filesystem::path resolved = base / includePath;
                if (std::filesystem::exists(resolved))
                {
                    fullPath = resolved.string();
                }
            }

            // 2. Em shaders/shaders_slang/
            if (fullPath.empty())
            {
                std::filesystem::path slangPath = currentPath / "shaders" / "shaders_slang" / includePath;
                if (std::filesystem::exists(slangPath))
                {
                    fullPath = slangPath.string();
                }
            }

            // 3. Relativo ao diretório atual
            if (fullPath.empty())
            {
                std::filesystem::path relPath = currentPath / includePath;
                if (std::filesystem::exists(relPath))
                {
                    fullPath = relPath.string();
                }
            }

            // 4. Tentar com caminho relativo do shader (subindo diretórios)
            if (fullPath.empty() && !basePath.empty())
            {
                std::filesystem::path base(basePath);
                // Remover "../" do início
                std::string cleanPath = includePath;
                while (cleanPath.find("../") == 0)
                {
                    cleanPath = cleanPath.substr(3);
                    base = base.parent_path();
                }
                std::filesystem::path resolved = base / cleanPath;
                if (std::filesystem::exists(resolved))
                {
                    fullPath = resolved.string();
                }
            }
        }

        if (!fullPath.empty() && std::filesystem::exists(fullPath))
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
                std::filesystem::path includeFilePath(fullPath);
                std::string includeDir = includeFilePath.parent_path().string();
                includeContent = processIncludes(includeContent, includeDir);

                // Substituir o #include pelo conteúdo
                result = std::regex_replace(result, includeRegex, includeContent, std::regex_constants::format_first_only);
                LOG_INFO("Arquivo incluído: " + fullPath);
            }
            else
            {
                LOG_WARN("Falha ao abrir arquivo incluído: " + fullPath);
                // Remover o #include que falhou
                result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
            }
        }
        else
        {
            LOG_WARN("Arquivo incluído não encontrado: " + includePath + " (base: " + basePath + ")");
            // Remover o #include que falhou
            result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
        }
    }

    return result;
}

void ShaderEngine::setViewport(uint32_t width, uint32_t height)
{
    m_viewportWidth = width;
    m_viewportHeight = height;
    LOG_INFO("Viewport atualizado: " + std::to_string(width) + "x" + std::to_string(height));
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
