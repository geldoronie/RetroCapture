/**
 * Service Worker registration with auto-update.
 *
 * Shared between index.html, recordings.html and config.html so the
 * lifecycle hooks stay identical across the portal.
 *
 * The flow is the standard "always serve fresh" PWA pattern:
 *   1. register() on page load (browser checks /service-worker.js
 *      bytes; if changed, installs the new SW alongside the old).
 *   2. On 'updatefound', wait for the new SW to reach 'installed'
 *      state, then tell it to skipWaiting() so it activates without
 *      the user having to close every tab.
 *   3. The 'controllerchange' event fires when the new SW takes
 *      control — at that point we reload the page so the user
 *      actually sees the new HTML/JS instead of the in-memory
 *      copies bound to the old SW.
 *
 * Without (2) and (3), pushing an update to /service-worker.js
 * leaves users stuck on the previous generation until they manually
 * close every tab — which is what caused the "language switch does
 * nothing" reports right after the v3→v5 SW rewrite.
 */
(function () {
    if (!('serviceWorker' in navigator)) return;

    // Reload-once guard so the controllerchange-driven reload doesn't
    // loop on the freshly-loaded page that already has the new SW.
    let _reloading = false;
    navigator.serviceWorker.addEventListener('controllerchange', () => {
        if (_reloading) return;
        _reloading = true;
        window.location.reload();
    });

    window.addEventListener('load', () => {
        navigator.serviceWorker.register('/service-worker.js')
            .then((registration) => {
                // If a new SW is already waiting at registration time
                // (e.g. user kept the tab open while we pushed a new
                // release), nudge it to activate now.
                if (registration.waiting) {
                    registration.waiting.postMessage({ type: 'SKIP_WAITING' });
                }

                // Catch SWs that move into 'installing' AFTER this load.
                registration.addEventListener('updatefound', () => {
                    const sw = registration.installing;
                    if (!sw) return;
                    sw.addEventListener('statechange', () => {
                        if (sw.state === 'installed' && navigator.serviceWorker.controller) {
                            // New SW installed while an old one was
                            // still in control — activate the new one.
                            sw.postMessage({ type: 'SKIP_WAITING' });
                        }
                    });
                });
            })
            .catch((err) => {
                console.warn('Service Worker registration failed:', err);
            });
    });
})();
