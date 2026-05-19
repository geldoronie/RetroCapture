/**
 * RetroCapture home page — landing/live experience.
 *
 * Keeps the bundle small by only doing what the home page actually
 * needs: live stream player + status polling + quick-apply presets.
 * The full configuration UI lives on config.html.
 */

// ============================================================
// Status polling — drives the four metric cards under the player
// and the "now playing" / recording badges.
// ============================================================

const POLL_INTERVAL_MS = 3000;

async function pollStatus() {
    try {
        const status = await api.getStatus();
        const isLive = !!status.streamingActive;
        document.getElementById('homeStreamStatus').textContent =
            isLive ? t('web.home.state.live') : t('web.home.state.offline');
        document.getElementById('homeStreamStatus').className = 'home-stat-value ' +
            (isLive ? 'text-danger' : 'text-secondary');
        document.getElementById('homeClientCount').textContent = status.clientCount ?? 0;
        if (status.streamUrl) {
            document.getElementById('streamLink').href = status.streamUrl;
        }

        // Show/hide the idle overlay on the player based on live state.
        const overlay = document.getElementById('livePlayerIdleOverlay');
        if (overlay) overlay.classList.toggle('d-none', isLive && !!_livePlayer);
    } catch (err) {
        console.warn('Status poll failed:', err);
    }
}

async function pollResolutionFps() {
    try {
        const [res, fps] = await Promise.all([api.getCaptureResolution(), api.getCaptureFPS()]);
        if (res && res.width && res.height) {
            document.getElementById('homeResolution').textContent = res.width + 'x' + res.height;
        }
        if (fps && fps.fps) {
            document.getElementById('homeFps').textContent = fps.fps;
        }
    } catch (err) {
        // Silent — these can fail before the source is connected.
    }
}

async function pollNowPlaying() {
    // Read-only: what's currently being captured/streamed. The home page
    // intentionally only surfaces stream-relevant info — anything that
    // *changes* configuration (preset apply, recording start, etc.) lives
    // on /config.html.
    try {
        const [src, shader] = await Promise.all([
            api.getSource().catch(() => ({})),
            api.getShader().catch(() => ({})),
        ]);
        const labels = { 0: t('web.home.source_none'), 1: 'V4L2', 2: 'DirectShow' };
        document.getElementById('homeSource').textContent =
            (labels[src.type] || t('web.home.unknown')) + (src.device ? ' — ' + src.device : '');
        document.getElementById('homeShader').textContent = shader.name || t('web.home.none');
    } catch (err) {
        console.warn('Now-playing poll failed:', err);
    }
}

// ============================================================
// Live player — same mpegts.js setup as before, just larger and
// auto-starting on page load.
// ============================================================

let _livePlayer = null;

function liveStreamUrl() {
    return '/stream?t=' + Date.now();
}

function startLivePlayer() {
    const video = document.getElementById('livePlayer');
    const status = document.getElementById('livePlayerStatus');
    const startBtn = document.getElementById('livePlayerStartBtn');
    const stopBtn = document.getElementById('livePlayerStopBtn');
    const overlay = document.getElementById('livePlayerIdleOverlay');
    if (!video) return;

    stopLivePlayer();

    if (typeof mpegts === 'undefined' || !mpegts.isSupported || !mpegts.isSupported()) {
        // Safari / native TS playback fallback.
        video.src = liveStreamUrl();
        video.play().catch(() => {});
        if (status) status.textContent = t('web.home.native_playback');
    } else {
        try {
            _livePlayer = mpegts.createPlayer({
                type: 'mpegts',
                isLive: true,
                url: liveStreamUrl(),
            }, {
                liveBufferLatencyChasing: true,
                liveBufferLatencyMaxLatency: 1.5,
                liveBufferLatencyMinRemain: 0.5,
                lazyLoad: false,
            });
            _livePlayer.attachMediaElement(video);
            _livePlayer.load();
            _livePlayer.play().catch(() => {});
            if (status) status.textContent = t('web.home.connected');
        } catch (err) {
            console.error('mpegts.js error:', err);
            if (status) status.textContent = t('web.home.player_error') + ' ' + err.message;
            return;
        }
    }
    if (startBtn) startBtn.disabled = true;
    if (stopBtn) stopBtn.disabled = false;
    if (overlay) overlay.classList.add('d-none');
}

function stopLivePlayer() {
    const video = document.getElementById('livePlayer');
    const startBtn = document.getElementById('livePlayerStartBtn');
    const stopBtn = document.getElementById('livePlayerStopBtn');
    const status = document.getElementById('livePlayerStatus');
    const overlay = document.getElementById('livePlayerIdleOverlay');

    if (_livePlayer) {
        try {
            _livePlayer.pause();
            _livePlayer.unload();
            _livePlayer.detachMediaElement();
            _livePlayer.destroy();
        } catch (err) {
            console.warn('Tear-down error:', err);
        }
        _livePlayer = null;
    }
    if (video) {
        video.pause();
        video.removeAttribute('src');
        video.load();
    }
    if (startBtn) startBtn.disabled = false;
    if (stopBtn) stopBtn.disabled = true;
    if (status) status.textContent = t('web.home.stopped');
    if (overlay) overlay.classList.remove('d-none');
}

// ============================================================
// Helpers shared with control.js (re-implemented here so the home
// page doesn't have to load the whole control.js bundle).
// ============================================================

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = String(text ?? '');
    return div.innerHTML;
}

function showAlert(message, type) {
    type = type || 'info';
    const container = document.getElementById('alertContainer');
    if (!container) return;
    const id = 'alert-' + Date.now();
    const el = document.createElement('div');
    el.id = id;
    el.className = 'alert alert-' + type + ' alert-dismissible fade show';
    el.innerHTML = escapeHtml(message) +
        '<button type="button" class="btn-close" data-bs-dismiss="alert"></button>';
    container.appendChild(el);
    setTimeout(() => { const e = document.getElementById(id); if (e) e.remove(); }, 5000);
}

function copyStreamUrl() {
    const url = document.getElementById('streamLink').href || (window.location.origin + '/stream');
    navigator.clipboard.writeText(url).then(
        () => showAlert(t('web.alert.copy_ok'), 'success'),
        () => showAlert(t('web.alert.copy_fail'), 'danger'),
    );
}

// ============================================================
// Wire up
// ============================================================

document.addEventListener('DOMContentLoaded', () => {
    // Player buttons.
    const startBtn = document.getElementById('livePlayerStartBtn');
    const stopBtn = document.getElementById('livePlayerStopBtn');
    if (startBtn) startBtn.addEventListener('click', startLivePlayer);
    if (stopBtn) stopBtn.addEventListener('click', stopLivePlayer);

    // Language dropdown — picks up current selection on load, persists
    // the user's choice in localStorage via i18n.setLanguage().
    const langSelect = document.getElementById('langSelect');
    if (langSelect && window.i18n) {
        window.i18n.onReady(() => { langSelect.value = window.i18n.getLanguage(); });
        langSelect.addEventListener('change', () => { window.i18n.setLanguage(langSelect.value); });
    }

    // First load.
    pollStatus();
    pollResolutionFps();
    pollNowPlaying();

    // Poll status / now-playing periodically.
    setInterval(pollStatus, POLL_INTERVAL_MS);
    setInterval(pollNowPlaying, POLL_INTERVAL_MS * 2);
    setInterval(pollResolutionFps, POLL_INTERVAL_MS * 4);

    // Auto-start the player if the stream is already live.
    setTimeout(async () => {
        try {
            const s = await api.getStatus();
            if (s && s.streamingActive) startLivePlayer();
        } catch (err) {
            // ignore
        }
    }, 200);
});
