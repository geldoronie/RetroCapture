#pragma once

// Compatibilidade de filesystem para diferentes versões de C++ e compiladores
#if defined(_WIN32) && defined(__GNUC__) && __GNUC__ < 8
// MinGW com GCC < 8: usar implementação manual baseada em Windows API
#include <string>
#include <vector>
#include <cstdint>
#include <stack>
#include <memory>
#include <cstring>
#include <windows.h>
#include <shlwapi.h>
#include <direct.h>
#include <io.h>

namespace fs
{
    class path
    {
    private:
        std::string m_path;

    public:
        path() : m_path("") {}
        path(const std::string &p) : m_path(p) {}
        path(const char *p) : m_path(p ? p : "") {}

        path operator/(const path &other) const
        {
            if (m_path.empty())
                return other;
            if (other.m_path.empty())
                return *this;
            std::string sep = (m_path.back() == '/' || m_path.back() == '\\') ? "" : "\\";
            return path(m_path + sep + other.m_path);
        }

        path parent_path() const
        {
            size_t pos = m_path.find_last_of("/\\");
            if (pos == std::string::npos)
                return path();
            return path(m_path.substr(0, pos));
        }

        std::string filename() const
        {
            size_t pos = m_path.find_last_of("/\\");
            if (pos == std::string::npos)
                return m_path;
            return m_path.substr(pos + 1);
        }

        std::string extension() const
        {
            std::string fname = filename();
            size_t pos = fname.find_last_of('.');
            if (pos == std::string::npos)
                return "";
            return fname.substr(pos);
        }

        std::string string() const { return m_path; }
        const char *c_str() const { return m_path.c_str(); }

        bool empty() const { return m_path.empty(); }

        path &replace_extension(const std::string &ext)
        {
            size_t pos = m_path.find_last_of('.');
            if (pos != std::string::npos)
            {
                m_path = m_path.substr(0, pos) + ext;
            }
            else
            {
                m_path += ext;
            }
            return *this;
        }

        bool operator==(const path &other) const
        {
            return m_path == other.m_path;
        }

        bool operator!=(const path &other) const
        {
            return m_path != other.m_path;
        }

        bool is_absolute() const
        {
            if (m_path.empty())
                return false;
            if (m_path.length() >= 2 && m_path[1] == ':')
                return true; // Windows drive (C:)
            if (m_path[0] == '/' || m_path[0] == '\\')
                return true;
            return false;
        }

        bool is_relative() const { return !is_absolute(); }

        path lexically_normal() const
        {
            // Simplificação básica - remover . e ..
            std::string result = m_path;
            // Substituir / por backslash
            size_t len = result.length();
            for (size_t idx = 0; idx < len; ++idx)
            {
                if (result[idx] == '/')
                {
                    result[idx] = '\\';
                }
            }
            return fs::path(result);
        }
    };

    // Funções inline que usam path - devem estar dentro do namespace fs
    inline bool exists(const fs::path &p)
    {
        DWORD attrs = GetFileAttributesA(p.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES;
    }

    inline bool is_directory(const fs::path &p)
    {
        DWORD attrs = GetFileAttributesA(p.c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    inline bool is_regular_file(const fs::path &p)
    {
        DWORD attrs = GetFileAttributesA(p.c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    inline fs::path current_path()
    {
        char buffer[MAX_PATH];
        if (_getcwd(buffer, MAX_PATH))
        {
            return fs::path(buffer);
        }
        return fs::path();
    }

    inline fs::path canonical(const fs::path &p)
    {
        // Simplificação: retornar absolute path
        if (p.is_absolute())
            return p;
        char fullPath[MAX_PATH];
        if (_fullpath(fullPath, p.c_str(), MAX_PATH))
        {
            return fs::path(fullPath);
        }
        // Se _fullpath falhar, usar current_path() que já está definido acima
        return current_path() / p;
    }

    inline fs::path absolute(const fs::path &p)
    {
        if (p.is_absolute())
            return p;
        char fullPath[MAX_PATH];
        if (_fullpath(fullPath, p.c_str(), MAX_PATH))
        {
            return fs::path(fullPath);
        }
        return current_path() / p;
    }

    inline fs::path relative(const fs::path &p, const fs::path &base)
    {
        // Implementação simplificada
        std::string pStr = absolute(p).string();
        std::string baseStr = absolute(base).string();

        // Normalizar
        for (size_t i = 0; i < pStr.length(); ++i)
            if (pStr[i] == '/')
                pStr[i] = '\\';
        for (size_t i = 0; i < baseStr.length(); ++i)
            if (baseStr[i] == '/')
                baseStr[i] = '\\';

        // Verificar se p está dentro de base
        if (pStr.find(baseStr) == 0)
        {
            std::string rel = pStr.substr(baseStr.length());
            if (!rel.empty() && rel[0] == '\\')
                rel = rel.substr(1);
            return fs::path(rel);
        }
        return p;
    }

    inline bool remove(const fs::path &p)
    {
        DWORD attrs = GetFileAttributesA(p.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return false;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            return RemoveDirectoryA(p.c_str()) != 0;
        }
        else
        {
            return DeleteFileA(p.c_str()) != 0;
        }
    }

    inline bool create_directories(const fs::path &p)
    {
        std::string dir = p.string();
        for (size_t j = 0; j < dir.length(); ++j)
            if (dir[j] == '/')
                dir[j] = '\\';

        // Criar diretórios recursivamente
        for (size_t i = 0; i < dir.length(); ++i)
        {
            if (dir[i] == '\\' || dir[i] == '/')
            {
                std::string subdir = dir.substr(0, i);
                if (!subdir.empty() && subdir != "." && subdir != "..")
                {
                    CreateDirectoryA(subdir.c_str(), nullptr);
                }
            }
        }
        return CreateDirectoryA(dir.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    class filesystem_error : public std::exception
    {
    public:
        filesystem_error(const std::string &msg) : m_msg(msg) {}
        const char *what() const noexcept override { return m_msg.c_str(); }

    private:
        std::string m_msg;
    };

    inline uintmax_t file_size(const fs::path &p)
    {
        WIN32_FILE_ATTRIBUTE_DATA fileData;
        if (GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fileData))
        {
            ULARGE_INTEGER size;
            size.LowPart = fileData.nFileSizeLow;
            size.HighPart = fileData.nFileSizeHigh;
            return size.QuadPart;
        }
        throw filesystem_error("Cannot get file size: " + p.string());
    }

    // Estrutura para rastrear diretórios na pilha recursiva
    struct DirectoryState
    {
        std::string path;
        HANDLE handle;
        WIN32_FIND_DATAA findData;
        bool firstEntry;

        DirectoryState(const std::string &p) : path(p), handle(INVALID_HANDLE_VALUE), firstEntry(true)
        {
            std::string searchPath = p + "\\*";
            handle = FindFirstFileA(searchPath.c_str(), &findData);
        }

        ~DirectoryState()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                FindClose(handle);
            }
        }
    };

    class recursive_directory_iterator
    {
    public:
        recursive_directory_iterator() : m_valid(false) {}
        recursive_directory_iterator(const fs::path &p) : m_valid(false)
        {
            std::string basePath = p.string();
            // Normalizar separadores
            for (size_t i = 0; i < basePath.length(); ++i)
            {
                if (basePath[i] == '/')
                    basePath[i] = '\\';
            }
            // Adicionar diretório base à pilha
            m_stack.push(std::make_unique<DirectoryState>(basePath));
            advance();
        }

        ~recursive_directory_iterator()
        {
            // Destrutor limpa automaticamente os DirectoryState
        }

        bool operator==(const recursive_directory_iterator &other) const
        {
            return m_valid == other.m_valid;
        }

        bool operator!=(const recursive_directory_iterator &other) const
        {
            return m_valid != other.m_valid;
        }

        recursive_directory_iterator &operator++()
        {
            advance();
            return *this;
        }

        fs::path path() const
        {
            if (!m_valid || m_stack.empty())
                return fs::path();
            return fs::path(m_currentPath);
        }

        bool is_regular_file() const
        {
            if (!m_valid || m_stack.empty())
                return false;
            return (m_stack.top()->findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

    private:
        void advance()
        {
            while (!m_stack.empty())
            {
                auto &state = m_stack.top();

                // Se é a primeira entrada, já temos os dados
                if (state->firstEntry)
                {
                    state->firstEntry = false;
                    // Pular . e ..
                    if (strcmp(state->findData.cFileName, ".") != 0 &&
                        strcmp(state->findData.cFileName, "..") != 0)
                    {
                        m_currentPath = state->path + "\\" + state->findData.cFileName;
                        m_valid = true;

                        // Se é um diretório, adicionar à pilha para processar recursivamente
                        if (state->findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        {
                            std::string subdirPath = state->path + "\\" + state->findData.cFileName;
                            m_stack.push(std::make_unique<DirectoryState>(subdirPath));
                            // Continuar loop para processar o novo diretório
                            continue;
                        }
                        return; // É um arquivo, retornar
                    }
                }

                // Buscar próxima entrada no diretório atual
                if (FindNextFileA(state->handle, &state->findData))
                {
                    // Pular . e ..
                    if (strcmp(state->findData.cFileName, ".") != 0 &&
                        strcmp(state->findData.cFileName, "..") != 0)
                    {
                        m_currentPath = state->path + "\\" + state->findData.cFileName;
                        m_valid = true;

                        // Se é um diretório, adicionar à pilha para processar recursivamente
                        if (state->findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        {
                            std::string subdirPath = state->path + "\\" + state->findData.cFileName;
                            m_stack.push(std::make_unique<DirectoryState>(subdirPath));
                            // Continuar loop para processar o novo diretório
                            continue;
                        }
                        return; // É um arquivo, retornar
                    }
                }
                else
                {
                    // Não há mais entradas neste diretório, remover da pilha
                    m_stack.pop();
                }
            }

            // Pilha vazia, fim da iteração
            m_valid = false;
        }

        std::stack<std::unique_ptr<DirectoryState>> m_stack;
        std::string m_currentPath;
        bool m_valid;
    };
} // namespace fs

// Namespace auxiliar para funções helper (sempre disponível)
namespace fs_helper
{
    inline bool is_regular_file(const fs::recursive_directory_iterator& iter)
    {
        return iter.is_regular_file();
    }

    inline fs::path get_path(const fs::recursive_directory_iterator& iter)
    {
        return iter.path();
    }

    inline std::string get_extension_string(const fs::path& p)
    {
        return p.extension(); // Custom implementation retorna string diretamente
    }

    inline std::string get_filename_string(const fs::path& p)
    {
        return p.filename(); // Custom implementation retorna string diretamente
    }
}
#elif defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 8 && __cplusplus >= 201703L
#include <filesystem>
namespace fs = std::filesystem;

// Namespace auxiliar para funções helper (sempre disponível)
namespace fs_helper
{
    inline bool is_regular_file(const fs::recursive_directory_iterator& iter)
    {
        return iter->is_regular_file();
    }

    inline fs::path get_path(const fs::recursive_directory_iterator& iter)
    {
        return iter->path();
    }

    inline std::string get_extension_string(const fs::path& p)
    {
        return p.extension().string(); // std::filesystem retorna path, precisa .string()
    }

    inline std::string get_filename_string(const fs::path& p)
    {
        return p.filename().string(); // std::filesystem retorna path, precisa .string()
    }
}
#elif __cplusplus >= 201703L && (!defined(_WIN32) || (defined(_MSC_VER) && _MSC_VER >= 1914))
#include <filesystem>
namespace fs = std::filesystem;

// Namespace auxiliar para funções helper (sempre disponível)
namespace fs_helper
{
    inline bool is_regular_file(const fs::recursive_directory_iterator& iter)
    {
        return iter->is_regular_file();
    }

    inline fs::path get_path(const fs::recursive_directory_iterator& iter)
    {
        return iter->path();
    }

    inline std::string get_extension_string(const fs::path& p)
    {
        return p.extension().string(); // std::filesystem retorna path, precisa .string()
    }

    inline std::string get_filename_string(const fs::path& p)
    {
        return p.filename().string(); // std::filesystem retorna path, precisa .string()
    }
}
#elif defined(__GNUC__) && __GNUC__ >= 8
#include <filesystem>
namespace fs = std::filesystem;

// Namespace auxiliar para funções helper (sempre disponível)
namespace fs_helper
{
    inline bool is_regular_file(const fs::recursive_directory_iterator& iter)
    {
        return iter->is_regular_file();
    }

    inline fs::path get_path(const fs::recursive_directory_iterator& iter)
    {
        return iter->path();
    }

    inline std::string get_extension_string(const fs::path& p)
    {
        return p.extension().string(); // std::filesystem retorna path, precisa .string()
    }

    inline std::string get_filename_string(const fs::path& p)
    {
        return p.filename().string(); // std::filesystem retorna path, precisa .string()
    }
}
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

// Namespace auxiliar para funções helper (sempre disponível)
namespace fs_helper
{
    inline bool is_regular_file(const fs::recursive_directory_iterator& iter)
    {
        return iter->is_regular_file();
    }

    inline fs::path get_path(const fs::recursive_directory_iterator& iter)
    {
        return iter->path();
    }

    inline std::string get_extension_string(const fs::path& p)
    {
        return p.extension().string(); // std::experimental::filesystem retorna path, precisa .string()
    }

    inline std::string get_filename_string(const fs::path& p)
    {
        return p.filename().string(); // std::experimental::filesystem retorna path, precisa .string()
    }
}
#endif
