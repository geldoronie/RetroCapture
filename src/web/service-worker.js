/**
 * RetroCapture PWA Service Worker
 *
 * Strategy split is *deliberate* and was rewritten in v5 after the
 * v3/v4 "cache-first for everything" policy caused two real problems:
 *
 *   1. Stale HTML/JS hung around for days after a release, so new
 *      features (e.g. the i18n dropdown wiring on home.js) silently
 *      didn't work for returning visitors.
 *   2. /config.html is server-side gated to LAN clients only. A cached
 *      copy bypasses the gate — once a LAN user loaded it once, every
 *      subsequent visit from the same browser served the cached
 *      response without re-asking the server. That was a security hole
 *      dressed as a perf optimization. Now configurable HTML always
 *      goes through the network so the server's LAN check runs every
 *      time.
 *
 * Current rules:
 *   - /api/*                         → network-only with offline JSON.
 *   - /assets/i18n/*.json            → network-first (translations
 *                                       must never drift behind code).
 *   - /stream and /recordings/*/file → pass-through (large/streamed).
 *   - HTML (/, *.html) and JS        → network-first, cache fallback
 *                                       only when offline.
 *   - CSS / fonts / icons / images   → stale-while-revalidate: serve
 *                                       from cache instantly, refresh
 *                                       in the background.
 *
 * Cache name bumps wipe the previous generation on activate. Bump
 * whenever the precache list or any cache-first asset changes.
 */

const CACHE_NAME = 'retrocapture-v5';
const RUNTIME_CACHE = 'retrocapture-runtime-v5';

// Resources to precache on install. Keep this list small — every
// entry has to download successfully or the install fails.
const PRECACHE_URLS = [
  '/',
  '/index.html',
  '/style.css',
  '/manifest.json',
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

// Network-first: try the network, fall back to whatever's in the
// cache when offline. Successful network responses refresh the cache
// silently for next time.
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

// Stale-while-revalidate: ship the cached copy immediately and kick
// off a background fetch to refresh it for next time. Fast + always
// eventually fresh, at the cost of one stale render after each push.
// Only suitable for assets where one stale render doesn't break
// correctness (CSS, fonts, icons, images).
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

self.addEventListener('fetch', (event) => {
  const { request } = event;
  if (request.method !== 'GET') return;

  const url = new URL(request.url);

  // Pass-through: streams and large recording downloads must never
  // be intercepted by the SW (Range requests + cache don't mix).
  if (url.pathname === '/stream' || url.pathname.startsWith('/stream/')) return;
  if (url.pathname.includes('/recordings/') && url.pathname.endsWith('/file')) return;

  // APIs — network-only with a synthesized offline JSON fallback for
  // GETs. We deliberately do NOT cache /api responses; the few
  // mutations in the portal don't need offline-replay support and
  // stale GET responses caused confused-state bugs in the past.
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

  // Translation bundles — network-first so a freshly-edited bundle
  // reaches users on the next request without a cache bump.
  if (isI18nBundle(url)) {
    networkFirst(event, RUNTIME_CACHE);
    return;
  }

  // HTML + JS — network-first. HTML carries the LAN-only gate for
  // /config.html on the server side, and JS has to match the HTML
  // it shipped with. Both must always reflect the server's view.
  if (isHTML(url) || isScript(url)) {
    networkFirst(event, CACHE_NAME);
    return;
  }

  // CSS, fonts, icons, images — these change rarely and are safe to
  // serve stale for one render while refreshing in the background.
  if (isStaticAsset(url)) {
    staleWhileRevalidate(event, CACHE_NAME);
    return;
  }

  // Anything else: network-first into the runtime cache.
  networkFirst(event, RUNTIME_CACHE);
});

self.addEventListener('message', (event) => {
  if (event.data && event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }
});
