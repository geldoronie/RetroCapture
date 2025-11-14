#include "StreamManager.h"
#include "../utils/Logger.h"

StreamManager::StreamManager() {
}

StreamManager::~StreamManager() {
    cleanup();
}

void StreamManager::addStreamer(std::unique_ptr<IStreamer> streamer) {
    if (streamer) {
        m_streamers.push_back(std::move(streamer));
        LOG_INFO("Streamer adicionado: " + m_streamers.back()->getType());
    }
}

bool StreamManager::initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps) {
    if (m_initialized) {
        LOG_WARN("StreamManager já inicializado");
        return true;
    }
    
    m_width = width;
    m_height = height;
    
    bool allInitialized = true;
    for (auto& streamer : m_streamers) {
        if (!streamer->initialize(port, width, height, fps)) {
            LOG_ERROR("Falha ao inicializar streamer: " + streamer->getType());
            allInitialized = false;
        }
    }
    
    m_initialized = allInitialized;
    return allInitialized;
}

bool StreamManager::start() {
    if (!m_initialized) {
        LOG_ERROR("StreamManager não inicializado");
        return false;
    }
    
    if (m_active) {
        LOG_WARN("StreamManager já está ativo");
        return true;
    }
    
    bool allStarted = true;
    for (auto& streamer : m_streamers) {
        if (!streamer->start()) {
            LOG_ERROR("Falha ao iniciar streamer: " + streamer->getType());
            allStarted = false;
        }
    }
    
    m_active = allStarted;
    if (m_active) {
        LOG_INFO("StreamManager iniciado - " + std::to_string(m_streamers.size()) + " streamer(s) ativo(s)");
    }
    
    return allStarted;
}

void StreamManager::stop() {
    if (!m_active) {
        return;
    }
    
    for (auto& streamer : m_streamers) {
        streamer->stop();
    }
    
    m_active = false;
    LOG_INFO("StreamManager parado");
}

bool StreamManager::isActive() const {
    return m_active;
}

void StreamManager::pushFrame(const uint8_t* data, uint32_t width, uint32_t height) {
    if (!m_active || !data) {
        return;
    }
    
    for (auto& streamer : m_streamers) {
        if (streamer->isActive()) {
            streamer->pushFrame(data, width, height);
        }
    }
}

void StreamManager::pushAudio(const int16_t* samples, size_t sampleCount) {
    if (!m_active || !samples || sampleCount == 0) {
        return;
    }
    
    for (auto& streamer : m_streamers) {
        if (streamer->isActive()) {
            streamer->pushAudio(samples, sampleCount);
        }
    }
}

std::vector<std::string> StreamManager::getStreamUrls() const {
    std::vector<std::string> urls;
    for (const auto& streamer : m_streamers) {
        if (streamer->isActive()) {
            urls.push_back(streamer->getStreamUrl());
        }
    }
    return urls;
}

uint32_t StreamManager::getTotalClientCount() const {
    uint32_t total = 0;
    for (const auto& streamer : m_streamers) {
        total += streamer->getClientCount();
    }
    return total;
}

void StreamManager::cleanup() {
    stop();
    
    for (auto& streamer : m_streamers) {
        streamer->cleanup();
    }
    
    m_streamers.clear();
    m_initialized = false;
    m_active = false;
}

