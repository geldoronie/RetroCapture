#include "ShaderPreprocessor.h"
#include "../utils/Logger.h"
#include "ShaderEngine.h"  // For ShaderParameterInfo
#include "../utils/FilesystemCompat.h"
#include "../renderer/glad_loader.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

ShaderPreprocessor::PreprocessResult ShaderPreprocessor::preprocess(
    const std::string& shaderSource,
    const std::string& shaderPath,
    size_t passIndex,
    uint32_t outputWidth,
    uint32_t outputHeight,
    uint32_t inputWidth,
    uint32_t inputHeight,
    const std::vector<ShaderPass>& presetPasses)
{
    PreprocessResult result;

    // Extrair diretório base do shader para resolver includes
    fs::path shaderPathObj(shaderPath);
    std::string shaderDir = shaderPathObj.parent_path().string();

    // Processar includes primeiro
    std::string processedSource = processIncludes(shaderSource, shaderDir);

    // EXTRAIR parâmetros de #pragma parameter ANTES de remover
    // Formato: #pragma parameter paramName "Description" default min max step
    std::map<std::string, float> paramDefaults; // Nome -> valor padrão
    std::regex pragmaParamRegex("#pragma\\s+parameter\\s+(\\w+)\\s+\"([^\"]*)\"\\s+([\\d.]+)\\s+([\\d.]+)\\s+([\\d.]+)\\s+([\\d.]+)");
    auto pragmaBegin = std::sregex_iterator(processedSource.begin(), processedSource.end(), pragmaParamRegex);
    auto pragmaEnd = std::sregex_iterator();
    for (std::sregex_iterator i = pragmaBegin; i != pragmaEnd; ++i)
    {
        std::string paramName = (*i)[1].str();
        std::string description = (*i)[2].str();
        std::string defaultValue = (*i)[3].str();
        std::string minValue = (*i)[4].str();
        std::string maxValue = (*i)[5].str();
        std::string stepValue = (*i)[6].str();
        // Ignorar parâmetros que são apenas labels/títulos (começam com bogus_)
        if (paramName.find("bogus_") == std::string::npos)
        {
            try
            {
                float defVal = std::stof(defaultValue);
                float minVal = std::stof(minValue);
                float maxVal = std::stof(maxValue);
                float stepVal = std::stof(stepValue);
                
                paramDefaults[paramName] = defVal;
                
                ShaderParameterInfo info;
                info.defaultValue = defVal;
                info.min = minVal;
                info.max = maxVal;
                info.step = stepVal;
                info.description = description;
                result.parameterInfo[paramName] = info;
            }
            catch (...)
            {
                paramDefaults[paramName] = 0.0f; // Fallback
                ShaderParameterInfo info;
                info.defaultValue = 0.0f;
                info.min = 0.0f;
                info.max = 1.0f;
                info.step = 0.01f;
                info.description = description;
                result.parameterInfo[paramName] = info;
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

    // Corrigir OutputSize type
    processedSource = correctOutputSizeType(processedSource);

    // Armazenar valores padrão
    result.extractedParameters = paramDefaults;

    // SIMPLIFICADO: Usar o MESMO código fonte para vertex e fragment
    // RetroArch faz isso - adiciona defines diferentes antes do código
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
        // Adicionar versão GLSL dinâmica baseada na versão OpenGL disponível
        versionLine = getGLSLVersionString() + "\n";
    }

    // Construir shaders: version + extension + define + código completo
    // RetroArch adiciona: "#define VERTEX\n#define PARAMETER_UNIFORM\n" para vertex
    // e "#define FRAGMENT\n#define PARAMETER_UNIFORM\n" para fragment
    // IMPORTANTE: RetroArch também adiciona defines para compatibilidade
    
    // Verificar se estamos usando OpenGL ES
    bool isES = isOpenGLES();
    
    // Adicionar extensão para inicialização estilo C (GL_ARB_shading_language_420pack)
    // Isso permite inicialização de arrays e estruturas estilo C
    // IMPORTANTE: Esta extensão NÃO é suportada em OpenGL ES, então só adicionar em Desktop
    std::string extensionLine = "";
    if (!isES) {
        extensionLine = "#extension GL_ARB_shading_language_420pack : require\n";
    }
    
    // Remover extensões não suportadas do código fonte se for OpenGL ES
    std::string processedCodeAfterVersion = codeAfterVersion;
    if (isES) {
        // Remover todas as extensões GL_ARB_shading_language_420pack do código fonte
        std::regex extensionRegex(R"(#extension\s+GL_ARB_shading_language_420pack\s*:?\s*\w*\s*\n?)");
        processedCodeAfterVersion = std::regex_replace(processedCodeAfterVersion, extensionRegex, "");
        
        // Remover outras extensões problemáticas comuns em ES
        std::regex arbExtensionRegex(R"(#extension\s+GL_ARB_[^\n]*\n?)");
        processedCodeAfterVersion = std::regex_replace(processedCodeAfterVersion, arbExtensionRegex, "");
    }

    std::string vertexCode = processedCodeAfterVersion;
    std::string fragmentCode = processedCodeAfterVersion;

    // Injetar código de compatibilidade se necessário
    injectCompatibilityCode(vertexCode, fragmentCode, shaderPath, passIndex,
                          outputWidth, outputHeight, inputWidth, inputHeight, presetPasses);

    // Adicionar precisão para OpenGL ES (deve vir logo após #version)
    // NOTA: GL_ES é definido automaticamente pelo driver OpenGL ES, não precisamos defini-lo manualmente
    std::string precisionLine = "";
    if (isES) {
        // Em OpenGL ES, precisamos especificar precisão para todos os tipos de ponto flutuante
        // Isso deve vir logo após #version, antes de qualquer código
        // GL_ES já está definido automaticamente pelo driver, então os shaders podem usar #ifdef GL_ES
        precisionLine = "precision mediump float;\nprecision mediump int;\n";
    }
    
    // Construir fontes finais com defines
    // Ordem: version + precision (se ES) + extension (se Desktop) + defines + código
    result.vertexSource = versionLine + precisionLine + extensionLine + "#define VERTEX\n#define PARAMETER_UNIFORM\n" + vertexCode;
    result.fragmentSource = versionLine + precisionLine + extensionLine + "#define FRAGMENT\n#define PARAMETER_UNIFORM\n" + fragmentCode;

    return result;
}

std::string ShaderPreprocessor::processIncludes(const std::string& source, const std::string& basePath)
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
            LOG_WARN("Arquivo incluído não encontrado: " + includePath);
            // Remover o #include que falhou
            result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
        }
    }

    return result;
}

std::string ShaderPreprocessor::correctOutputSizeType(const std::string& source)
{
    std::string processedSource = source;

    // Verificar se OutputSize é usado e detectar o tipo esperado
    // IMPORTANTE: Detectar o tipo necessário ANTES de verificar a declaração
    // Se o código precisa de vec3 mas está declarado como vec2, precisamos corrigir
    bool outputSizeUsed = (processedSource.find("OutputSize") != std::string::npos);

    // IMPORTANTE: Verificar PRIMEIRO se OutputSize é usado em #define que espera vec2
    // Padrão: #define OutSize vec4(OutputSize, 1.0 / OutputSize)
    // ou: #define outsize vec4(OutputSize, 1.0 / OutputSize)
    // Este padrão REQUER que OutputSize seja vec2
    // Padrão mais flexível: qualquer nome de variável seguido de vec4(OutputSize, ...)
    std::regex definePattern(R"(#define\s+\w+\s+vec4\s*\(\s*OutputSize\s*,\s*[^)]*OutputSize)");
    bool usedInVec4Define = std::regex_search(processedSource, definePattern);

    // Detectar tipo necessário baseado no uso no código
    std::string requiredType = "vec2"; // Default
    if (outputSizeUsed)
    {
        // Se OutputSize é usado em #define vec4(OutputSize, ...), DEVE ser vec2
        // Esta verificação deve vir ANTES de outras detecções para ter prioridade
        if (usedInVec4Define)
        {
            requiredType = "vec2";
        }
        else
        {
            // Procurar por padrões de uso que indiquem o tipo necessário
            // Padrões possíveis:
            // - vec3 x = OutputSize;
            // - vec3 x = OutputSize.xyz;
            // - vec3 x; x = OutputSize;
            // - vec3(OutputSize)
            // - vec3 x = vec3(OutputSize);
            std::regex vec3Pattern1(R"(\bvec3\s+\w+\s*=\s*OutputSize\b)");
            std::regex vec3Pattern2(R"(\bvec3\s*\(\s*OutputSize\s*\))");
            std::regex vec3Pattern3(R"(\bvec3\s+\w+\s*=\s*OutputSize\s*\.)");
            std::regex vec3Pattern4(R"(\bvec3\s+\w+\s*=\s*vec3\s*\(\s*OutputSize)");

            std::regex vec4Pattern1(R"(\bvec4\s+\w+\s*=\s*OutputSize\b)");
            std::regex vec4Pattern2(R"(\bvec4\s*\(\s*OutputSize\s*\))");
            std::regex vec4Pattern3(R"(\bvec4\s+\w+\s*=\s*vec4\s*\(\s*OutputSize)");

            bool isVec3 = std::regex_search(processedSource, vec3Pattern1) ||
                          std::regex_search(processedSource, vec3Pattern2) ||
                          std::regex_search(processedSource, vec3Pattern3) ||
                          std::regex_search(processedSource, vec3Pattern4);
            bool isVec4 = std::regex_search(processedSource, vec4Pattern1) ||
                          std::regex_search(processedSource, vec4Pattern2) ||
                          std::regex_search(processedSource, vec4Pattern3);

            if (isVec3)
            {
                requiredType = "vec3";
            }
            else if (isVec4)
            {
                requiredType = "vec4";
            }
            else
            {
                // Se OutputSize é usado mas não detectamos o tipo, fazer uma análise mais profunda
                // Contar ocorrências de vec3, vec4 próximas a OutputSize
                int vec3Count = 0;
                int vec4Count = 0;

                // Procurar todas as ocorrências de OutputSize
                size_t pos = 0;
                while ((pos = processedSource.find("OutputSize", pos)) != std::string::npos)
                {
                    // Verificar contexto antes e depois de OutputSize (até 100 caracteres)
                    size_t start = (pos > 100) ? pos - 100 : 0;
                    size_t end = (pos + 100 < processedSource.length()) ? pos + 100 : processedSource.length();
                    std::string context = processedSource.substr(start, end - start);

                    // Contar vec3 e vec4 no contexto
                    if (context.find("vec3") != std::string::npos)
                        vec3Count++;
                    if (context.find("vec4") != std::string::npos)
                        vec4Count++;

                    pos += 10; // Avançar após "OutputSize"
                }

                // Se há mais vec3 do que vec4 próximo a OutputSize, assumir vec3
                if (vec3Count > vec4Count && vec3Count > 0)
                {
                    requiredType = "vec3";
                }
                else if (vec4Count > vec3Count && vec4Count > 0)
                {
                    requiredType = "vec4";
                }
                else if (processedSource.find("vec3") != std::string::npos &&
                         processedSource.find("OutputSize") != std::string::npos)
                {
                    // Fallback: se há vec3 e OutputSize no shader, assumir vec3
                    requiredType = "vec3";
                }
            }
        }
    }

    // Verificar se OutputSize já está declarado como uniform
    // IMPORTANTE: Shaders podem ter múltiplas declarações (VERTEX e FRAGMENT)
    // Precisamos verificar todas e substituir TODAS se necessário
    std::regex uniformDeclRegex(R"(uniform\s+(?:COMPAT_PRECISION\s+|PRECISION\s+)?(vec[234]|float|int|uint)\s+OutputSize)");
    std::smatch declMatch;
    bool hasOutputSizeDecl = std::regex_search(processedSource, declMatch, uniformDeclRegex);
    std::string declaredType = hasOutputSizeDecl ? declMatch[1].str() : "";

    // Se o tipo necessário é diferente do declarado, precisamos corrigir
    if (outputSizeUsed)
    {
        if (!hasOutputSizeDecl)
        {
            // Não está declarado, injetar com o tipo necessário
            std::string outputSizeDecl = "uniform " + requiredType + " OutputSize;\n";
            std::regex versionRegex(R"(#version\s+\d+[^\n]*)");
            processedSource = std::regex_replace(processedSource, versionRegex, "$&\n" + outputSizeDecl, std::regex_constants::format_first_only);
        }
        else if (declaredType != requiredType)
        {
            // Está declarado mas com tipo errado, substituir TODAS as ocorrências
            // IMPORTANTE: Shaders podem ter múltiplas declarações (VERTEX e FRAGMENT)
            // Precisamos substituir TODAS, preservando o qualificador de precisão
            // Padrão mais flexível para capturar PRECISION, COMPAT_PRECISION ou nenhum
            std::regex replaceRegex(R"(uniform\s+((?:COMPAT_)?PRECISION\s+)?(vec[234]|float|int|uint)\s+OutputSize)");
            
            // Encontrar a primeira ocorrência para determinar o qualificador usado
            std::smatch firstMatch;
            std::string precisionQualifier = "";
            if (std::regex_search(processedSource, firstMatch, replaceRegex))
            {
                if (firstMatch[1].matched && !firstMatch[1].str().empty())
                {
                    precisionQualifier = firstMatch[1].str();
                }
            }
            
            // Construir replacement preservando o qualificador de precisão
            std::string replacement = "uniform " + precisionQualifier + requiredType + " OutputSize";
            
            // Substituir TODAS as ocorrências (não apenas a primeira)
            processedSource = std::regex_replace(processedSource, replaceRegex, replacement);
        }
    }

    return processedSource;
}

std::string ShaderPreprocessor::buildFinalSource(
    const std::string& code,
    bool /* isVertex */)
{
    // Esta função não é mais usada, mas mantida para compatibilidade
    // A construção final é feita diretamente em preprocess()
    return code;
}

void ShaderPreprocessor::injectCompatibilityCode(
    std::string& vertexCode,
    std::string& fragmentCode,
    const std::string& shaderPath,
    size_t passIndex,
    uint32_t /* outputWidth */,
    uint32_t /* outputHeight */,
    uint32_t /* inputWidth */,
    uint32_t /* inputHeight */,
    const std::vector<ShaderPass>& presetPasses)
{
    // IMPORTANTE: Para shaders que escalam a altura (como interlacing.glsl com scale_y = 2.0),
    // o shader lê COMPAT_TEXTURE(Source, vTexCoord) onde Source é a entrada, mas vTexCoord.y
    // é baseado na saída. Precisamos ajustar o vertex shader para que TEX0.y seja baseado
    // na entrada quando a saída é escalada. Isso permite que o fragment shader leia corretamente
    // da textura de entrada.
    // Verificar se este pass escala a altura e se é o shader interlacing.glsl
    bool needsTexCoordAdjustment = false;
    if (passIndex < presetPasses.size())
    {
        const auto &presetPass = presetPasses[passIndex];
        // Verificar se scale_y escala a altura
        if (presetPass.scaleTypeY == "viewport" || presetPass.scaleTypeY == "absolute" ||
            (presetPass.scaleTypeY == "source" && presetPass.scaleY != 1.0f))
        {
            // Verificar se é o shader interlacing.glsl
            if (shaderPath.find("interlacing.glsl") != std::string::npos)
            {
                needsTexCoordAdjustment = true;
            }
        }
    }

    // Se precisamos ajustar TexCoord, injetar código após TEX0.xy = TexCoord.xy;
    if (needsTexCoordAdjustment)
    {
        // Procurar por "TEX0.xy = TexCoord.xy;" no código do vertex shader
        std::string pattern = "TEX0.xy = TexCoord.xy;";
        size_t pos = vertexCode.find(pattern);
        if (pos != std::string::npos)
        {
            // Injetar código para ajustar TEX0.y baseado na entrada quando a saída é escalada
            // Quando a saída tem o dobro da altura, cada linha da entrada deve ser lida duas vezes
            // Para replicar cada linha: TEX0.y = floor(TEX0.y * OutputSize.y / 2.0) / InputSize.y
            // Mas isso requer floor() que pode não estar disponível no vertex shader
            // Alternativa: usar uma abordagem que mapeia [0,1] na saída para [0,1] na entrada
            // mas replicando cada linha: TEX0.y = (floor(TEX0.y * OutputSize.y / 2.0) + 0.5) / InputSize.y
            // Simplificando: TEX0.y = floor(TEX0.y * OutputSize.y / 2.0) / InputSize.y
            std::string adjustment = "\n   // Ajustar TEX0.y para replicar cada linha da entrada duas vezes na saída\n";
            adjustment += "   // Quando a saída tem o dobro da altura, mapear coordenadas para replicar linhas\n";
            adjustment += "   TEX0.y = (floor(TEX0.y * OutputSize.y / 2.0) + 0.5) / InputSize.y;\n";
            vertexCode.insert(pos + pattern.length(), adjustment);
        }
    }

    // IMPORTANTE: Para shaders como box-center.glsl que usam gl_FragCoord.xy diretamente
    // na verificação de bordas, precisamos ajustar o fragment shader para normalizar
    // gl_FragCoord.xy dividindo por OutputSize.xy
    bool needsFragCoordAdjustment = false;
    bool needsInterlaceAdjustment = false;
    // Verificar se é o shader box-center.glsl
    if (shaderPath.find("box-center.glsl") != std::string::npos)
    {
        needsFragCoordAdjustment = true;
    }
    // Verificar se é o shader interlacing.glsl e se precisa ajustar o cálculo do interlace
    if (shaderPath.find("interlacing.glsl") != std::string::npos && needsTexCoordAdjustment)
    {
        needsInterlaceAdjustment = true;
    }

    // Se precisamos ajustar gl_FragCoord, injetar código após bordertest = gl_FragCoord.xy;
    if (needsFragCoordAdjustment)
    {
        // Procurar por "bordertest = gl_FragCoord.xy;" no código do fragment shader
        std::string pattern = "bordertest = gl_FragCoord.xy;";
        size_t pos = fragmentCode.find(pattern);
        if (pos != std::string::npos)
        {
            // Injetar código para normalizar gl_FragCoord.xy dividindo por OutputSize.xy
            std::string adjustment = "\n   // Normalizar gl_FragCoord.xy dividindo por OutputSize.xy\n";
            adjustment += "   bordertest = bordertest / OutputSize.xy;\n";
            fragmentCode.insert(pos + pattern.length(), adjustment);
        }
    }

    // Se precisamos ajustar o cálculo do interlace, usar gl_FragCoord.y em vez de vTexCoord.y
    if (needsInterlaceAdjustment)
    {
        // Procurar por padrões como "y = 2.000001 * TextureSize.y * vTexCoord.y" ou "y = TextureSize.y * vTexCoord.y"
        // e substituir vTexCoord.y por gl_FragCoord.y / OutputSize.y para usar coordenadas baseadas na saída
        std::regex interlacePattern1(R"(\by\s*=\s*2\.0+[0-9]*\s*\*\s*TextureSize\.y\s*\*\s*vTexCoord\.y)");
        std::regex interlacePattern2(R"(\by\s*=\s*TextureSize\.y\s*\*\s*vTexCoord\.y)");

        if (std::regex_search(fragmentCode, interlacePattern1))
        {
            // Substituir vTexCoord.y por gl_FragCoord.y / OutputSize.y no cálculo do interlace
            fragmentCode = std::regex_replace(fragmentCode, interlacePattern1,
                                              "y = 2.000001 * TextureSize.y * (gl_FragCoord.y / OutputSize.y)");
        }
        else if (std::regex_search(fragmentCode, interlacePattern2))
        {
            // Substituir vTexCoord.y por gl_FragCoord.y / OutputSize.y no cálculo do interlace
            fragmentCode = std::regex_replace(fragmentCode, interlacePattern2,
                                              "y = TextureSize.y * (gl_FragCoord.y / OutputSize.y)");
        }
    }
}

