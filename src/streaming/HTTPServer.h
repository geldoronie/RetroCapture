#pragma once

#include <string>
#include <map>
#include <cstddef>
#include <sys/types.h>

#ifdef ENABLE_HTTPS
struct ssl_st;
struct ssl_ctx_st;
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
#endif

/**
 * HTTPServer - Wrapper para servidor HTTP/HTTPS
 *
 * Esta classe abstrai a criação e gerenciamento de sockets HTTP/HTTPS,
 * permitindo usar tanto HTTP simples quanto HTTPS com SSL/TLS.
 */
class HTTPServer
{
public:
    HTTPServer();
    ~HTTPServer();

    /**
     * Configurar certificado SSL/TLS para HTTPS
     * @param certPath Caminho para o arquivo de certificado (.crt ou .pem)
     * @param keyPath Caminho para o arquivo de chave privada (.key)
     * @return true se configurado com sucesso, false caso contrário
     */
    bool setSSLCertificate(const std::string &certPath, const std::string &keyPath);

    /**
     * Criar socket do servidor
     * @param port Porta para escutar
     * @return true se criado com sucesso, false caso contrário
     */
    bool createServer(int port);

    /**
     * Aceitar nova conexão de cliente
     * @return File descriptor do cliente, ou -1 se erro
     */
    int acceptClient();

    /**
     * Enviar dados através do socket (com suporte SSL se habilitado)
     * @param clientFd File descriptor do cliente
     * @param data Dados para enviar
     * @param size Tamanho dos dados
     * @return Número de bytes enviados, ou -1 se erro
     */
    ssize_t sendData(int clientFd, const void *data, size_t size);

    /**
     * Receber dados do socket (com suporte SSL se habilitado)
     * @param clientFd File descriptor do cliente
     * @param buffer Buffer para receber dados
     * @param size Tamanho do buffer
     * @return Número de bytes recebidos, ou -1 se erro
     */
    ssize_t receiveData(int clientFd, void *buffer, size_t size);

    /**
     * Fechar conexão do cliente
     * @param clientFd File descriptor do cliente
     */
    void closeClient(int clientFd);

    /**
     * Fechar servidor
     */
    void closeServer();

    /**
     * Verificar se HTTPS está habilitado no servidor
     * @return true se HTTPS está habilitado, false caso contrário
     */
    bool isHTTPS() const { return m_useSSL; }

    /**
     * Verificar se um cliente específico está usando HTTPS
     * @param clientFd File descriptor do cliente
     * @return true se o cliente está usando HTTPS, false caso contrário
     */
    bool isClientHTTPS(int clientFd) const;

    /**
     * Obter URL base (http:// ou https://)
     * @param hostname Nome do host (ex: "localhost")
     * @param port Porta
     * @return URL base
     */
    std::string getBaseUrl(const std::string &hostname, int port) const;

private:
#ifdef ENABLE_HTTPS
    bool initializeSSL();
    void cleanupSSL();
    SSL *getSSLContext(int clientFd);
#endif

    int m_serverSocket = -1;
    bool m_useSSL = false;

#ifdef ENABLE_HTTPS
    SSL_CTX *m_sslContext = nullptr;
    std::map<int, SSL *> m_sslClients; // Mapeia clientFd -> SSL*
#endif
};
