/*
 * Shared live /stream player for the web portal (home + config pages).
 *
 * One implementation (#216) instead of two diverging copies. Wraps
 * mpegts.js (Safari plays TS natively) and keeps the stream alive:
 *
 *  - Reconnects on any mpegts.js error, AND when a live load *ends*: the
 *    host closes a /stream client whose send backlog grows past its cap
 *    (a viewer whose link can't sustain the bitrate). Before, that clean
 *    close just drained the buffer and left the player paused forever.
 *  - Watchdog: if the timeline stops advancing for 8 s while the user
 *    wants playback, rebuild — including a paused / ended video that the
 *    user did not pause themselves.
 *  - Capped exponential backoff between attempts (1.5 s → 15 s), reset
 *    once playback advances again.
 *  - When the tab becomes visible again, jump back to the live edge
 *    instead of playing out whatever piled up while it was hidden.
 *
 * Usage:
 *   const player = createLivePlayer({ videoId, statusId, startBtnId,
 *                                     stopBtnId, overlayId });
 *   player.start(); player.stop();
 */
(function () {
    'use strict';

    function tr(key, fallback) {
        if (typeof window.t === 'function') {
            const s = window.t(key);
            if (s && s !== key) return s;
        }
        return fallback;
    }

    function createLivePlayer(ids) {
        const $ = (id) => (id ? document.getElementById(id) : null);
        let player = null;
        let active = false;          // the user wants playback (Start, not Stop)
        let userPaused = false;      // paused with the video's own controls
        let watchdog = null;
        let lastTime = -1;
        let stalledMs = 0;
        let recoverTimer = null;
        let backoffMs = 1500;
        let pausingInternally = false;

        function setStatus(text) {
            const el = $(ids.statusId);
            if (el) el.textContent = text;
        }

        function setButtons(running) {
            const startBtn = $(ids.startBtnId);
            const stopBtn = $(ids.stopBtnId);
            const overlay = $(ids.overlayId);
            if (startBtn) startBtn.disabled = running;
            if (stopBtn) stopBtn.disabled = !running;
            if (overlay) overlay.classList.toggle('d-none', running);
        }

        function streamUrl() {
            // Same origin as the portal; cache-buster so a reconnect never
            // gets a stale response from an intermediary.
            return '/stream?t=' + Date.now();
        }

        function teardown() {
            if (watchdog) { clearInterval(watchdog); watchdog = null; }
            if (player) {
                try {
                    pausingInternally = true;
                    player.pause();
                    player.unload();
                    player.detachMediaElement();
                    player.destroy();
                } catch (err) {
                    console.warn('live player teardown:', err);
                } finally {
                    pausingInternally = false;
                }
                player = null;
            }
            const video = $(ids.videoId);
            if (video) {
                pausingInternally = true;
                video.pause();
                pausingInternally = false;
                video.removeAttribute('src');
                video.load();
            }
        }

        function scheduleRecover(reason) {
            if (!active || recoverTimer) return;
            console.warn('live player: reconnecting (' + reason + ') in ' + backoffMs + ' ms');
            setStatus(tr('web.home.reconnecting', 'Reconnecting…'));
            const delay = backoffMs;
            backoffMs = Math.min(backoffMs * 2, 15000);
            recoverTimer = setTimeout(() => {
                recoverTimer = null;
                if (active) connect();
            }, delay);
        }

        function startWatchdog() {
            lastTime = -1;
            stalledMs = 0;
            watchdog = setInterval(() => {
                const video = $(ids.videoId);
                if (!video || !active || recoverTimer) return;
                if (userPaused) { stalledMs = 0; return; }
                // Still connecting / buffering the first frames: give it time,
                // but not forever.
                const advancing = video.currentTime !== lastTime && !video.paused;
                if (advancing) {
                    if (stalledMs > 0 || backoffMs > 1500) setStatus(tr('web.home.connected', 'Connected.'));
                    stalledMs = 0;
                    backoffMs = 1500; // healthy again → reset the backoff
                    lastTime = video.currentTime;
                    return;
                }
                stalledMs += 2000;
                if (video.ended) {
                    scheduleRecover('ended');
                } else if (stalledMs >= 8000) {
                    scheduleRecover(video.paused ? 'paused' : 'stall');
                }
            }, 2000);
        }

        function connect() {
            const video = $(ids.videoId);
            if (!video) return;
            teardown();
            userPaused = false;

            if (typeof mpegts === 'undefined' || !mpegts.isSupported || !mpegts.isSupported()) {
                // Safari / native MPEG-TS playback.
                video.src = streamUrl();
                video.play().catch(() => {});
                setStatus(tr('web.home.native_playback', 'Native playback (Safari / fallback)'));
            } else {
                try {
                    player = mpegts.createPlayer({
                        type: 'mpegts',
                        isLive: true,
                        url: streamUrl(),
                    }, {
                        liveBufferLatencyChasing: true,
                        liveBufferLatencyMaxLatency: 1.5,
                        liveBufferLatencyMinRemain: 0.5,
                        lazyLoad: false,
                    });
                    player.attachMediaElement(video);
                    player.on(mpegts.Events.ERROR, (type, detail) => {
                        console.error('mpegts.js error:', type, detail);
                        scheduleRecover('error:' + type);
                    });
                    // A live load that *completes* means the server closed
                    // the connection (stream stopped / restarted, or we fell
                    // too far behind and were dropped).
                    player.on(mpegts.Events.LOADING_COMPLETE, () => scheduleRecover('connection closed'));
                    player.load();
                    player.play().catch(() => {});
                    setStatus(tr('web.home.connected', 'Connected.'));
                } catch (err) {
                    console.error('mpegts.js error:', err);
                    setStatus(tr('web.home.player_error', 'Player error:') + ' ' + err.message);
                    scheduleRecover('create failed');
                    return;
                }
            }
            startWatchdog();
        }

        function start() {
            if (recoverTimer) { clearTimeout(recoverTimer); recoverTimer = null; }
            active = true;
            backoffMs = 1500;
            connect();
            setButtons(true);
        }

        function stop() {
            active = false;
            if (recoverTimer) { clearTimeout(recoverTimer); recoverTimer = null; }
            teardown();
            setButtons(false);
            setStatus(tr('web.home.stopped', 'Stopped.'));
        }

        // Track pauses the *user* makes with the video controls so the
        // watchdog respects them; resuming jumps back to live.
        const video = $(ids.videoId);
        if (video) {
            video.addEventListener('pause', () => {
                if (!pausingInternally && active && !video.ended && document.visibilityState === 'visible') {
                    userPaused = true;
                }
            });
            video.addEventListener('play', () => {
                if (userPaused && active) {
                    userPaused = false;
                    connect(); // resume at the live edge, not where we paused
                }
            });
        }

        // Back from a hidden tab: skip what piled up and play live.
        document.addEventListener('visibilitychange', () => {
            const v = $(ids.videoId);
            if (document.visibilityState !== 'visible' || !active || !v || userPaused) return;
            const b = v.buffered;
            if (b.length && b.end(b.length - 1) - v.currentTime > 2.0) {
                v.currentTime = b.end(b.length - 1) - 0.3;
            }
        });

        return { start, stop, isActive: () => active };
    }

    window.createLivePlayer = createLivePlayer;
})();
