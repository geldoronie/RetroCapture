#pragma once

#include <string>
#include <functional>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../utils/ShaderBundle.h"

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

    /**
     * #49 Phase 3 — Updates the password hash used to gate /meta.
     * Pass an empty string to disable auth. Mirrors the behaviour of
     * HTTPTSStreamer::setStreamPasswordHash; Application keeps both
     * in sync with the UI on every frame.
     */
    void setStreamPasswordHash(const std::string &sha256Hex);

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

    /**
     * Reliably send the entire buffer. sendData() does a single non-blocking
     * send() that can return a partial count or 0 (EAGAIN, socket buffer
     * full); this loops until everything is sent so large responses aren't
     * silently truncated. Returns false on a fatal socket error.
     */
    bool sendAll(int clientFd, const char *data, size_t size) const;

    /**
     * Compute a content hash of a preset file for the /meta endpoint.
     * Returns an opaque string (e.g. "fnv1a64:abcd...") used by the remote
     * client to decide whether its locally-cached preset is still valid.
     * Returns empty string if the file cannot be read.
     */
    std::string computePresetHash(const std::string &presetPath) const;

    // #54 — collect a preset's bundle (the .glslp + every .glsl/LUT it
    // references), with paths relative to their common root so the layout
    // survives client extraction. Returns false if the preset can't be read
    // or the bundle exceeds kMaxShaderBundleBytes. Used for both the bundle
    // hash (in /meta) and the /api/v1/shader/bundle endpoint.
    bool collectShaderBundle(const std::string &presetPath,
                             std::vector<shaderbundle::Entry> &out,
                             std::string &rootRelGlslp,
                             uint64_t &totalBytes) const;
    bool handleGETShaderBundle(int clientFd, const std::string &request);
    static constexpr uint64_t kMaxShaderBundleBytes = 8ull * 1024 * 1024; // 8 MB cap

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
    bool handleGETMeta(int clientFd, const std::string &request);
    /**
     * Long-lived Server-Sent Events loop on /meta — pushes snapshot deltas
     * to the connected client every ~250 ms whenever the JSON changes,
     * plus a comment keepalive every 30 s. Returns when the client
     * disconnects or sending fails. Phase 6 of #47.
     */
    bool handleGETMetaSSE(int clientFd);
    /**
     * Builds the /meta JSON snapshot. Pure function over current
     * Application / UIManager / ShaderEngine state — safe to call from
     * the SSE loop on every tick.
     */
    std::string buildMetaSnapshotJSON();
    bool handleRefreshV4L2Devices(int clientFd);
    bool handleGETPlatform(int clientFd);
    bool handleGETDSDevices(int clientFd);
    bool handleRefreshDSDevices(int clientFd);
    bool handleGETPresets(int clientFd);
    bool handleGETPreset(int clientFd, const std::string& presetName);
    bool handleGETSourceOverscan(int clientFd);
    /**
     * GET /api/v1/preferences — exposes the host application's current
     * UI language so the portal can default to it on first load.
     * Read-only; the portal user overrides via header dropdown
     * (persisted in localStorage on the client only — does not affect
     * the host's setting).
     */
    bool handleGETPreferences(int clientFd);
    bool handleSetSourceOverscan(int clientFd, const std::string& body);
    bool handleGETAudioInputSources(int clientFd);
    bool handleGETAudioStatus(int clientFd);
#ifdef __APPLE__
    // AVFoundation device + format endpoints (macOS only). The format
    // dropdown follows OBS's "pick a (resolution, fps range, pixel
    // format) tuple atomically" pattern instead of independent
    // resolution/FPS sliders.
    bool handleGETAVFoundationDevices(int clientFd);
    bool handleGETAVFoundationFormats(int clientFd, const std::string &deviceId);
    bool handleGETAVFoundationAudioDevices(int clientFd);
#endif

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
    bool handleCreatePreset(int clientFd, const std::string &body);
    bool handleApplyPreset(int clientFd, const std::string &body);
    bool handleDeletePreset(int clientFd, const std::string &presetName);
    bool handleUpdatePresetParameters(int clientFd, const std::string &presetName, const std::string &body);
    bool handleSetAudioInputSource(int clientFd, const std::string &body);
    bool handleDisconnectAudioInput(int clientFd);
    bool handleResyncAudioMonitor(int clientFd);
#ifdef __APPLE__
    bool handleSetAVFoundationDevice(int clientFd, const std::string &body);
    bool handleSetAVFoundationFormat(int clientFd, const std::string &body);
    bool handleSetAVFoundationAudioDevice(int clientFd, const std::string &body);
#endif

    // Recording profiles (saved snapshots of recording configuration)
    bool handleGETRecordingProfiles(int clientFd);
    bool handleSaveRecordingProfile(int clientFd, const std::string &body);
    bool handleApplyRecordingProfile(int clientFd, const std::string &name);
    bool handleDeleteRecordingProfile(int clientFd, const std::string &name);

    // Streaming profiles (saved snapshots of streaming configuration)
    bool handleGETStreamingProfiles(int clientFd);
    bool handleSaveStreamingProfile(int clientFd, const std::string &body);
    bool handleApplyStreamingProfile(int clientFd, const std::string &name);
    bool handleDeleteStreamingProfile(int clientFd, const std::string &name);

    Application *m_application = nullptr;
    UIManager *m_uiManager = nullptr;
    HTTPServer *m_httpServer = nullptr; // Ponteiro para HTTPServer

    // #49 Phase 3 — sha256(password) hex; empty == no auth.
    mutable std::mutex m_passwordMu;
    std::string        m_streamPasswordHash;

    // #54 — cache of the active preset's bundle hash so the ~1 Hz /meta
    // poll doesn't re-read every shader file each time. Invalidated when the
    // preset path or the .glslp's mtime changes.
    mutable std::mutex  m_bundleHashMu;
    mutable std::string m_bundleHashPath;   // presetPath the cache is for
    mutable int64_t     m_bundleHashMtime = 0;
    mutable std::string m_bundleHashValue;  // cached bundle hash
};
