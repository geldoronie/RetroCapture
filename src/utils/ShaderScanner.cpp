#include "ShaderScanner.h"
#include "../utils/Logger.h"
#include "FilesystemCompat.h"
#include <algorithm>
#include <cctype>

std::vector<std::string> ShaderScanner::scan(const std::string& basePath)
{
    std::vector<std::string> shaders;

    fs::path path(basePath);
    if (!fs::exists(path))
    {
        // Tentar caminho relativo ao diretório de trabalho
        path = fs::current_path() / basePath;
    }

    if (!fs::exists(path))
    {
        LOG_WARN("Diretório de shaders não encontrado: " + basePath);
        return shaders;
    }

    try
    {
        // Normalizar o caminho base para comparações
        fs::path normalizedBasePath = fs::canonical(path);

        // Escanear recursivamente todos os arquivos
        // Nota: range-based for não funciona com nossa implementação, usar loop manual
        fs::recursive_directory_iterator it(path);
        fs::recursive_directory_iterator end;
        for (; it != end; ++it)
        {
            if (it.is_regular_file())
            {
                std::string ext = it.path().extension();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".glslp")
                {
                    fs::path entryPath = it.path();

                    // Tentar normalizar o caminho do arquivo
                    fs::path normalizedEntryPath;
                    try
                    {
                        normalizedEntryPath = fs::canonical(entryPath);
                    }
                    catch (...)
                    {
                        // Se falhar, usar o caminho original
                        normalizedEntryPath = entryPath;
                    }

                    // Obter o diretório pai normalizado
                    fs::path parentPath = normalizedEntryPath.parent_path();
                    try
                    {
                        parentPath = fs::canonical(parentPath);
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
                        relativePath = entryPath.filename();
                    }
                    else
                    {
                        // Arquivo está em subpasta, usar caminho relativo completo
                        relativePath = fs::relative(entryPath, path).string();
                    }

                    shaders.push_back(relativePath);
                }
            }
        }
    }
    catch (const fs::filesystem_error &e)
    {
        LOG_ERROR("Erro ao escanear diretório de shaders: " + std::string(e.what()));
    }

    // Sort the shader list alphabetically
    std::sort(shaders.begin(), shaders.end());

    return shaders;
}

