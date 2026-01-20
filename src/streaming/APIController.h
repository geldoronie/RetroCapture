#pragma once

#include <string>
#include <functional>
#include <cstdint>

class UIManager;
class Application;
class HTTPServer;

/**
 * APIController - Controlador REST API para controle remoto do RetroCapture
 *
 * Esta classe gerencia todos os endpoints da API REST que permitem
 * controlar remotamente todas as funcionalidades da aplicação através
 * do portal web ou por terceiros.
 */
class APIController
{
public:
    APIController();
    ~APIController() = default;

    /**
     * Define a referência ao Application para acessar componentes
     * @param application Referência ao Application
     */
    void setApplication(Application *application) { m_application = application; }

    /**
     * Define a referência ao UIManager para acessar configurações
     * @param uiManager Referência ao UIManager
     */
    void setUIManager(UIManager *uiManager) { m_uiManager = uiManager; }

    /**
     * Define a referência ao HTTPServer para enviar dados
     * @param httpServer Referência ao HTTPServer
     */
    void setHTTPServer(HTTPServer *httpServer) { m_httpServer = httpServer; }

    /**
     * Verifica se uma requisição é para a API
     * @param request Requisição HTTP recebida
     * @return true se é uma requisição da API, false caso contrário
     */
    bool isAPIRequest(const std::string &request) const;

    /**
     * Processa uma requisição HTTP da API
     * @param clientFd File descriptor do socket do cliente
     * @param request Requisição HTTP recebida
     * @return true se a requisição foi processada, false caso contrário
     */
    bool handleRequest(int clientFd, const std::string &request);

private:
    /**
     * Extrai o método HTTP da requisição (GET, POST, PUT, DELETE)
     */
    std::string extractMethod(const std::string &request) const;

    /**
     * Extrai o path da requisição (ex: /api/v1/source)
     */
    std::string extractPath(const std::string &request) const;

    /**
     * Extrai o corpo JSON da requisição (para POST/PUT)
     */
    std::string extractBody(const std::string &request) const;

    /**
     * Extrai o header Range da requisição (para Range Requests)
     * Retorna pair<start, end> ou pair<0, 0> se não houver Range header
     */
    std::pair<uint64_t, uint64_t> extractRange(const std::string &request, uint64_t fileSize) const;

    /**
     * Envia resposta JSON
     */
    void sendJSONResponse(int clientFd, int statusCode, const std::string &json) const;

    /**
     * Envia resposta de erro
     */
    void sendErrorResponse(int clientFd, int statusCode, const std::string &message) const;

    /**
     * Envia resposta 404
     */
    void send404(int clientFd) const;

    /**
     * Envia dados através do HTTPServer (suporta SSL se necessário)
     */
    ssize_t sendData(int clientFd, const void *data, size_t size) const;

    // Endpoints GET (leitura)
    bool handleGET(int clientFd, const std::string &path, const std::string &request);
    bool handleGETSource(int clientFd);
    bool handleGETShader(int clientFd);
    bool handleGETShaderList(int clientFd);
    bool handleGETShaderParameters(int clientFd);
    bool handleGETCaptureResolution(int clientFd);
    bool handleGETCaptureFPS(int clientFd);
    bool handleGETImageSettings(int clientFd);
    bool handleGETStreamingSettings(int clientFd);
    bool handleGETRecordingSettings(int clientFd);
    bool handleGETRecordingStatus(int clientFd);
    bool handleGETRecordings(int clientFd);
    bool handleGETRecording(int clientFd, const std::string& recordingId);
    bool handleGETV4L2Devices(int clientFd);
    bool handleGETV4L2Controls(int clientFd);
    bool handleGETStatus(int clientFd);
    bool handleRefreshV4L2Devices(int clientFd);
    bool handleGETPlatform(int clientFd);
    bool handleGETDSDevices(int clientFd);
    bool handleRefreshDSDevices(int clientFd);
    bool handleGETAVFoundationDevices(int clientFd);
    bool handleRefreshAVFoundationDevices(int clientFd);
    bool handleGETAVFoundationAudioDevices(int clientFd);
    bool handleRefreshAVFoundationAudioDevices(int clientFd);
    bool handleGETPresets(int clientFd);
    bool handleGETPreset(int clientFd, const std::string& presetName);
    bool handleGETAudioInputSources(int clientFd);
    bool handleGETAudioStatus(int clientFd);

    // Endpoints POST/PUT (escrita)
    bool handlePOST(int clientFd, const std::string &path, const std::string &body);
    bool handlePUT(int clientFd, const std::string &path, const std::string &body);
    bool handleSetSource(int clientFd, const std::string &body);
    bool handleSetShader(int clientFd, const std::string &body);
    bool handleSetShaderParameter(int clientFd, const std::string &body);
    bool handleSetCaptureResolution(int clientFd, const std::string &body);
    bool handleSetCaptureFPS(int clientFd, const std::string &body);
    bool handleSetImageSettings(int clientFd, const std::string &body);
    bool handleSetStreamingSettings(int clientFd, const std::string &body);
    bool handleSetStreamingControl(int clientFd, const std::string &body);
    bool handleSetRecordingSettings(int clientFd, const std::string &body);
    bool handleSetRecordingControl(int clientFd, const std::string &body);
    bool handleDeleteRecording(int clientFd, const std::string &recordingId);
    bool handlePUTRecording(int clientFd, const std::string &recordingId, const std::string &body);
    bool handleGETRecordingFile(int clientFd, const std::string &recordingId, const std::string &request);
    bool handleGETRecordingThumbnail(int clientFd, const std::string &recordingId);
    bool handleSetV4L2Control(int clientFd, const std::string &body);
    bool handleSetV4L2Device(int clientFd, const std::string &body);
    bool handleSetDSDevice(int clientFd, const std::string &body);
    bool handleGETAVFoundationFormats(int clientFd);
    bool handleSetAVFoundationDevice(int clientFd, const std::string &body);
    bool handleSetAVFoundationFormat(int clientFd, const std::string &body);
    bool handleSetAVFoundationAudioDevice(int clientFd, const std::string &body);
    bool handleCreatePreset(int clientFd, const std::string &body);
    bool handleApplyPreset(int clientFd, const std::string &body);
    bool handleDeletePreset(int clientFd, const std::string &presetName);
    bool handleSetAudioInputSource(int clientFd, const std::string &body);
    bool handleDisconnectAudioInput(int clientFd);

    Application *m_application = nullptr;
    UIManager *m_uiManager = nullptr;
    HTTPServer *m_httpServer = nullptr; // Ponteiro para HTTPServer
};
