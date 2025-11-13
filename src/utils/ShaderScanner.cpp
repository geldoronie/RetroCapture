#include "ShaderScanner.h"
#include "Logger.h"
#include <filesystem>
#include <algorithm>

std::vector<std::string> ShaderScanner::scan(const std::string& basePath)
{
    std::vector<std::string> shaders;
    scan(basePath, shaders);
    return shaders;
}

size_t ShaderScanner::scan(const std::string& basePath, std::vector<std::string>& shaders)
{
    shaders.clear();

    std::filesystem::path path(basePath);
    if (!std::filesystem::exists(path))
    {
        // Tentar caminho relativo ao diretório de trabalho
        path = std::filesystem::current_path() / basePath;
    }

    if (!std::filesystem::exists(path))
    {
        LOG_WARN("Diretório de shaders não encontrado: " + basePath);
        return 0;
    }

    try
    {
        // Normalizar o caminho base para comparações
        std::filesystem::path normalizedBasePath = std::filesystem::canonical(path);

        // Escanear recursivamente todos os arquivos
        for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
        {
            if (entry.is_regular_file())
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".glslp")
                {
                    std::filesystem::path entryPath = entry.path();

                    // Tentar normalizar o caminho do arquivo
                    std::filesystem::path normalizedEntryPath;
                    try
                    {
                        normalizedEntryPath = std::filesystem::canonical(entryPath);
                    }
                    catch (...)
                    {
                        // Se falhar, usar o caminho original
                        normalizedEntryPath = entryPath;
                    }

                    // Obter o diretório pai normalizado
                    std::filesystem::path parentPath = normalizedEntryPath.parent_path();
                    try
                    {
                        parentPath = std::filesystem::canonical(parentPath);
                    }
                    catch (...)
                    {
                        // Se falhar, usar o caminho original
                        parentPath = entryPath.parent_path();
                    }

                    // Calcular caminho relativo
                    std::string relativePath;
                    if (parentPath == normalizedBasePath)
                    {
                        // Arquivo está na raiz, usar apenas o nome do arquivo
                        relativePath = entryPath.filename().string();
                    }
                    else
                    {
                        // Arquivo está em subpasta, usar caminho relativo completo
                        relativePath = std::filesystem::relative(entryPath, path).string();
                    }

                    shaders.push_back(relativePath);
                }
            }
        }

        std::sort(shaders.begin(), shaders.end());
        LOG_INFO("Encontrados " + std::to_string(shaders.size()) + " shaders em " + basePath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Erro ao escanear shaders: " + std::string(e.what()));
    }

    return shaders.size();
}

