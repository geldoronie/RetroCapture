#pragma once

#include <string>
#include <unordered_map>
#include <mutex>

/**
 * Minimal i18n / l10n facility for RetroCapture (#45).
 *
 * JSON-keyed translation bundles live under
 *   <assets>/i18n/<lang>.json
 *
 * with a flat `{ "key.path": "Translated string", ... }` shape.
 * English (`en.json`) is the canonical source and always loaded.
 * Switching to any other language overlays its bundle on top —
 * keys missing from the chosen language fall back to English,
 * keys missing from English fall back to the key string itself
 * so a brand-new T("foo.bar") never renders an empty label even
 * when the resource file hasn't been edited yet.
 *
 * Thread safety: all calls are guarded by an internal mutex. The
 * UI thread is the typical caller; lookup is a single map find
 * so the lock is cheap.
 *
 * Usage pattern from anywhere:
 *
 *   #include "../utils/TranslationManager.h"
 *   ImGui::Text("%s", T("menu.file").c_str());
 *
 * The free `T()` overload returns by value so the caller doesn't
 * accidentally keep a reference into the map across a language
 * switch.
 */
class TranslationManager
{
public:
    static TranslationManager &instance();

    /**
     * Wire up the bundles. `assetsDir` should be the
     * read-only-assets root (Paths::getReadOnlyAssetsDir()); we
     * append `i18n/` and load `en.json` (always) and
     * `<language>.json` (if different from "en"). Calling init
     * again with a different language reloads the overlay bundle
     * but keeps the cached English fallback in memory.
     */
    void init(const std::string &assetsDir, const std::string &language);

    /// Hot-swap the overlay language without restarting. Empty
    /// argument falls back to English-only (no overlay).
    void setLanguage(const std::string &language);

    /// The current overlay language code (or "en" when none).
    std::string getLanguage() const;

    /// Translation lookup. Returns a copy so the caller can hold
    /// it across a setLanguage() call without dangling references.
    std::string get(const std::string &key) const;

private:
    TranslationManager()  = default;
    ~TranslationManager() = default;
    TranslationManager(const TranslationManager &)            = delete;
    TranslationManager &operator=(const TranslationManager &) = delete;

    void loadBundle(const std::string &path,
                    std::unordered_map<std::string, std::string> &into);

    mutable std::mutex                            m_mu;
    std::string                                   m_assetsDir;
    std::string                                   m_language;     // "en", "pt", ...
    std::unordered_map<std::string, std::string>  m_fallbackEn;
    std::unordered_map<std::string, std::string>  m_overlay;      // current language; empty when language == "en"
};

/// Convenience wrapper: `T("key")` reads more naturally inline
/// than `TranslationManager::instance().get("key")` and the
/// short name keeps the touched call sites legible.
inline std::string T(const std::string &key)
{
    return TranslationManager::instance().get(key);
}
