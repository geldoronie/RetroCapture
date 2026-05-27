/**
 * RetroCapture PWA Service Worker.
 *
 * Strategy split by sensitivity — the v3/v4 "cache-first for
 * everything" policy caused real bugs (stale /config.html bypassing
 * the server's LAN gate, language switches that didn't take because
 * cached home.js trailed the server), so v5 onward draws a hard line:
 *
 *   - HTML / JS / i18n bundles  → network-first, fall back to cache
 *                                  only when offline. Anything that
 *                                  carries auth, code, or strings
 *                                  *must* match the server's view.
 *   - CSS / fonts / icons / images / manifest
 *                               → stale-while-revalidate. One stale
 *                                 render is acceptable; the
 *                                 background refresh fixes it on the
 *                                 next visit.
 *   - /api/*                    → network-only, with a synthetic
 *                                  offline JSON for graceful degrade.
 *   - /stream and /recordings/.../file
 *                               → pass-through (Range + cache don't
 *                                 mix).
 *
 * Lifecycle: install precaches the minimal viable shell, then
 * skipWaiting + clients.claim let a freshly-pushed SW take over the
 * page on the very next navigation. Index.html ships a tiny
 * controllerchange listener that reloads the page when a new SW
 * activates mid-session — that, plus a cache-name bump per release,
 * is what keeps users out of stale-cache hell.
 *
 * Bump CACHE_NAME and RUNTIME_CACHE in lockstep on any portal
 * release that touches static assets — that's what trips the
 * activate-time cleanup of older caches.
 */

// Bumped v6 → v7 for the #84 web chat overhaul (chat.js rewrite, new
// YouTube-style layout, removed localhost-hostname rewrite). Without
// this bump, browsers carry the old chat.js until the user manually
// clears storage or closes every open tab.
const CACHE_NAME = 'retrocapture-v7';
const RUNTIME_CACHE = 'retrocapture-runtime-v7';

// Resources to precache so the PWA shell works offline. Keep small
// — every entry has to fetch successfully or install fails.
const PRECACHE_URLS = [
  '/',
  '/index.html',
  '/style.css',
  '/manifest.json',
  '/api.js',
  '/home.js',
  '/chat.js',
  '/i18n.js',
  '/vendor/bootstrap.min.css',
  '/vendor/bootstrap.bundle.min.js',
  '/vendor/bootstrap-icons.css',
  '/vendor/mpegts.min.js',
  '/vendor/fonts/bootstrap-icons.woff',
  '/vendor/fonts/bootstrap-icons.woff2',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(PRECACHE_URLS))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then((names) => Promise.all(
        names.map((name) => {
          if (name !== CACHE_NAME && name !== RUNTIME_CACHE) {
            return caches.delete(name);
          }
        })
      ))
      .then(() => self.clients.claim())
  );
});

// ─── strategy helpers ────────────────────────────────────────────

function networkFirst(event, cacheName) {
  event.respondWith(
    fetch(event.request)
      .then((response) => {
        if (response && response.status === 200) {
          const clone = response.clone();
          caches.open(cacheName).then((c) => c.put(event.request, clone));
        }
        return response;
      })
      .catch(() => caches.match(event.request))
  );
}

function staleWhileRevalidate(event, cacheName) {
  event.respondWith(
    caches.match(event.request).then((cached) => {
      const networkFetch = fetch(event.request)
        .then((response) => {
          if (response && response.status === 200) {
            const clone = response.clone();
            caches.open(cacheName).then((c) => c.put(event.request, clone));
          }
          return response;
        })
        .catch(() => cached);
      return cached || networkFetch;
    })
  );
}

// ─── classifiers ─────────────────────────────────────────────────

function isHTML(url) {
  return url.pathname === '/' || url.pathname.endsWith('.html');
}

function isScript(url) {
  return url.pathname.endsWith('.js');
}

function isI18nBundle(url) {
  return url.pathname.startsWith('/assets/i18n/');
}

function isStaticAsset(url) {
  return (
    url.pathname.endsWith('.css') ||
    url.pathname.endsWith('.woff') ||
    url.pathname.endsWith('.woff2') ||
    url.pathname.endsWith('.png') ||
    url.pathname.endsWith('.jpg') ||
    url.pathname.endsWith('.jpeg') ||
    url.pathname.endsWith('.svg') ||
    url.pathname.endsWith('.ico') ||
    url.pathname === '/manifest.json'
  );
}

// ─── router ──────────────────────────────────────────────────────

self.addEventListener('fetch', (event) => {
  const { request } = event;
  if (request.method !== 'GET') return;

  const url = new URL(request.url);

  if (url.pathname === '/stream' || url.pathname.startsWith('/stream/')) return;
  if (url.pathname.includes('/recordings/') && url.pathname.endsWith('/file')) return;

  if (url.pathname.startsWith('/api/')) {
    event.respondWith(
      fetch(request).catch(() =>
        new Response(
          JSON.stringify({ error: 'Offline', message: 'No connection to the server' }),
          {
            status: 503,
            statusText: 'Service Unavailable',
            headers: { 'Content-Type': 'application/json' },
          }
        )
      )
    );
    return;
  }

  if (isI18nBundle(url) || isHTML(url) || isScript(url)) {
    networkFirst(event, isI18nBundle(url) ? RUNTIME_CACHE : CACHE_NAME);
    return;
  }

  if (isStaticAsset(url)) {
    staleWhileRevalidate(event, CACHE_NAME);
    return;
  }

  networkFirst(event, RUNTIME_CACHE);
});

self.addEventListener('message', (event) => {
  if (event.data && event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }
});
