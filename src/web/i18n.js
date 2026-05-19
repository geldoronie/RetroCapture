/**
 * RetroCapture portal i18n.
 *
 * Lookup precedence on page load:
 *   1. localStorage 'retroCapturePortalLang' — explicit user override
 *      from the header dropdown.
 *   2. GET /api/v1/preferences → {"language":"..."} — the host
 *      application's current UI language (default).
 *   3. navigator.language prefix (e.g. "pt" from "pt-BR").
 *   4. "en" fallback.
 *
 * Bundle source: GET /assets/i18n/<lang>.json. The English bundle is
 * always loaded as the fallback layer so a missing key in the overlay
 * still renders translated text.
 *
 * DOM application:
 *   - data-i18n="key"          → element.textContent = t(key)
 *   - data-i18n-html="key"     → element.innerHTML  = t(key)
 *     (use only on elements whose source content you control)
 *   - data-i18n-attr="attr:key,attr2:key2"
 *                              → element.setAttribute(attr, t(key))
 *   - data-i18n-title          → shorthand for title:key on the same node
 *   - data-i18n-placeholder    → shorthand for placeholder:key
 *
 * Runtime callers (control.js, home.js) read translations via
 * window.t(key, params) which does $1/$2 style positional substitution
 * when `params` is provided.
 */

(function (global) {
    'use strict';

    const STORAGE_KEY = 'retroCapturePortalLang';
    const FALLBACK_LANG = 'en';
    const SUPPORTED = ['en', 'pt'];

    let _overlay = {};
    let _fallback = {};
    let _currentLang = FALLBACK_LANG;
    const _readyCallbacks = [];
    let _ready = false;

    // Hide the body until translations are applied so users don't see
    // a flash of the raw English literals before the overlay lands.
    // The matching CSS rule lives in style.css. We also arm a safety
    // timeout — if init() ever crashes or hangs, we unhide anyway so
    // the page never stays invisible.
    try { document.documentElement.classList.add('i18n-loading'); } catch (e) { /* ignore */ }
    setTimeout(function () {
        try { document.documentElement.classList.remove('i18n-loading'); } catch (e) { /* ignore */ }
    }, 3000);

    function detectInitialLang(hostLang) {
        try {
            const override = localStorage.getItem(STORAGE_KEY);
            if (override && SUPPORTED.indexOf(override) >= 0) return override;
        } catch (e) { /* private mode etc. */ }

        if (hostLang && SUPPORTED.indexOf(hostLang) >= 0) return hostLang;

        const nav = (navigator.language || 'en').toLowerCase().split('-')[0];
        if (SUPPORTED.indexOf(nav) >= 0) return nav;

        return FALLBACK_LANG;
    }

    async function fetchBundle(lang) {
        try {
            const resp = await fetch('/assets/i18n/' + lang + '.json', { cache: 'no-cache' });
            if (!resp.ok) return {};
            return await resp.json();
        } catch (e) {
            console.warn('i18n: failed to load', lang, e);
            return {};
        }
    }

    async function fetchHostLanguage() {
        try {
            const resp = await fetch('/api/v1/preferences', { cache: 'no-cache' });
            if (!resp.ok) return null;
            const data = await resp.json();
            return data && typeof data.language === 'string' ? data.language : null;
        } catch (e) {
            return null;
        }
    }

    function t(key, params) {
        let value = _overlay[key];
        if (value === undefined) value = _fallback[key];
        if (value === undefined) value = key; // last-resort: surface the key
        if (params) {
            for (const p in params) {
                if (Object.prototype.hasOwnProperty.call(params, p)) {
                    value = value.split('{' + p + '}').join(String(params[p]));
                }
            }
        }
        return value;
    }

    function applyTranslations(root) {
        root = root || document;

        // data-i18n: textContent
        root.querySelectorAll('[data-i18n]').forEach(function (el) {
            const key = el.getAttribute('data-i18n');
            if (key) el.textContent = t(key);
        });

        // data-i18n-html: innerHTML (trusted keys only)
        root.querySelectorAll('[data-i18n-html]').forEach(function (el) {
            const key = el.getAttribute('data-i18n-html');
            if (key) el.innerHTML = t(key);
        });

        // data-i18n-attr="attr:key,attr2:key2"
        root.querySelectorAll('[data-i18n-attr]').forEach(function (el) {
            const spec = el.getAttribute('data-i18n-attr');
            if (!spec) return;
            spec.split(',').forEach(function (pair) {
                const idx = pair.indexOf(':');
                if (idx < 0) return;
                const attr = pair.substring(0, idx).trim();
                const key = pair.substring(idx + 1).trim();
                if (attr && key) el.setAttribute(attr, t(key));
            });
        });

        // Convenience shorthands.
        root.querySelectorAll('[data-i18n-title]').forEach(function (el) {
            const key = el.getAttribute('data-i18n-title');
            if (key) el.setAttribute('title', t(key));
        });
        root.querySelectorAll('[data-i18n-placeholder]').forEach(function (el) {
            const key = el.getAttribute('data-i18n-placeholder');
            if (key) el.setAttribute('placeholder', t(key));
        });

        document.documentElement.setAttribute('lang', _currentLang === 'pt' ? 'pt-BR' : 'en');
    }

    async function setLanguage(lang) {
        if (SUPPORTED.indexOf(lang) < 0) lang = FALLBACK_LANG;
        _currentLang = lang;
        try { localStorage.setItem(STORAGE_KEY, lang); } catch (e) { /* ignore */ }
        if (lang === FALLBACK_LANG) {
            _overlay = {};
        } else {
            _overlay = await fetchBundle(lang);
        }
        applyTranslations(document);
    }

    function getLanguage() { return _currentLang; }

    function onReady(cb) {
        if (_ready) cb();
        else _readyCallbacks.push(cb);
    }

    async function init() {
        try {
            // If the user already picked a language in this browser, skip
            // the /api/v1/preferences round-trip and go straight to that
            // bundle. Cuts one fetch from the critical-path on every page
            // load and removes a flicker source when the API call is slow.
            let stored = null;
            try { stored = localStorage.getItem(STORAGE_KEY); } catch (e) { /* ignore */ }

            // Fallback (English) is always loaded so missing-key paths
            // never return raw keys to the UI.
            _fallback = await fetchBundle(FALLBACK_LANG);

            let lang;
            if (stored && SUPPORTED.indexOf(stored) >= 0) {
                lang = stored;
            } else {
                const hostLang = await fetchHostLanguage();
                lang = detectInitialLang(hostLang);
            }
            _currentLang = lang;
            if (lang !== FALLBACK_LANG) {
                _overlay = await fetchBundle(lang);
            }
            applyTranslations(document);
        } catch (err) {
            console.warn('i18n init failed', err);
        } finally {
            // Unhide the body whether init succeeded or failed — better
            // to show untranslated content than nothing at all.
            try { document.documentElement.classList.remove('i18n-loading'); } catch (e) { /* ignore */ }
            _ready = true;
            _readyCallbacks.splice(0).forEach(function (cb) {
                try { cb(); } catch (e) { console.warn('i18n onReady cb threw', e); }
            });
        }
    }

    global.t = t;
    global.i18n = {
        init: init,
        setLanguage: setLanguage,
        getLanguage: getLanguage,
        apply: applyTranslations,
        onReady: onReady,
        supported: SUPPORTED.slice(),
    };

    // Auto-init on script load (independent of DOMContentLoaded so the
    // translations land before app code runs in most cases).
    init();
})(window);
