#include "TranslationManager.h"

#include "FilesystemCompat.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace
{
    using json = nlohmann::json;
}

TranslationManager &TranslationManager::instance()
{
    static TranslationManager s;
    return s;
}

void TranslationManager::init(const std::string &assetsDir, const std::string &language)
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_assetsDir = assetsDir;
    m_language  = language.empty() ? "en" : language;

    m_fallbackEn.clear();
    m_overlay.clear();

    const fs::path base = fs::path(assetsDir) / "i18n";
    loadBundle((base / "en.json").string(), m_fallbackEn);
    if (m_language != "en")
    {
        loadBundle((base / (m_language + ".json")).string(), m_overlay);
    }
    LOG_INFO("TranslationManager initialized (lang=" + m_language +
             ", en_keys=" + std::to_string(m_fallbackEn.size()) +
             ", overlay_keys=" + std::to_string(m_overlay.size()) + ")");
}

void TranslationManager::setLanguage(const std::string &language)
{
    std::lock_guard<std::mutex> lock(m_mu);
    const std::string lang = language.empty() ? "en" : language;
    if (lang == m_language) return;
    m_language = lang;
    m_overlay.clear();
    if (m_language != "en")
    {
        const fs::path base = fs::path(m_assetsDir) / "i18n";
        loadBundle((base / (m_language + ".json")).string(), m_overlay);
    }
    LOG_INFO("TranslationManager language switched to " + m_language +
             " (overlay_keys=" + std::to_string(m_overlay.size()) + ")");
}

std::string TranslationManager::getLanguage() const
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_language.empty() ? "en" : m_language;
}

std::string TranslationManager::get(const std::string &key) const
{
    std::lock_guard<std::mutex> lock(m_mu);
    // Three-step lookup: current overlay first, fall back to
    // English, then to the key itself. The last fallback makes a
    // missing translation obvious in the UI ("menu.file" instead
    // of an empty label) and keeps every screen rendering even
    // when a resource file is mid-edit.
    if (!m_overlay.empty())
    {
        auto it = m_overlay.find(key);
        if (it != m_overlay.end()) return it->second;
    }
    auto it = m_fallbackEn.find(key);
    if (it != m_fallbackEn.end()) return it->second;
    return key;
}

void TranslationManager::loadBundle(const std::string &path,
                                    std::unordered_map<std::string, std::string> &into)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        LOG_WARN("TranslationManager: bundle not found at " + path);
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    try
    {
        auto j = json::parse(buf.str());
        if (!j.is_object())
        {
            LOG_WARN("TranslationManager: bundle root is not an object: " + path);
            return;
        }
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (it.value().is_string())
            {
                into[it.key()] = it.value().get<std::string>();
            }
        }
    }
    catch (const std::exception &ex)
    {
        LOG_WARN(std::string("TranslationManager: parse failed for ") + path + ": " + ex.what());
    }
}
