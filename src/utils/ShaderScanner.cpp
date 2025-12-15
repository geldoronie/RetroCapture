#include "ShaderScanner.h"
#include "../utils/Logger.h"
#include "FilesystemCompat.h"
#include <algorithm>
#include <cctype>

std::vector<std::string> ShaderScanner::scan(const std::string &basePath)
{
    std::vector<std::string> shaders;

    fs::path path(basePath);
    if (!fs::exists(path))
    {
        // Try relative path to working directory
        path = fs::current_path() / basePath;
    }

    if (!fs::exists(path))
    {
        LOG_WARN("Diretório de shaders não encontrado: " + basePath);
        return shaders;
    }

    try
    {
        // Normalize base path for comparisons
        fs::path normalizedBasePath = fs::canonical(path);

        // Recursively scan all files
        // Note: range-based for doesn't work with our implementation, use manual loop
        fs::recursive_directory_iterator it(path);
        fs::recursive_directory_iterator end;
        for (; it != end; ++it)
        {
            // No Linux usa std::filesystem padrão (C++17), precisa desreferenciar
            // No Windows com MinGW < 8 usa implementação customizada que tem métodos diretos
#if defined(_WIN32) && defined(__GNUC__) && __GNUC__ < 8
            // Custom implementation from FilesystemCompat.h (Windows MinGW < 8)
            if (it.is_regular_file())
            {
                std::string ext = it.path().extension();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".glslp")
                {
                    fs::path entryPath = it.path();
#else
            // std::filesystem padrão (C++17) - Linux e Windows MinGW >= 8
            // *it retorna directory_entry, então usamos it->path() e fs::is_regular_file(*it)
            if (fs::is_regular_file(*it))
            {
                std::string ext = it->path().extension();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".glslp")
                {
                    fs::path entryPath = it->path();
#endif

                    // Try to normalize file path
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

                    // Get normalized parent directory
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
                        // File is in root, use only filename
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
        LOG_ERROR("Error scanning shader directory: " + std::string(e.what()));
    }

    // Sort the shader list alphabetically
    std::sort(shaders.begin(), shaders.end());

    return shaders;
}
