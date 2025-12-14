#include "APIController.h"
#include "../core/Application.h"
#include "../ui/UIManager.h"
#include "../shader/ShaderEngine.h"
#include "HTTPServer.h"
#include "../utils/Logger.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <cstring>

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
        return handleGET(clientFd, path);
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
    else if (method == "OPTIONS")
    {
        // CORS preflight
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Access-Control-Allow-Origin: *\r\n";
        response << "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n";
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

bool APIController::handleGET(int clientFd, const std::string &path)
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
    else if (path == "/api/v1/mf/devices")
    {
        return handleGETMFDevices(clientFd);
    }
    else if (path == "/api/v1/mf/devices/refresh")
    {
        return handleRefreshMFDevices(clientFd);
    }
    else if (path == "/api/v1/status")
    {
        return handleGETStatus(clientFd);
    }
    else if (path == "/api/v1/platform")
    {
        return handleGETPlatform(clientFd);
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
    else if (path == "/api/v1/v4l2/control")
    {
        return handleSetV4L2Control(clientFd, body);
    }
    else if (path == "/api/v1/v4l2/device")
    {
        return handleSetV4L2Device(clientFd, body);
    }
    else if (path == "/api/v1/mf/device")
    {
        return handleSetMFDevice(clientFd, body);
    }

    send404(clientFd);
    return true;
}

bool APIController::handlePUT(int clientFd, const std::string &path, const std::string &body)
{
    // PUT usa os mesmos handlers que POST
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
         << "\"monitorIndex\": " << jsonNumber(m_uiManager->getMonitorIndex())
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
         << "{\"value\": 2, \"name\": \"Media Foundation\"}";
#else
    json << "{\"value\": 0, \"name\": \"None\"}, "
         << "{\"value\": 1, \"name\": \"V4L2\"}";
#endif

    json << "]"
         << "}";
    sendJSONResponse(clientFd, 200, json.str());
    return true;
}

bool APIController::handleGETMFDevices(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    const auto &devices = m_uiManager->getMFDevices();
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

bool APIController::handleRefreshMFDevices(int clientFd)
{
    if (!m_uiManager)
    {
        sendErrorResponse(clientFd, 500, "UIManager not available");
        return true;
    }

    m_uiManager->refreshMFDevices();
    return handleGETMFDevices(clientFd);
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

bool APIController::handleSetMFDevice(int clientFd, const std::string &body)
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
