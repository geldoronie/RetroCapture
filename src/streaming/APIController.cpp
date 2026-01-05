#include "APIController.h"
#include "../core/Application.h"
#include "../ui/UIManager.h"
#include "../shader/ShaderEngine.h"
#include "HTTPServer.h"
#include "../utils/Logger.h"
#include "../utils/PresetManager.h"
#include "../recording/RecordingSettings.h"
#include "../recording/RecordingMetadata.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <vector>
#include <fstream>
#include <cstdint>
#include <climits>
#include "../utils/FilesystemCompat.h"

// Simple JSON helper functions
namespace
{
    std::string jsonEscape(const std::string &str)
    {
        std::string result;
        result.reserve(str.length() + 10);
        for (char c : str)
        {
            switch (c)
            {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
                break;
            }
        }
        return result;
    }

    std::string jsonString(const std::string &str)
    {
        return "\"" + jsonEscape(str) + "\"";
    }

    std::string jsonNumber(int value)
    {
        return std::to_string(value);
    }

    std::string jsonNumber(uint32_t value)
    {
        return std::to_string(value);
    }

    std::string jsonNumber(int64_t value)
    {
        return std::to_string(value);
    }

    std::string jsonNumber(uint64_t value)
    {
        return std::to_string(value);
    }

    std::string jsonNumber(float value)
    {
        std::ostringstream oss;
        oss.precision(6);
        oss << std::fixed << value;
        std::string str = oss.str();
        // Remove trailing zeros
        str.erase(str.find_last_not_of('0') + 1, std::string::npos);
        str.erase(str.find_last_not_of('.') + 1, std::string::npos);
        return str;
    }

    std::string jsonBool(bool value)
    {
        return value ? "true" : "false";
    }
}

APIController::APIController()
{
}

bool APIController::isAPIRequest(const std::string &request) const
{
    return request.find("/api/") != std::string::npos;
}

bool APIController::handleRequest(int clientFd, const std::string &request)
{
    if (!m_httpServer)
    {
        LOG_ERROR("APIController: HTTPServer not set");
        return false;
    }

    std::string method = extractMethod(request);
    std::string path = extractPath(request);

    if (method == "GET")
    {
        return handleGET(clientFd, path, request);
    }
    else if (method == "POST" || method == "PUT")
    {
        std::string body = extractBody(request);
        if (method == "POST")
        {
            return handlePOST(clientFd, path, body);
        }
        else
        {
            return handlePUT(clientFd, path, body);
        }
    }
    else if (method == "DELETE")
    {
        // Handle DELETE requests (e.g., /api/v1/presets/{name}, /api/v1/recordings/{id})
        if (path.find("/api/v1/presets/") == 0)
        {
            std::string presetName = path.substr(16); // Length of "/api/v1/presets/"
            if (!presetName.empty())
            {
                return handleDeletePreset(clientFd, presetName);
            }
        }
        else if (path.find("/api/v1/recordings/") == 0)
        {
            std::string recordingId = path.substr(19); // Length of "/api/v1/recordings/"
            if (!recordingId.empty())
            {
                return handleDeleteRecording(clientFd, recordingId);
            }
        }
        send404(clientFd);
        return true;
    }
    else if (method == "OPTIONS")
    {
        // CORS preflight
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Access-Control-Allow-Origin: *\r\n";
        response << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
        response << "Access-Control-Allow-Headers: Content-Type\r\n";
        response << "Content-Length: 0\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        sendData(clientFd, response.str().c_str(), response.str().length());
        return true;
    }

    send404(clientFd);
    return true;
}

std::string APIController::extractMethod(const std::string &request) const
{
    if (request.find("GET ") == 0)
        return "GET";
    if (request.find("POST ") == 0)
        return "POST";
    if (request.find("PUT ") == 0)
        return "PUT";
    if (request.find("DELETE ") == 0)
        return "DELETE";
    if (request.find("OPTIONS ") == 0)
        return "OPTIONS";
    return "";
}

std::string APIController::extractPath(const std::string &request) const
{
    size_t start = request.find(" /api/");
    if (start == std::string::npos)
        return "";

    start += 1; // Skip space
    size_t end = request.find(" ", start);
    if (end == std::string::npos)
        end = request.find("\r\n", start);
    if (end == std::string::npos)
        end = request.find("\n", start);

    if (end == std::string::npos)
        return "";

    std::string path = request.substr(start, end - start);

    // Remove query string
    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos)
    {
        path = path.substr(0, queryPos);
    }

    return path;
}

std::string APIController::extractBody(const std::string &request) const
{
    size_t bodyStart = request.find("\r\n\r\n");
    if (bodyStart == std::string::npos)
        bodyStart = request.find("\n\n");
    if (bodyStart == std::string::npos)
        return "";

    bodyStart += 4; // Skip \r\n\r\n or \n\n
    return request.substr(bodyStart);
}

std::pair<uint64_t, uint64_t> APIController::extractRange(const std::string &request, uint64_t fileSize) const
{
    // Procurar pelo header Range: bytes=start-end
    size_t rangePos = request.find("Range: bytes=");
    if (rangePos == std::string::npos)
    {
        rangePos = request.find("range: bytes=");
    }
    if (rangePos == std::string::npos)
    {
        // Return special value to indicate no range (use UINT64_MAX as marker)
        return std::make_pair(UINT64_MAX, UINT64_MAX);
    }

    rangePos += 13; // Skip "Range: bytes="
    size_t rangeEnd = request.find("\r\n", rangePos);
    if (rangeEnd == std::string::npos)
    {
        rangeEnd = request.find("\n", rangePos);
    }
    if (rangeEnd == std::string::npos)
    {
        return std::make_pair(UINT64_MAX, UINT64_MAX);
    }

    std::string rangeStr = request.substr(rangePos, rangeEnd - rangePos);
    
    // Parse "start-end" ou "start-"
    size_t dashPos = rangeStr.find('-');
    if (dashPos == std::string::npos)
    {
        return std::make_pair(UINT64_MAX, UINT64_MAX);
    }

    uint64_t start = 0;
    uint64_t end = fileSize - 1;

    if (dashPos > 0)
    {
        start = std::stoull(rangeStr.substr(0, dashPos));
    }

    if (dashPos < rangeStr.length() - 1)
    {
        end = std::stoull(rangeStr.substr(dashPos + 1));
    }

    // Validar range
    if (start >= fileSize || end >= fileSize || start > end)
    {
        return std::make_pair(UINT64_MAX, UINT64_MAX);
    }

    return std::make_pair(start, end);
}

void APIController::sendJSONResponse(int clientFd, int statusCode, const std::string &json) const
{
    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << " " << (statusCode == 200 ? "OK" : "Error") << "\r\n";
    response << "Content-Type: application/json\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Content-Length: " << json.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << json;

    std::string responseStr = response.str();
    sendData(clientFd, responseStr.c_str(), responseStr.length());
}

void APIController::sendErrorResponse(int clientFd, int statusCode, const std::string &message) const
{
    std::ostringstream json;
    json << "{\"error\": " << jsonString(message) << ", \"status\": " << statusCode << "}";
    sendJSONResponse(clientFd, statusCode, json.str());
}

void APIController::send404(int clientFd) const
{
    sendErrorResponse(clientFd, 404, "Not Found");
}

ssize_t APIController::sendData(int clientFd, const void *data, size_t size) const
{
    if (!m_httpServer)
        return -1;
    return m_httpServer->sendData(clientFd, data, size);
}

bool APIController::handleGET(int clientFd, const std::string &path, const std::string &request)
{
    if (path == "/api/v1/source")
    {
        return handleGETSource(clientFd);
    }
    else if (path == "/api/v1/shader")
    {
        return handleGETShader(clientFd);
    }
    else if (path == "/api/v1/shader/list")
    {
        return handleGETShaderList(clientFd);
    }
    else if (path == "/api/v1/shader/parameters")
    {
        return handleGETShaderParameters(clientFd);
    }
    else if (path == "/api/v1/capture/resolution")
    {
        return handleGETCaptureResolution(clientFd);
    }
    else if (path == "/api/v1/capture/fps")
    {
        return handleGETCaptureFPS(clientFd);
    }
    else if (path == "/api/v1/image/settings")
    {
        return handleGETImageSettings(clientFd);
    }
    else if (path == "/api/v1/streaming/settings")
    {
        return handleGETStreamingSettings(clientFd);
    }
    else if (path == "/api/v1/recording/settings")
    {
        return handleGETRecordingSettings(clientFd);
    }
    else if (path == "/api/v1/recording/status")
    {
        return handleGETRecordingStatus(clientFd);
    }
    else if (path == "/api/v1/recordings")
    {
        return handleGETRecordings(clientFd);
    }
    else if (path.find("/api/v1/recordings/") == 0)
    {
        std::string remaining = path.substr(19); // Length of "/api/v1/recordings/"
        
        // Check if it's /api/v1/recordings/{id}/file
        size_t filePos = remaining.find("/file");
        if (filePos != std::string::npos)
        {
            // Extract recording ID from path: /api/v1/recordings/{id}/file
            std::string recordingId = remaining.substr(0, filePos);
            if (!recordingId.empty())
            {
                return handleGETRecordingFile(clientFd, recordingId, request);
            }
        }
        // Check if it's /api/v1/recordings/{id}/thumbnail
        else if (remaining.find("/thumbnail") != std::string::npos)
        {
            size_t thumbPos = remaining.find("/thumbnail");
            std::string recordingId = remaining.substr(0, thumbPos);
            if (!recordingId.empty())
            {
                return handleGETRecordingThumbnail(clientFd, recordingId);
            }
        }
        else if (!remaining.empty())
        {
            // Extract recording ID from path: /api/v1/recordings/{id}
            return handleGETRecording(clientFd, remaining);
        }
    }
    else if (path == "/api/v1/v4l2/devices")
    {
        return handleGETV4L2Devices(clientFd);
    }
    else if (path == "/api/v1/v4l2/devices/refresh")
    {
        return handleRefreshV4L2Devices(clientFd);
    }
    else if (path == "/api/v1/v4l2/controls")
    {
        return handleGETV4L2Controls(clientFd);
    }
    else if (path == "/api/v1/ds/devices")
    {
        return handleGETDSDevices(clientFd);
    }
    else if (path == "/api/v1/ds/devices/refresh")
    {
        return handleRefreshDSDevices(clientFd);
    }
    else if (path == "/api/v1/status")
    {
        return handleGETStatus(clientFd);
    }
    else if (path == "/api/v1/platform")
    {
        return handleGETPlatform(clientFd);
    }
    else if (path == "/api/v1/presets")
    {
        return handleGETPresets(clientFd);
    }
    else if (path.find("/api/v1/presets/") == 0)
    {
        // Extract preset name from path: /api/v1/presets/{name}
        std::string presetName = path.substr(16); // Length of "/api/v1/presets/"
        if (!presetName.empty())
        {
            return handleGETPreset(clientFd, presetName);
        }
    }

    send404(clientFd);
    return true;
}

bool APIController::handlePOST(int clientFd, const std::string &path, const std::string &body)
{
    if (path == "/api/v1/source")
    {
        return handleSetSource(clientFd, body);
    }
    else if (path == "/api/v1/shader")
    {
        return handleSetShader(clientFd, body);
    }
    else if (path == "/api/v1/shader/parameter")
    {
        return handleSetShaderParameter(clientFd, body);
    }
    else if (path == "/api/v1/capture/resolution")
    {
        return handleSetCaptureResolution(clientFd, body);
    }
    else if (path == "/api/v1/capture/fps")
    {
        return handleSetCaptureFPS(clientFd, body);
    }
    else if (path == "/api/v1/image/settings")
    {
        return handleSetImageSettings(clientFd, body);
    }
    else if (path == "/api/v1/streaming/settings")
    {
        return handleSetStreamingSettings(clientFd, body);
    }
    else if (path == "/api/v1/streaming/control")
    {
        return handleSetStreamingControl(clientFd, body);
    }
    else if (path == "/api/v1/recording/settings")
    {
        return handleSetRecordingSettings(clientFd, body);
    }
    else if (path == "/api/v1/recording/control")
    {
        return handleSetRecordingControl(clientFd, body);
    }
    else if (path == "/api/v1/v4l2/control")
    {
        return handleSetV4L2Control(clientFd, body);
    }
    else if (path == "/api/v1/v4l2/device")
    {
        return handleSetV4L2Device(clientFd, body);
    }
    else if (path == "/api/v1/ds/device")
    {
        return handleSetDSDevice(clientFd, body);
    }
    else if (path == "/api/v1/presets")
    {
        return handleCreatePreset(clientFd, body);
    }
    else if (path.find("/api/v1/presets/") == 0 && path.find("/apply") != std::string::npos)
    {
        // Extract preset name from path: /api/v1/presets/{name}/apply
        std::string fullPath = path;
        size_t applyPos = fullPath.find("/apply");
        std::string presetName = fullPath.substr(16, applyPos - 16); // Length of "/api/v1/presets/"
        if (!presetName.empty())
        {
            // Create JSON body with preset name if body is empty
            std::string requestBody = body;
            if (requestBody.empty())
            {
                requestBody = "{\"name\": \"" + presetName + "\"}";
            }
            return handleApplyPreset(clientFd, requestBody);
        }
    }

    send404(clientFd);
    return true;
}

bool APIController::handlePUT(int clientFd, const std::string &path, const std::string &body)
{
    // Handle PUT /api/v1/recordings/{id} for renaming
    if (path.find("/api/v1/recordings/") == 0)
    {
        std::string recordingId = path.substr(19); // Length of "/api/v1/recordings/"
        if (!recordingId.empty() && recordingId.find("/") == std::string::npos)
        {
            return handlePUTRecording(clientFd, recordingId, body);
        }
    }
    
    // PUT usa os mesmos handlers que POST para outros endpoints
    return handlePOST(clientFd, path, body);
}

// Implementações dos handlers GET
bool APIController::handleGETSource(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{\"type\": " << static_cast<int>(m_uiManager->getSourceType())
         << ", \"device\": " << jsonString(m_uiManager->getCurrentDevice()) << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETShader(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{\"name\": " << jsonString(m_uiManager->getCurrentShader()) << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETShaderList(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    const auto &shaders = m_uiManager->getShaderList();
    std::ostringstream json;
    json << "{\"shaders\": [";
    for (size_t i = 0; i < shaders.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        json << jsonString(shaders[i]);
    }
    json << "]}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETShaderParameters(int clientFd)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    ShaderEngine *shaderEngine = m_application->getShaderEngine();
    if (!shaderEngine || !shaderEngine->isShaderActive())
    {
        std::ostringstream json;
        json << "{\"parameters\": []}";
        sendJSONResponse(clientFd, 200, json.str());
        return true;
    }

    auto params = shaderEngine->getShaderParameters();
    std::ostringstream json;
    json << "{\"parameters\": [";
    for (size_t i = 0; i < params.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        const auto &param = params[i];
        json << "{"
             << "\"name\": " << jsonString(param.name) << ", "
             << "\"value\": " << jsonNumber(param.value) << ", "
             << "\"defaultValue\": " << jsonNumber(param.defaultValue) << ", "
             << "\"min\": " << jsonNumber(param.min) << ", "
             << "\"max\": " << jsonNumber(param.max) << ", "
             << "\"step\": " << jsonNumber(param.step) << ", "
             << "\"description\": " << jsonString(param.description)
             << "}";
    }
    json << "]}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETCaptureResolution(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{\"width\": " << jsonNumber(m_uiManager->getCaptureWidth())
         << ", \"height\": " << jsonNumber(m_uiManager->getCaptureHeight()) << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETCaptureFPS(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{\"fps\": " << jsonNumber(m_uiManager->getCaptureFps()) << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETImageSettings(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{"
         << "\"brightness\": " << jsonNumber(m_uiManager->getBrightness()) << ", "
         << "\"contrast\": " << jsonNumber(m_uiManager->getContrast()) << ", "
         << "\"maintainAspect\": " << jsonBool(m_uiManager->getMaintainAspect()) << ", "
         << "\"fullscreen\": " << jsonBool(m_uiManager->getFullscreen()) << ", "
         << "\"monitorIndex\": " << jsonNumber(m_uiManager->getMonitorIndex()) << ", "
         << "\"outputWidth\": " << jsonNumber(m_uiManager->getOutputWidth()) << ", "
         << "\"outputHeight\": " << jsonNumber(m_uiManager->getOutputHeight())
         << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETStreamingSettings(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{"
         << "\"port\": " << jsonNumber(m_uiManager->getStreamingPort()) << ", "
         << "\"width\": " << jsonNumber(m_uiManager->getStreamingWidth()) << ", "
         << "\"height\": " << jsonNumber(m_uiManager->getStreamingHeight()) << ", "
         << "\"fps\": " << jsonNumber(m_uiManager->getStreamingFps()) << ", "
         << "\"bitrate\": " << jsonNumber(m_uiManager->getStreamingBitrate()) << ", "
         << "\"audioBitrate\": " << jsonNumber(m_uiManager->getStreamingAudioBitrate()) << ", "
         << "\"videoCodec\": " << jsonString(m_uiManager->getStreamingVideoCodec()) << ", "
         << "\"audioCodec\": " << jsonString(m_uiManager->getStreamingAudioCodec()) << ", "
         << "\"h264Preset\": " << jsonString(m_uiManager->getStreamingH264Preset()) << ", "
         << "\"h265Preset\": " << jsonString(m_uiManager->getStreamingH265Preset()) << ", "
         << "\"h265Profile\": " << jsonString(m_uiManager->getStreamingH265Profile()) << ", "
         << "\"h265Level\": " << jsonString(m_uiManager->getStreamingH265Level()) << ", "
         << "\"vp8Speed\": " << jsonNumber(m_uiManager->getStreamingVP8Speed()) << ", "
         << "\"vp9Speed\": " << jsonNumber(m_uiManager->getStreamingVP9Speed())
         << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETV4L2Devices(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    // Garantir que os dispositivos sejam escaneados se a lista estiver vazia
    // (mesmo comportamento da interface local)
    if (m_uiManager->getV4L2Devices().empty())
    {
        m_uiManager->refreshV4L2Devices();
    }

    const auto &devices = m_uiManager->getV4L2Devices();
    std::ostringstream json;
    json << "{\"devices\": [";
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        json << jsonString(devices[i]);
    }
    json << "]}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleRefreshV4L2Devices(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    // Forçar refresh dos dispositivos
    m_uiManager->refreshV4L2Devices();

    // Retornar a lista atualizada
    const auto &devices = m_uiManager->getV4L2Devices();
    std::ostringstream json;
    json << "{\"devices\": [";
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        json << jsonString(devices[i]);
    }
    json << "]}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETV4L2Controls(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    const auto &controls = m_uiManager->getV4L2Controls();
    std::ostringstream json;
    json << "{\"controls\": [";
    for (size_t i = 0; i < controls.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        const auto &ctrl = controls[i];
        json << "{"
             << "\"name\": " << jsonString(ctrl.name) << ", "
             << "\"value\": " << jsonNumber(ctrl.value) << ", "
             << "\"min\": " << jsonNumber(ctrl.min) << ", "
             << "\"max\": " << jsonNumber(ctrl.max) << ", "
             << "\"step\": " << jsonNumber(ctrl.step) << ", "
             << "\"available\": " << jsonBool(ctrl.available)
             << "}";
    }
    json << "]}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETStatus(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{"
         << "\"streamingActive\": " << jsonBool(m_uiManager->getStreamingActive()) << ", "
         << "\"streamingCanStart\": " << jsonBool(m_uiManager->canStartStreaming()) << ", "
         << "\"streamingCooldownRemainingMs\": " << jsonNumber(m_uiManager->getStreamingCooldownRemainingMs()) << ", "
         << "\"streamUrl\": " << jsonString(m_uiManager->getStreamUrl()) << ", "
         << "\"clientCount\": " << jsonNumber(m_uiManager->getStreamClientCount())
         << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETPlatform(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{"
         << "\"platform\": " << jsonString(
#ifdef _WIN32
                                    "windows"
#else
                                    "linux"
#endif
                                    )
         << ", "
         << "\"availableSourceTypes\": [";

    // Adicionar tipos de source disponíveis baseado na plataforma
#ifdef _WIN32
    json << "{\"value\": 0, \"name\": \"None\"}, "
         << "{\"value\": 2, \"name\": \"DirectShow\"}";
#else
    json << "{\"value\": 0, \"name\": \"None\"}, "
         << "{\"value\": 1, \"name\": \"V4L2\"}";
#endif

    json << "]"
         << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETDSDevices(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    const auto &devices = m_uiManager->getDSDevices();
    std::ostringstream json;
    json << "{\"devices\": [";
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        const auto &device = devices[i];
        json << "{"
             << "\"id\": " << jsonString(device.id) << ", "
             << "\"name\": " << jsonString(device.name) << ", "
             << "\"available\": " << jsonBool(device.available)
             << "}";
    }
    json << "]}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleRefreshDSDevices(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    m_uiManager->refreshDSDevices();
    return handleGETDSDevices(clientFd);
}

// Implementações dos handlers POST/PUT
bool APIController::handleSetSource(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("type"))
        {
            int sourceTypeInt = json["type"].get<int>();
            UIManager::SourceType sourceType = static_cast<UIManager::SourceType>(sourceTypeInt);

            // Disparar callback via método auxiliar do UIManager
            m_uiManager->triggerSourceTypeChange(sourceType);

            std::ostringstream response;
            response << "{\"success\": true, \"type\": " << sourceTypeInt << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'type' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetShader(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("shader"))
        {
            std::string shader = json["shader"].get<std::string>();

            // setCurrentShader agora dispara o callback automaticamente
            m_uiManager->setCurrentShader(shader);

            std::ostringstream response;
            response << "{\"success\": true, \"shader\": " << jsonString(shader) << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'shader' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetShaderParameter(int clientFd, const std::string &body)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    ShaderEngine *shaderEngine = m_application->getShaderEngine();
    if (!shaderEngine || !shaderEngine->isShaderActive())
    {
        sendErrorResponse(clientFd, 400, "No shader active");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("name") && json.contains("value"))
        {
            std::string name = json["name"].get<std::string>();
            float value = json["value"].get<float>();

            if (shaderEngine->setShaderParameter(name, value))
            {
                std::ostringstream response;
                response << "{\"success\": true, \"name\": " << jsonString(name)
                         << ", \"value\": " << jsonNumber(value) << "}";
                sendJSONResponse(clientFd, 200, response.str());
                return true;
            }
            else
            {
                sendErrorResponse(clientFd, 400, "Failed to set shader parameter");
                return true;
            }
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'name' or 'value' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetCaptureResolution(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("width") && json.contains("height"))
        {
            uint32_t width = json["width"].get<uint32_t>();
            uint32_t height = json["height"].get<uint32_t>();

            // Disparar callback via método auxiliar do UIManager
            m_uiManager->triggerResolutionChange(width, height);

            std::ostringstream response;
            response << "{\"success\": true, \"width\": " << width << ", \"height\": " << height << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'width' or 'height' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetCaptureFPS(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("fps"))
        {
            uint32_t fps = json["fps"].get<uint32_t>();

            // Disparar callback via método auxiliar do UIManager
            m_uiManager->triggerFramerateChange(fps);

            std::ostringstream response;
            response << "{\"success\": true, \"fps\": " << fps << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'fps' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetImageSettings(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        bool updated = false;

        if (json.contains("brightness"))
        {
            float brightness = json["brightness"].get<float>();
            m_uiManager->setBrightness(brightness);
            updated = true;
        }

        if (json.contains("contrast"))
        {
            float contrast = json["contrast"].get<float>();
            m_uiManager->setContrast(contrast);
            updated = true;
        }

        if (json.contains("maintainAspect"))
        {
            bool maintainAspect = json["maintainAspect"].get<bool>();
            m_uiManager->setMaintainAspect(maintainAspect);
            updated = true;
        }

        if (json.contains("fullscreen"))
        {
            bool fullscreen = json["fullscreen"].get<bool>();
            m_uiManager->setFullscreen(fullscreen);
            updated = true;
        }

        if (json.contains("monitorIndex"))
        {
            int monitorIndex = json["monitorIndex"].get<int>();
            m_uiManager->setMonitorIndex(monitorIndex);
            updated = true;
        }

        if (json.contains("outputWidth") && json.contains("outputHeight"))
        {
            uint32_t outputWidth = json["outputWidth"].get<uint32_t>();
            uint32_t outputHeight = json["outputHeight"].get<uint32_t>();
            m_uiManager->setOutputResolution(outputWidth, outputHeight);
            updated = true;
        }

        std::ostringstream response;
        response << "{\"success\": " << jsonBool(updated) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetStreamingControl(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);

        if (!json.contains("action"))
        {
            sendErrorResponse(clientFd, 400, "Missing 'action' field. Use 'start' or 'stop'");
            return true;
        }

        std::string action = json["action"].get<std::string>();

        if (action == "start")
        {
            // Verificar se pode iniciar (não está em cooldown)
            if (!m_uiManager->canStartStreaming())
            {
                int64_t cooldownMs = m_uiManager->getStreamingCooldownRemainingMs();
                int cooldownSeconds = static_cast<int>(cooldownMs / 1000);
                std::ostringstream response;
                response << "{\"success\": false, \"action\": \"start\", \"message\": \"Streaming ainda em cooldown. Aguarde "
                         << cooldownSeconds << " segundos\", \"cooldownRemainingMs\": " << cooldownMs << "}";
                sendJSONResponse(clientFd, 429, response.str()); // 429 = Too Many Requests
                return true;
            }

            m_uiManager->triggerStreamingStartStop(true);
            std::ostringstream response;
            response << "{\"success\": true, \"action\": \"start\", \"message\": \"Streaming iniciado\"}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else if (action == "stop")
        {
            m_uiManager->triggerStreamingStartStop(false);
            std::ostringstream response;
            response << "{\"success\": true, \"action\": \"stop\", \"message\": \"Streaming parado\"}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Invalid 'action' value. Use 'start' or 'stop'");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetStreamingSettings(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        bool updated = false;

        // Atualizar apenas campos presentes no JSON
        if (json.contains("port"))
        {
            uint16_t port = json["port"].get<uint16_t>();
            m_uiManager->setStreamingPort(port);
            updated = true;
        }

        if (json.contains("width"))
        {
            uint32_t width = json["width"].get<uint32_t>();
            m_uiManager->setStreamingWidth(width);
            updated = true;
        }

        if (json.contains("height"))
        {
            uint32_t height = json["height"].get<uint32_t>();
            m_uiManager->setStreamingHeight(height);
            updated = true;
        }

        if (json.contains("fps"))
        {
            uint32_t fps = json["fps"].get<uint32_t>();
            m_uiManager->setStreamingFps(fps);
            updated = true;
        }

        if (json.contains("bitrate"))
        {
            uint32_t bitrate = json["bitrate"].get<uint32_t>();
            m_uiManager->setStreamingBitrate(bitrate);
            updated = true;
        }

        if (json.contains("audioBitrate"))
        {
            uint32_t audioBitrate = json["audioBitrate"].get<uint32_t>();
            m_uiManager->setStreamingAudioBitrate(audioBitrate);
            updated = true;
        }

        if (json.contains("videoCodec"))
        {
            std::string codec = json["videoCodec"].get<std::string>();
            m_uiManager->setStreamingVideoCodec(codec);
            updated = true;
        }

        if (json.contains("audioCodec"))
        {
            std::string codec = json["audioCodec"].get<std::string>();
            m_uiManager->setStreamingAudioCodec(codec);
            updated = true;
        }

        if (json.contains("h264Preset"))
        {
            std::string preset = json["h264Preset"].get<std::string>();
            m_uiManager->setStreamingH264Preset(preset);
            updated = true;
        }

        if (json.contains("h265Preset"))
        {
            std::string preset = json["h265Preset"].get<std::string>();
            m_uiManager->setStreamingH265Preset(preset);
            updated = true;
        }

        if (json.contains("h265Profile"))
        {
            std::string profile = json["h265Profile"].get<std::string>();
            m_uiManager->setStreamingH265Profile(profile);
            updated = true;
        }

        if (json.contains("h265Level"))
        {
            std::string level = json["h265Level"].get<std::string>();
            m_uiManager->setStreamingH265Level(level);
            updated = true;
        }

        if (json.contains("vp8Speed"))
        {
            int speed = json["vp8Speed"].get<int>();
            m_uiManager->setStreamingVP8Speed(speed);
            updated = true;
        }

        if (json.contains("vp9Speed"))
        {
            int speed = json["vp9Speed"].get<int>();
            m_uiManager->setStreamingVP9Speed(speed);
            updated = true;
        }

        std::ostringstream response;
        response << "{\"success\": " << jsonBool(updated) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetV4L2Control(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("name") && json.contains("value"))
        {
            std::string name = json["name"].get<std::string>();
            int32_t value = json["value"].get<int32_t>();

            // Disparar callback via método auxiliar do UIManager
            m_uiManager->triggerV4L2ControlChange(name, value);

            std::ostringstream response;
            response << "{\"success\": true, \"name\": " << jsonString(name)
                     << ", \"value\": " << jsonNumber(value) << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'name' or 'value' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetV4L2Device(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("device"))
        {
            std::string device = json["device"].get<std::string>();

            // setCurrentDevice agora dispara o callback automaticamente
            m_uiManager->setCurrentDevice(device);

            std::ostringstream response;
            response << "{\"success\": true, \"device\": " << jsonString(device) << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'device' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetDSDevice(int clientFd, const std::string &body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (json.contains("device"))
        {
            std::string device = json["device"].get<std::string>();

            // setCurrentDevice agora dispara o callback automaticamente
            m_uiManager->setCurrentDevice(device);

            std::ostringstream response;
            response << "{\"success\": true, \"device\": " << jsonString(device) << "}";
            sendJSONResponse(clientFd, 200, response.str());
            return true;
        }
        else
        {
            sendErrorResponse(clientFd, 400, "Missing 'device' field");
            return true;
        }
    }
    catch (const std::exception &e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

// Preset handlers
bool APIController::handleGETPresets(int clientFd)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        PresetManager presetManager;
        std::vector<std::string> presetNames = presetManager.listPresets();
        
        std::ostringstream response;
        response << "{\"presets\": [";
        
        bool first = true;
        for (const auto& name : presetNames)
        {
            if (!first)
                response << ",";
            first = false;
            
            PresetManager::PresetData data;
            if (presetManager.loadPreset(name, data))
            {
                response << "{";
                response << "\"name\": " << jsonString(name) << ",";
                response << "\"displayName\": " << jsonString(data.name.empty() ? name : data.name) << ",";
                response << "\"description\": " << jsonString(data.description) << ",";
                response << "\"created\": " << jsonString(data.created) << ",";
                response << "\"thumbnail\": " << jsonString(data.thumbnailPath);
                response << "}";
            }
            else
            {
                // Fallback if preset can't be loaded
                response << "{";
                response << "\"name\": " << jsonString(name) << ",";
                response << "\"displayName\": " << jsonString(name) << ",";
                response << "\"description\": \"\",";
                response << "\"created\": \"\",";
                response << "\"thumbnail\": \"\"";
                response << "}";
            }
        }
        
        response << "]}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error listing presets: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleGETPreset(int clientFd, const std::string& presetName)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        PresetManager presetManager;
        PresetManager::PresetData data;
        
        if (!presetManager.loadPreset(presetName, data))
        {
            sendErrorResponse(clientFd, 404, "Preset not found: " + presetName);
            return true;
        }
        
        std::ostringstream response;
        response << "{";
        response << "\"name\": " << jsonString(presetName) << ",";
        response << "\"displayName\": " << jsonString(data.name.empty() ? presetName : data.name) << ",";
        response << "\"description\": " << jsonString(data.description) << ",";
        response << "\"created\": " << jsonString(data.created) << ",";
        response << "\"thumbnail\": " << jsonString(data.thumbnailPath) << ",";
        response << "\"shader\": {";
        response << "\"path\": " << jsonString(data.shaderPath) << ",";
        response << "\"parameters\": {";
        bool firstParam = true;
        for (const auto& param : data.shaderParameters)
        {
            if (!firstParam)
                response << ",";
            firstParam = false;
            response << jsonString(param.first) << ": " << jsonNumber(param.second);
        }
        response << "}},";
        response << "\"capture\": {";
        response << "\"width\": " << data.captureWidth << ",";
        response << "\"height\": " << data.captureHeight << ",";
        response << "\"fps\": " << data.captureFps << ",";
        response << "\"sourceType\": " << data.sourceType;
        response << "},";
        response << "\"image\": {";
        response << "\"brightness\": " << jsonNumber(data.imageBrightness) << ",";
        response << "\"contrast\": " << jsonNumber(data.imageContrast) << ",";
        response << "\"maintainAspect\": " << jsonBool(data.maintainAspect);
        response << "}";
        response << "}";
        
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error loading preset: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleCreatePreset(int clientFd, const std::string& body)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        
        if (!json.contains("name") || json["name"].empty())
        {
            sendErrorResponse(clientFd, 400, "Missing or empty 'name' field");
            return true;
        }
        
        std::string name = json["name"].get<std::string>();
        std::string description = json.contains("description") ? json["description"].get<std::string>() : "";
        bool captureThumbnail = json.contains("captureThumbnail") && json["captureThumbnail"].get<bool>();
        
        // Create preset from current state (handles thumbnail capture internally)
        m_application->createPresetFromCurrentState(name, description, captureThumbnail);
        
        std::ostringstream response;
        response << "{\"success\": true, \"name\": " << jsonString(name) << "}";
        sendJSONResponse(clientFd, 201, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleApplyPreset(int clientFd, const std::string& body)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        // Extract preset name from body
        nlohmann::json json;
        std::string presetName;
        
        if (!body.empty())
        {
            json = nlohmann::json::parse(body);
            if (json.contains("name"))
            {
                presetName = json["name"].get<std::string>();
            }
        }
        
        if (presetName.empty())
        {
            sendErrorResponse(clientFd, 400, "Missing 'name' field");
            return true;
        }
        
        // Schedule preset application for main thread (thread-safe)
        m_application->schedulePresetApplication(presetName);
        
        std::ostringstream response;
        response << "{\"success\": true, \"name\": " << jsonString(presetName) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleDeletePreset(int clientFd, const std::string& presetName)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        PresetManager presetManager;
        
        if (!presetManager.deletePreset(presetName))
        {
            sendErrorResponse(clientFd, 404, "Preset not found: " + presetName);
            return true;
        }
        
        std::ostringstream response;
        response << "{\"success\": true, \"name\": " << jsonString(presetName) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error deleting preset: " + std::string(e.what()));
        return true;
    }
}

// Recording handlers
bool APIController::handleGETRecordingSettings(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    std::ostringstream json;
    json << "{"
         << "\"width\": " << jsonNumber(m_uiManager->getRecordingWidth()) << ", "
         << "\"height\": " << jsonNumber(m_uiManager->getRecordingHeight()) << ", "
         << "\"fps\": " << jsonNumber(m_uiManager->getRecordingFps()) << ", "
         << "\"bitrate\": " << jsonNumber(m_uiManager->getRecordingBitrate()) << ", "
         << "\"audioBitrate\": " << jsonNumber(m_uiManager->getRecordingAudioBitrate()) << ", "
         << "\"codec\": " << jsonString(m_uiManager->getRecordingVideoCodec()) << ", "
         << "\"audioCodec\": " << jsonString(m_uiManager->getRecordingAudioCodec()) << ", "
         << "\"h264Preset\": " << jsonString(m_uiManager->getRecordingH264Preset()) << ", "
         << "\"h265Preset\": " << jsonString(m_uiManager->getRecordingH265Preset()) << ", "
         << "\"h265Profile\": " << jsonString(m_uiManager->getRecordingH265Profile()) << ", "
         << "\"h265Level\": " << jsonString(m_uiManager->getRecordingH265Level()) << ", "
         << "\"vp8Speed\": " << jsonNumber(m_uiManager->getRecordingVP8Speed()) << ", "
         << "\"vp9Speed\": " << jsonNumber(m_uiManager->getRecordingVP9Speed()) << ", "
         << "\"container\": " << jsonString(m_uiManager->getRecordingContainer()) << ", "
         << "\"outputPath\": " << jsonString(m_uiManager->getRecordingOutputPath()) << ", "
         << "\"filenameTemplate\": " << jsonString(m_uiManager->getRecordingFilenameTemplate()) << ", "
         << "\"includeAudio\": " << (m_uiManager->getRecordingIncludeAudio() ? "true" : "false")
         << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETRecordingStatus(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    bool isRecording = m_uiManager->getRecordingActive();
    uint64_t durationUs = m_uiManager->getRecordingDurationUs();
    uint64_t fileSize = m_uiManager->getRecordingFileSize();
    std::string filename = m_uiManager->getRecordingFilename();

    std::ostringstream json;
    json << "{"
         << "\"isRecording\": " << (isRecording ? "true" : "false") << ", "
         << "\"duration\": " << jsonNumber(durationUs) << ", "
         << "\"fileSize\": " << jsonNumber(fileSize) << ", "
         << "\"currentFile\": " << jsonString(filename);
    
    if (isRecording)
    {
        json << ", \"settings\": {"
             << "\"width\": " << jsonNumber(m_uiManager->getRecordingWidth()) << ", "
             << "\"height\": " << jsonNumber(m_uiManager->getRecordingHeight()) << ", "
             << "\"fps\": " << jsonNumber(m_uiManager->getRecordingFps()) << ", "
             << "\"codec\": " << jsonString(m_uiManager->getRecordingVideoCodec())
             << "}";
    }
    
    json << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETRecordings(int clientFd)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        auto recordings = m_application->listRecordings();
        
        std::ostringstream json;
        json << "{\"recordings\": [";
        for (size_t i = 0; i < recordings.size(); ++i)
        {
            if (i > 0)
                json << ", ";
            json << recordings[i].toJSON();
        }
        json << "], \"total\": " << recordings.size() << "}";
        
        sendJSONResponse(clientFd, 200, json.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error listing recordings: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleGETRecording(int clientFd, const std::string& recordingId)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        auto recordings = m_application->listRecordings();
        auto it = std::find_if(recordings.begin(), recordings.end(),
                              [&recordingId](const RecordingMetadata& m) { return m.id == recordingId; });
        
        if (it == recordings.end())
        {
            sendErrorResponse(clientFd, 404, "Recording not found");
            return true;
        }
        
        sendJSONResponse(clientFd, 200, it->toJSON().dump());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error getting recording: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetRecordingSettings(int clientFd, const std::string& body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);

        if (json.contains("width"))
            m_uiManager->triggerRecordingWidthChange(json["width"].get<uint32_t>());
        if (json.contains("height"))
            m_uiManager->triggerRecordingHeightChange(json["height"].get<uint32_t>());
        if (json.contains("fps"))
            m_uiManager->triggerRecordingFpsChange(json["fps"].get<uint32_t>());
        if (json.contains("bitrate"))
            m_uiManager->triggerRecordingBitrateChange(json["bitrate"].get<uint32_t>());
        if (json.contains("audioBitrate"))
            m_uiManager->triggerRecordingAudioBitrateChange(json["audioBitrate"].get<uint32_t>());
        if (json.contains("codec"))
            m_uiManager->triggerRecordingVideoCodecChange(json["codec"].get<std::string>());
        if (json.contains("audioCodec"))
            m_uiManager->triggerRecordingAudioCodecChange(json["audioCodec"].get<std::string>());
        if (json.contains("h264Preset"))
            m_uiManager->triggerRecordingH264PresetChange(json["h264Preset"].get<std::string>());
        if (json.contains("h265Preset"))
            m_uiManager->triggerRecordingH265PresetChange(json["h265Preset"].get<std::string>());
        if (json.contains("h265Profile"))
            m_uiManager->triggerRecordingH265ProfileChange(json["h265Profile"].get<std::string>());
        if (json.contains("h265Level"))
            m_uiManager->triggerRecordingH265LevelChange(json["h265Level"].get<std::string>());
        if (json.contains("vp8Speed"))
            m_uiManager->triggerRecordingVP8SpeedChange(json["vp8Speed"].get<int>());
        if (json.contains("vp9Speed"))
            m_uiManager->triggerRecordingVP9SpeedChange(json["vp9Speed"].get<int>());
        if (json.contains("container"))
            m_uiManager->triggerRecordingContainerChange(json["container"].get<std::string>());
        if (json.contains("outputPath"))
            m_uiManager->triggerRecordingOutputPathChange(json["outputPath"].get<std::string>());
        if (json.contains("filenameTemplate"))
            m_uiManager->triggerRecordingFilenameTemplateChange(json["filenameTemplate"].get<std::string>());
        if (json.contains("includeAudio"))
            m_uiManager->triggerRecordingIncludeAudioChange(json["includeAudio"].get<bool>());

        sendJSONResponse(clientFd, 200, "{\"success\": true}");
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleSetRecordingControl(int clientFd, const std::string& body)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        
        if (!json.contains("action"))
        {
            sendErrorResponse(clientFd, 400, "Missing 'action' field");
            return true;
        }

        std::string action = json["action"].get<std::string>();
        bool start = (action == "start");

        m_uiManager->triggerRecordingStartStop(start);

        std::ostringstream response;
        response << "{\"success\": true, \"action\": " << jsonString(action) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleDeleteRecording(int clientFd, const std::string& recordingId)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        if (!m_application->deleteRecording(recordingId))
        {
            sendErrorResponse(clientFd, 404, "Recording not found");
            return true;
        }

        std::ostringstream response;
        response << "{\"success\": true, \"id\": " << jsonString(recordingId) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error deleting recording: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handlePUTRecording(int clientFd, const std::string& recordingId, const std::string& body)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(body);
        if (!json.contains("name") || !json["name"].is_string())
        {
            sendErrorResponse(clientFd, 400, "Missing or invalid 'name' field");
            return true;
        }

        std::string newName = json["name"].get<std::string>();
        if (newName.empty())
        {
            sendErrorResponse(clientFd, 400, "Name cannot be empty");
            return true;
        }

        if (!m_application->renameRecording(recordingId, newName))
        {
            sendErrorResponse(clientFd, 404, "Recording not found");
            return true;
        }

        std::ostringstream response;
        response << "{\"success\": true, \"id\": " << jsonString(recordingId) 
                 << ", \"name\": " << jsonString(newName) << "}";
        sendJSONResponse(clientFd, 200, response.str());
        return true;
    }
    catch (const nlohmann::json::exception& e)
    {
        sendErrorResponse(clientFd, 400, "Invalid JSON: " + std::string(e.what()));
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error renaming recording: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleGETRecordingFile(int clientFd, const std::string& recordingId, const std::string& request)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        std::string filepath = m_application->getRecordingPath(recordingId);
        if (filepath.empty())
        {
            sendErrorResponse(clientFd, 404, "Recording not found");
            return true;
        }

        // Check if file exists
        if (!fs::exists(filepath))
        {
            sendErrorResponse(clientFd, 404, "Recording file not found");
            return true;
        }

        // Get file size
        uint64_t fileSize = fs::file_size(filepath);

        // Determine content type based on extension
        std::string contentType = "video/mp4"; // Default
        fs::path filePath(filepath);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == ".mkv")
            contentType = "video/x-matroska";
        else if (ext == ".webm")
            contentType = "video/webm";
        else if (ext == ".mp4")
            contentType = "video/mp4";

        // Check for Range request
        auto range = extractRange(request, fileSize);
        // UINT64_MAX indicates no range header
        bool isRangeRequest = (range.first != UINT64_MAX && range.second != UINT64_MAX);

        // Open file
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open())
        {
            sendErrorResponse(clientFd, 500, "Failed to open recording file");
            return true;
        }

        uint64_t startByte = 0;
        uint64_t endByte = fileSize - 1;
        uint64_t contentLength = fileSize;

        if (isRangeRequest)
        {
            startByte = range.first;
            endByte = range.second;
            contentLength = endByte - startByte + 1;
        }

        // Seek to start position
        file.seekg(startByte, std::ios::beg);

        // Prepare response headers
        std::ostringstream response;
        if (isRangeRequest)
        {
            // 206 Partial Content
            response << "HTTP/1.1 206 Partial Content\r\n";
            response << "Content-Range: bytes " << startByte << "-" << endByte << "/" << fileSize << "\r\n";
        }
        else
        {
            // 200 OK
            response << "HTTP/1.1 200 OK\r\n";
        }

        response << "Content-Type: " << contentType << "\r\n";
        response << "Content-Length: " << contentLength << "\r\n";
        response << "Accept-Ranges: bytes\r\n";
        response << "Content-Disposition: inline; filename=\"" << fs_helper::get_filename_string(filePath) << "\"\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";

        std::string headerStr = response.str();
        ssize_t sent = sendData(clientFd, headerStr.c_str(), headerStr.length());
        if (sent < 0)
        {
            file.close();
            return true;
        }

        // Stream file content in chunks (64KB at a time)
        const size_t chunkSize = 64 * 1024; // 64KB chunks
        std::vector<char> buffer(chunkSize);
        uint64_t remaining = contentLength;

        while (remaining > 0 && file.good())
        {
            size_t toRead = (remaining > chunkSize) ? chunkSize : static_cast<size_t>(remaining);
            file.read(buffer.data(), toRead);
            size_t bytesRead = file.gcount();

            if (bytesRead == 0)
                break;

            sent = sendData(clientFd, buffer.data(), bytesRead);
            if (sent < 0)
            {
                file.close();
                return true;
            }

            remaining -= bytesRead;
        }

        file.close();
        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error serving recording file: " + std::string(e.what()));
        return true;
    }
}

bool APIController::handleGETRecordingThumbnail(int clientFd, const std::string& recordingId)
{
    if (!m_application)
    {
        sendErrorResponse(clientFd, 500, "Application not available");
        return true;
    }

    try
    {
        auto recordings = m_application->listRecordings();
        auto it = std::find_if(recordings.begin(), recordings.end(),
                              [&recordingId](const RecordingMetadata& m) { return m.id == recordingId; });
        
        if (it == recordings.end())
        {
            sendErrorResponse(clientFd, 404, "Recording not found");
            return true;
        }

        // Check if thumbnail exists
        if (it->thumbnailPath.empty() || !fs::exists(it->thumbnailPath))
        {
            sendErrorResponse(clientFd, 404, "Thumbnail not found");
            return true;
        }

        // Get file size
        uint64_t fileSize = fs::file_size(it->thumbnailPath);

        // Open file
        std::ifstream file(it->thumbnailPath, std::ios::binary);
        if (!file.is_open())
        {
            sendErrorResponse(clientFd, 500, "Failed to open thumbnail file");
            return true;
        }

        // Read file content
        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), fileSize);
        file.close();

        // Prepare response headers
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: image/jpeg\r\n";
        response << "Content-Length: " << fileSize << "\r\n";
        response << "Cache-Control: public, max-age=3600\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";

        std::string headerStr = response.str();
        ssize_t sent = sendData(clientFd, headerStr.c_str(), headerStr.length());
        if (sent >= 0)
        {
            sent = sendData(clientFd, buffer.data(), fileSize);
        }

        return true;
    }
    catch (const std::exception& e)
    {
        sendErrorResponse(clientFd, 500, "Error serving thumbnail: " + std::string(e.what()));
        return true;
    }
}
