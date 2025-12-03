/**
 * RetroCapture PWA Service Worker
 * Gerencia cache e funcionalidade offline
 */

const CACHE_NAME = 'retrocapture-v1';
const RUNTIME_CACHE = 'retrocapture-runtime-v1';

// Recursos para cachear na instalação
const PRECACHE_URLS = [
  '/',
  '/index.html',
  '/style.css',
  '/control.js',
  '/api.js',
  // Bootstrap e ícones são carregados via CDN, não precisam ser cacheados
];

// Instalar Service Worker e fazer precache
self.addEventListener('install', (event) => {
  console.log('[Service Worker] Installing...');
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => {
        console.log('[Service Worker] Precaching static assets');
        return cache.addAll(PRECACHE_URLS);
      })
      .then(() => self.skipWaiting()) // Ativar imediatamente
  );
});

// Ativar Service Worker e limpar caches antigos
self.addEventListener('activate', (event) => {
  console.log('[Service Worker] Activating...');
  event.waitUntil(
    caches.keys().then((cacheNames) => {
      return Promise.all(
        cacheNames.map((cacheName) => {
          if (cacheName !== CACHE_NAME && cacheName !== RUNTIME_CACHE) {
            console.log('[Service Worker] Deleting old cache:', cacheName);
            return caches.delete(cacheName);
          }
        })
      );
    }).then(() => self.clients.claim()) // Controlar todas as páginas imediatamente
  );
});

// Estratégia de cache: Network First com fallback para cache
// Para APIs, sempre tentar rede primeiro (dados em tempo real)
// Para recursos estáticos, usar cache primeiro
self.addEventListener('fetch', (event) => {
  const { request } = event;
  const url = new URL(request.url);

  // Ignorar requisições que não são GET
  if (request.method !== 'GET') {
    return;
  }

  // APIs sempre tentam rede primeiro (dados em tempo real)
  if (url.pathname.startsWith('/api/')) {
    event.respondWith(
      fetch(request)
        .then((response) => {
          // Se a rede funcionou, retornar resposta
          if (response && response.status === 200) {
            // Cachear resposta para uso offline
            const responseClone = response.clone();
            caches.open(RUNTIME_CACHE).then((cache) => {
              cache.put(request, responseClone);
            });
          }
          return response;
        })
        .catch(() => {
          // Se a rede falhou, tentar cache
          return caches.match(request).then((cachedResponse) => {
            if (cachedResponse) {
              return cachedResponse;
            }
            // Se não há cache, retornar resposta offline genérica
            return new Response(
              JSON.stringify({ error: 'Offline', message: 'Sem conexão com o servidor' }),
              {
                status: 503,
                statusText: 'Service Unavailable',
                headers: { 'Content-Type': 'application/json' }
              }
            );
          });
        })
    );
    return;
  }

  // Para recursos estáticos (HTML, CSS, JS), usar cache primeiro
  if (url.pathname === '/' || 
      url.pathname === '/index.html' ||
      url.pathname.endsWith('.css') ||
      url.pathname.endsWith('.js')) {
    event.respondWith(
      caches.match(request)
        .then((cachedResponse) => {
          if (cachedResponse) {
            return cachedResponse;
          }
          // Se não está em cache, buscar da rede
          return fetch(request)
            .then((response) => {
              // Cachear resposta para próxima vez
              if (response && response.status === 200) {
                const responseClone = response.clone();
                caches.open(CACHE_NAME).then((cache) => {
                  cache.put(request, responseClone);
                });
              }
              return response;
            });
        })
    );
    return;
  }

  // Para outros recursos, tentar rede primeiro
  event.respondWith(
    fetch(request)
      .then((response) => {
        if (response && response.status === 200) {
          const responseClone = response.clone();
          caches.open(RUNTIME_CACHE).then((cache) => {
            cache.put(request, responseClone);
          });
        }
        return response;
      })
      .catch(() => {
        return caches.match(request);
      })
  );
});

// Notificações push (opcional, para futuras implementações)
self.addEventListener('push', (event) => {
  console.log('[Service Worker] Push notification received');
  // Implementar notificações push no futuro se necessário
});

// Mensagens do cliente (para atualizar o service worker)
self.addEventListener('message', (event) => {
  if (event.data && event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }
});
