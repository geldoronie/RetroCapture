#include "HTTPServer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

#ifdef ENABLE_HTTPS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#endif

HTTPServer::HTTPServer()
    : m_serverSocket(-1), m_useSSL(false)
#ifdef ENABLE_HTTPS
      ,
      m_sslContext(nullptr)
#endif
{
}

HTTPServer::~HTTPServer()
{
    closeServer();
#ifdef ENABLE_HTTPS
    cleanupSSL();
#endif
}

bool HTTPServer::setSSLCertificate(const std::string &certPath, const std::string &keyPath)
{
#ifdef ENABLE_HTTPS
    LOG_INFO("setSSLCertificate called with:");
    LOG_INFO("  certPath: " + certPath);
    LOG_INFO("  keyPath: " + keyPath);

    // Verificar se os arquivos existem antes de tentar carregar
    std::filesystem::path certFsPath(certPath);
    std::filesystem::path keyFsPath(keyPath);

    if (!std::filesystem::exists(certFsPath))
    {
        LOG_ERROR("Certificate file does not exist: " + certPath);
        return false;
    }

    if (!std::filesystem::exists(keyFsPath))
    {
        LOG_ERROR("Private key file does not exist: " + keyPath);
        return false;
    }

    LOG_INFO("Both certificate files exist. File sizes:");
    LOG_INFO("  Certificate: " + std::to_string(std::filesystem::file_size(certFsPath)) + " bytes");
    LOG_INFO("  Private Key: " + std::to_string(std::filesystem::file_size(keyFsPath)) + " bytes");

    if (!initializeSSL())
    {
        LOG_ERROR("Failed to initialize SSL");
        return false;
    }

    // Usar caminho absoluto para garantir que o OpenSSL encontre os arquivos
    std::string absCertPath = std::filesystem::absolute(certFsPath).string();
    std::string absKeyPath = std::filesystem::absolute(keyFsPath).string();

    LOG_INFO("Loading certificate from: " + absCertPath);
    // Carregar certificado
    if (SSL_CTX_use_certificate_file(m_sslContext, absCertPath.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        LOG_ERROR("Failed to load SSL certificate: " + absCertPath);
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        LOG_ERROR("OpenSSL error: " + std::string(err_buf));
        ERR_print_errors_fp(stderr);
        return false;
    }

    LOG_INFO("Loading private key from: " + absKeyPath);
    // Carregar chave privada
    if (SSL_CTX_use_PrivateKey_file(m_sslContext, absKeyPath.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        LOG_ERROR("Failed to load SSL private key: " + absKeyPath);
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        LOG_ERROR("OpenSSL error: " + std::string(err_buf));
        ERR_print_errors_fp(stderr);
        return false;
    }

    // Verificar se a chave privada corresponde ao certificado
    if (!SSL_CTX_check_private_key(m_sslContext))
    {
        LOG_ERROR("Private key does not match certificate");
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        LOG_ERROR("OpenSSL error: " + std::string(err_buf));
        return false;
    }

    m_useSSL = true;
    LOG_INFO("SSL certificate configured successfully");
    return true;
#else
    (void)certPath; // Suppress unused parameter warning
    (void)keyPath;  // Suppress unused parameter warning
    LOG_WARN("HTTPS support not compiled. Rebuild with -DENABLE_HTTPS=ON");
    return false;
#endif
}

bool HTTPServer::createServer(int port)
{
    // Criar socket
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
        LOG_ERROR("Failed to create server socket: " + std::string(strerror(errno)));
        return false;
    }

    // Configurar opções do socket
    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Configurar endereço
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Fazer bind
    if (bind(m_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR("Failed to bind to port " + std::to_string(port) + ": " + std::string(strerror(errno)));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    // Fazer listen
    if (listen(m_serverSocket, 5) < 0)
    {
        LOG_ERROR("Failed to listen on socket: " + std::string(strerror(errno)));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    LOG_INFO("HTTP" + std::string(m_useSSL ? "S" : "") + " server created on port " + std::to_string(port));
    return true;
}

int HTTPServer::acceptClient()
{
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);

    int clientFd = accept(m_serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
    if (clientFd < 0)
    {
        return -1;
    }

#ifdef ENABLE_HTTPS
    if (m_useSSL && m_sslContext)
    {
        // Detectar se o cliente está tentando HTTPS ou HTTP
        // Fazemos peek dos primeiros bytes sem consumir do buffer
        // TLS handshake tem estrutura específica:
        // Byte 0: Content Type (0x16 = Handshake)
        // Byte 1-2: Version (0x03 0x03 = TLS 1.2, 0x03 0x01 = TLS 1.0, etc.)
        // Byte 3-4: Length
        char peekBuffer[5];
        ssize_t peeked = recv(clientFd, peekBuffer, sizeof(peekBuffer), MSG_PEEK);

        bool isHTTPS = false;
        if (peeked >= 1)
        {
            unsigned char firstByte = static_cast<unsigned char>(peekBuffer[0]);

            // Helper para converter byte para hex string
            std::stringstream hexStream;
            hexStream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(firstByte);
            std::string hexStr = hexStream.str();

            // Log detalhado para debug
            LOG_INFO("Protocol detection - First byte: 0x" + hexStr +
                     " (" + std::to_string(static_cast<int>(firstByte)) + ")");

            // Handshake TLS começa com 0x16 (Content Type: Handshake)
            // Também pode começar com 0x14 (Change Cipher Spec) ou 0x15 (Alert)
            // Mas o mais comum é 0x16 para ClientHello
            if (firstByte == 0x16 || firstByte == 0x14 || firstByte == 0x15)
            {
                // É um handshake TLS
                isHTTPS = true;
                LOG_INFO("Detected HTTPS connection (TLS handshake, content type: 0x" + hexStr + ")");
            }
            else if (peeked >= 3)
            {
                // Verificar se começa com caracteres ASCII de requisição HTTP
                // "GET ", "POST ", "HEAD ", "OPTIONS ", etc.
                if ((peekBuffer[0] == 'G' && peekBuffer[1] == 'E' && peekBuffer[2] == 'T') ||
                    (peekBuffer[0] == 'P' && peekBuffer[1] == 'O' && peekBuffer[2] == 'S') ||
                    (peekBuffer[0] == 'H' && peekBuffer[1] == 'E' && peekBuffer[2] == 'A') ||
                    (peekBuffer[0] == 'O' && peekBuffer[1] == 'P' && peekBuffer[2] == 'T'))
                {
                    LOG_INFO("Detected HTTP connection (plain text: " +
                             std::string(peekBuffer, std::min(static_cast<size_t>(peeked), size_t(4))) + "...)");
                }
                else
                {
                    // Converter os 3 primeiros bytes para hex
                    std::stringstream hexStream2;
                    hexStream2 << std::hex << std::setfill('0');
                    hexStream2 << std::setw(2) << static_cast<int>(firstByte) << " ";
                    hexStream2 << std::setw(2) << static_cast<int>(static_cast<unsigned char>(peekBuffer[1])) << " ";
                    hexStream2 << std::setw(2) << static_cast<int>(static_cast<unsigned char>(peekBuffer[2]));

                    LOG_WARN("Unknown protocol, first 3 bytes: 0x" + hexStream2.str() +
                             ", treating as HTTP");
                }
            }
            else
            {
                LOG_WARN("Not enough data for protocol detection (only " +
                         std::to_string(peeked) + " bytes), treating as HTTP");
            }
        }
        else if (peeked < 0)
        {
            // Erro ao fazer peek, tratar como HTTP para evitar problemas
            LOG_WARN("Error peeking socket, treating as HTTP: " + std::string(strerror(errno)));
        }
        else
        {
            LOG_WARN("No data available for protocol detection (peeked: " +
                     std::to_string(peeked) + "), treating as HTTP");
        }

        if (isHTTPS)
        {
            // Criar SSL para este cliente
            SSL *ssl = SSL_new(m_sslContext);
            if (!ssl)
            {
                LOG_ERROR("Failed to create SSL for client");
                close(clientFd);
                return -1;
            }

            // Associar socket ao SSL
            if (SSL_set_fd(ssl, clientFd) != 1)
            {
                LOG_ERROR("Failed to set SSL file descriptor");
                SSL_free(ssl);
                close(clientFd);
                return -1;
            }

            // Fazer handshake SSL com retry para WANT_READ/WANT_WRITE
            int acceptResult = SSL_accept(ssl);
            int retries = 0;
            const int maxRetries = 10;

            while (acceptResult <= 0 && retries < maxRetries)
            {
                int err = SSL_get_error(ssl, acceptResult);

                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                {
                    // Precisa de mais dados, tentar novamente
                    retries++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    acceptResult = SSL_accept(ssl);
                    continue;
                }
                else
                {
                    // Erro real, não pode continuar
                    if (err == SSL_ERROR_SYSCALL && errno == 0)
                    {
                        LOG_WARN("SSL handshake: client closed connection during handshake (EOF)");
                    }
                    else
                    {
                        LOG_ERROR("SSL handshake failed: " + std::to_string(err));
                        ERR_print_errors_fp(stderr);
                    }

                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                    close(clientFd);
                    return -1;
                }
            }

            if (acceptResult <= 0)
            {
                LOG_ERROR("SSL handshake failed: too many retries");
                SSL_shutdown(ssl);
                SSL_free(ssl);
                close(clientFd);
                return -1;
            }

            m_sslClients[clientFd] = ssl;
            LOG_INFO("SSL connection established with client");
        }
        else
        {
            // Cliente está usando HTTP, mas servidor está configurado para HTTPS
            // Retornar o clientFd normalmente - o handleClient decidirá se redireciona ou rejeita
            LOG_INFO("Client using HTTP (not HTTPS), socket will be used for HTTP response");
        }
    }
#endif

    return clientFd;
}

ssize_t HTTPServer::sendData(int clientFd, const void *data, size_t size)
{
#ifdef ENABLE_HTTPS
    if (m_useSSL)
    {
        auto it = m_sslClients.find(clientFd);
        if (it != m_sslClients.end())
        {
            return SSL_write(it->second, data, static_cast<int>(size));
        }
    }
#endif
    return send(clientFd, data, size, MSG_NOSIGNAL);
}

ssize_t HTTPServer::receiveData(int clientFd, void *buffer, size_t size)
{
#ifdef ENABLE_HTTPS
    if (m_useSSL)
    {
        auto it = m_sslClients.find(clientFd);
        if (it != m_sslClients.end())
        {
            return SSL_read(it->second, buffer, static_cast<int>(size));
        }
    }
#endif
    return recv(clientFd, buffer, size, 0);
}

void HTTPServer::closeClient(int clientFd)
{
#ifdef ENABLE_HTTPS
    if (m_useSSL)
    {
        auto it = m_sslClients.find(clientFd);
        if (it != m_sslClients.end())
        {
            SSL_shutdown(it->second);
            SSL_free(it->second);
            m_sslClients.erase(it);
        }
    }
#endif
    close(clientFd);
}

void HTTPServer::closeServer()
{
    if (m_serverSocket >= 0)
    {
        shutdown(m_serverSocket, SHUT_RDWR);
        close(m_serverSocket);
        m_serverSocket = -1;
    }
}

std::string HTTPServer::getBaseUrl(const std::string &hostname, int port) const
{
    std::string protocol = m_useSSL ? "https" : "http";
    return protocol + "://" + hostname + ":" + std::to_string(port);
}

bool HTTPServer::isClientHTTPS(int clientFd) const
{
#ifdef ENABLE_HTTPS
    if (m_useSSL)
    {
        auto it = m_sslClients.find(clientFd);
        return (it != m_sslClients.end());
    }
#endif
    return false;
}

#ifdef ENABLE_HTTPS
bool HTTPServer::initializeSSL()
{
    // Inicializar OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Criar contexto SSL
    const SSL_METHOD *method = SSLv23_server_method();
    m_sslContext = SSL_CTX_new(method);
    if (!m_sslContext)
    {
        LOG_ERROR("Failed to create SSL context");
        ERR_print_errors_fp(stderr);
        return false;
    }

    // Configurações de segurança
    SSL_CTX_set_options(m_sslContext, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_options(m_sslContext, SSL_OP_SINGLE_DH_USE);

    // Para desenvolvimento: aceitar certificados auto-assinados
    // Em produção, remover estas linhas e usar certificados válidos
    SSL_CTX_set_verify(m_sslContext, SSL_VERIFY_NONE, nullptr);

    // Configurar modo de segurança mínimo (TLS 1.2+)
    SSL_CTX_set_min_proto_version(m_sslContext, TLS1_2_VERSION);

    LOG_INFO("SSL initialized successfully");
    return true;
}

void HTTPServer::cleanupSSL()
{
    // Fechar todas as conexões SSL de clientes
    for (auto &[fd, ssl] : m_sslClients)
    {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
    }
    m_sslClients.clear();

    // Limpar contexto SSL
    if (m_sslContext)
    {
        SSL_CTX_free(m_sslContext);
        m_sslContext = nullptr;
    }

    EVP_cleanup();
}

SSL *HTTPServer::getSSLContext(int clientFd)
{
    auto it = m_sslClients.find(clientFd);
    if (it != m_sslClients.end())
    {
        return it->second;
    }
    return nullptr;
}
#endif
