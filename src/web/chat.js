/**
 * RetroCapture web chat client (#84).
 *
 * Talks to the chat service documented in docs/CHAT_PROTOCOL.md
 * using the browser's native WebSocket. Reuses the same wire format
 * as the C++ OSD overlay so backend has nothing to special-case
 * between native and web clients.
 *
 * Identity:
 *   - Persistent nickname in localStorage under `rc_chat_nick`.
 *     Editable inline next to the message list, mirroring the
 *     native OSD's Apply flow.
 *
 * Discovery:
 *   - Reads chat.url + chat.streamId from /meta. /meta lives on the
 *     same origin as this page (served by the host's web portal),
 *     so we don't need any cross-origin gymnastics for the API
 *     fetch itself — only the WebSocket connects out to the chat
 *     service.
 *
 * Lifecycle:
 *   - Auto-connects when /meta exposes a non-empty streamId.
 *   - Tears down + reconnects when either chat.url or chat.streamId
 *     changes between polls.
 *   - Idle (panel placeholder) when the host isn't published.
 */
(function () {
    'use strict';

    // --- DOM handles ----------------------------------------------------

    const $log         = document.getElementById('chatLog');
    const $stateBadge  = document.getElementById('chatStateBadge');
    const $count       = document.getElementById('chatParticipantCount');
    const $unavailable = document.getElementById('chatUnavailable');
    const $nick        = document.getElementById('chatNickInput');
    const $nickApply   = document.getElementById('chatNickApply');
    const $nickError   = document.getElementById('chatNickError');
    const $msg         = document.getElementById('chatMessageInput');
    const $send        = document.getElementById('chatSendBtn');

    if (!$log) return; // page hasn't loaded the chat card

    // --- State ----------------------------------------------------------

    const NICK_KEY = 'rc_chat_nick';

    const state = {
        chatUrl:           '',
        streamId:          '',
        roomId:            '',
        nickname:          localStorage.getItem(NICK_KEY) || '',
        ws:                null,
        myParticipantId:   '',
        hostParticipantId: '',
        helloAcked:        false,
        messageIds:        new Set(),
        participants:      [], // [{id, nickname, isHost}]
        autoScroll:        true,
        reconnectTimer:    null,
        reconnectDelayMs:  2000, // exponential backoff, reset on welcome
        connectionState:   'idle',
        // Set true while openSession is running. Guards against
        // pollMeta firing a second openSession before the first one
        // has finished its WS handshake (which would tear down the
        // half-open WS and start over).
        opening:           false,
    };

    $nick.value = state.nickname;

    // --- Helpers --------------------------------------------------------

    // If the host configured a localhost-shaped chat URL (typical dev
    // setup against docker compose), the browser can only reach it
    // when it happens to be on the same machine. Rewrite the host part
    // to whatever hostname the browser used to reach this page,
    // preserving scheme + port — so e.g. http://localhost:8082 becomes
    // http://<lan-ip>:8082 when the page was loaded over a LAN address.
    // No-op for non-loopback hosts.
    function reachableFromBrowser(url) {
        if (!url) return '';
        try {
            const u = new URL(url);
            if (u.hostname === 'localhost' || u.hostname === '127.0.0.1') {
                u.hostname = window.location.hostname;
            }
            return u.toString().replace(/\/$/, '');
        } catch (err) {
            return url;
        }
    }

    // Same scheme normalisation the C++ ChatClient does: accept any of
    // ws/wss/http/https and emit ws/wss for the WebSocket dial.
    function toWsBase(url) {
        const normalized = reachableFromBrowser(url);
        if (!normalized) return '';
        if (normalized.startsWith('https://')) return 'wss://' + normalized.slice(8);
        if (normalized.startsWith('http://'))  return 'ws://'  + normalized.slice(7);
        return normalized; // already ws:// or wss://
    }

    function toHttpBase(url) {
        const normalized = reachableFromBrowser(url);
        if (!normalized) return '';
        if (normalized.startsWith('wss://')) return 'https://' + normalized.slice(6);
        if (normalized.startsWith('ws://'))  return 'http://'  + normalized.slice(5);
        return normalized;
    }

    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = String(text ?? '');
        return div.innerHTML;
    }

    function setState(newState) {
        state.connectionState = newState;
        $stateBadge.className = 'home-chat-state ' + newState;
        $stateBadge.textContent = newState;
        const sendable = newState === 'connected';
        $send.disabled = !sendable;
        $msg.disabled  = !sendable;
    }

    function formatTime(epochMs) {
        if (!epochMs) return '';
        const d = new Date(epochMs);
        const hh = String(d.getHours()).padStart(2, '0');
        const mm = String(d.getMinutes()).padStart(2, '0');
        return `${hh}:${mm}`;
    }

    function renderMessage(m) {
        if (state.messageIds.has(m.id)) return;
        state.messageIds.add(m.id);

        const isHost = m.is_host ||
            (state.hostParticipantId && m.participant_id === state.hostParticipantId);

        const row = document.createElement('div');
        row.className = 'home-chat-msg';
        row.dataset.msgId = m.id;
        if (m.deleted) row.classList.add('deleted');

        const time = formatTime(m.posted_at_ms);
        const nickClass = isHost
            ? 'home-chat-nick-label host'
            : (m.participant_id === state.myParticipantId
                ? 'home-chat-nick-label self'
                : 'home-chat-nick-label');

        const hostBadge = isHost
            ? '<span class="home-chat-host-badge">Host</span>'
            : '';

        row.innerHTML =
            (time ? `<span class="home-chat-time">${escapeHtml(time)}</span>` : '') +
            hostBadge +
            `<span class="${nickClass}">${escapeHtml(m.nickname)}</span>` +
            `<span class="home-chat-body">${escapeHtml(m.deleted ? '[message removed]' : m.body)}</span>`;

        $log.appendChild(row);
        if (state.autoScroll) {
            $log.scrollTop = $log.scrollHeight;
        }
    }

    function clearLog() {
        $log.innerHTML = '';
        state.messageIds.clear();
    }

    function updateCount() {
        $count.textContent = state.participants.length
            ? `• ${state.participants.length}`
            : '';
    }

    // --- Transport ------------------------------------------------------

    async function loadHistory(roomId) {
        try {
            const url = toHttpBase(state.chatUrl) + '/rooms/' + roomId + '/history?limit=50';
            const resp = await fetch(url, { credentials: 'omit' });
            if (!resp.ok) return;
            const j = await resp.json();
            const messages = (j && j.data && j.data.messages) || [];
            // /history returns newest-first; we want oldest at top.
            messages.sort((a, b) => a.posted_at_ms - b.posted_at_ms);
            messages.forEach(renderMessage);
        } catch (err) {
            console.warn('chat history fetch failed:', err);
        }
    }

    async function resolveRoom(streamId) {
        const url = toHttpBase(state.chatUrl) + '/rooms/by-stream/' + encodeURIComponent(streamId);
        const resp = await fetch(url, { credentials: 'omit' });
        if (!resp.ok) throw new Error('resolve HTTP ' + resp.status);
        const j = await resp.json();
        if (!j || !j.data || !j.data.room_id) throw new Error('resolve missing room_id');
        return j.data.room_id;
    }

    // teardown clears the active session: drops the reconnect timer,
    // detaches all WS handlers BEFORE closing so the old socket's
    // async close event can't fire scheduleReconnect, then drops the
    // socket. Used by every code path that re-opens a session (poll
    // detected new URL, user changed nickname) — those paths follow
    // up with a fresh openSession that wires its own handlers on a
    // new socket.
    function teardown() {
        if (state.reconnectTimer) {
            clearTimeout(state.reconnectTimer);
            state.reconnectTimer = null;
        }
        if (state.ws) {
            try {
                state.ws.onopen    = null;
                state.ws.onmessage = null;
                state.ws.onclose   = null;
                state.ws.onerror   = null;
                state.ws.close();
            } catch (e) { /* ignore */ }
            state.ws = null;
        }
        state.myParticipantId   = '';
        state.hostParticipantId = '';
        state.helloAcked        = false;
        state.participants      = [];
        updateCount();
    }

    async function openSession() {
        if (state.opening) return; // already in flight
        state.opening = true;
        try {
            await openSessionInner();
        } finally {
            state.opening = false;
        }
    }

    async function openSessionInner() {
        if (!state.chatUrl || !state.streamId) {
            teardown();
            setState('idle');
            $unavailable.classList.remove('d-none');
            return;
        }
        $unavailable.classList.add('d-none');
        teardown();
        clearLog();

        setState('resolving');
        let roomId;
        try {
            roomId = await resolveRoom(state.streamId);
        } catch (err) {
            console.warn('chat resolve failed:', err);
            setState('error');
            scheduleReconnect();
            return;
        }
        state.roomId = roomId;

        // Seed history first so the panel paints something useful
        // before the WS handshake completes.
        await loadHistory(roomId);

        setState('connecting');
        const wsUrl = toWsBase(state.chatUrl) + '/ws?room=' + encodeURIComponent(roomId);
        const ws = new WebSocket(wsUrl);
        state.ws = ws;

        ws.onopen = () => {
            try {
                ws.send(JSON.stringify({
                    kind:  'hello',
                    hello: { nickname: state.nickname },
                }));
            } catch (err) {
                console.warn('hello send failed:', err);
            }
        };

        ws.onmessage = (evt) => {
            let frame;
            try { frame = JSON.parse(evt.data); }
            catch (err) { return; }
            handleFrame(frame);
        };

        ws.onclose = () => {
            // teardown() nulls all handlers before closing, so we only
            // ever see this when the server really dropped us.
            state.helloAcked = false;
            if (state.connectionState !== 'idle') {
                setState('reconnecting');
                scheduleReconnect();
            }
        };

        ws.onerror = () => {
            // onclose follows; reconnect is handled there.
        };
    }

    function scheduleReconnect() {
        if (state.reconnectTimer) return;
        const delay = state.reconnectDelayMs;
        state.reconnectTimer = setTimeout(() => {
            state.reconnectTimer = null;
            // Exponential backoff with a 30 s cap so a genuinely
            // unreachable chat service doesn't pin the tab pegging
            // the network.
            state.reconnectDelayMs = Math.min(state.reconnectDelayMs * 2, 30000);
            openSession();
        }, delay);
    }

    function handleFrame(frame) {
        if (!frame || !frame.kind) return;
        switch (frame.kind) {
            case 'welcome': {
                const w = frame.welcome || {};
                state.myParticipantId   = w.participant_id || '';
                state.hostParticipantId = w.host_participant_id || '';
                state.helloAcked        = true;
                // A successful handshake resets the reconnect backoff
                // so the next transient blip starts at 2 s again
                // instead of inheriting whatever capped delay we
                // were sitting on from a previous outage.
                state.reconnectDelayMs  = 2000;
                setState('connected');
                break;
            }
            case 'room_state': {
                const rs = frame.room_state || {};
                state.participants = (rs.participants || []).map(p => ({
                    id: p.id || p.participant_id,
                    nickname: p.nickname,
                    isHost: !!p.is_host,
                }));
                if (rs.host_participant_id) {
                    state.hostParticipantId = rs.host_participant_id;
                }
                updateCount();
                break;
            }
            case 'message': {
                renderMessage(frame.message || {});
                break;
            }
            case 'presence': {
                const p = frame.presence || {};
                if (p.event === 'join') {
                    const existing = state.participants.find(x => x.id === p.participant_id);
                    if (!existing) {
                        state.participants.push({
                            id: p.participant_id,
                            nickname: p.nickname,
                            isHost: !!p.is_host,
                        });
                    }
                    if (p.is_host) state.hostParticipantId = p.participant_id;
                } else if (p.event === 'leave') {
                    state.participants = state.participants.filter(x => x.id !== p.participant_id);
                    if (state.hostParticipantId === p.participant_id) {
                        state.hostParticipantId = '';
                    }
                }
                updateCount();
                break;
            }
            case 'deleted': {
                const d = frame.deleted || {};
                const row = $log.querySelector(`[data-msg-id="${d.message_id}"]`);
                if (row) {
                    row.classList.add('text-muted', 'fst-italic');
                    const body = row.querySelector('.chat-body');
                    if (body) body.textContent = '[message removed]';
                }
                break;
            }
            case 'error': {
                console.warn('chat server error:', frame.error);
                break;
            }
            default:
                break;
        }
    }

    // --- UI wiring ------------------------------------------------------

    $log.addEventListener('scroll', () => {
        // Sticky-bottom detection same as the native OSD.
        const atBottom = ($log.scrollHeight - $log.scrollTop - $log.clientHeight) < 8;
        state.autoScroll = atBottom;
    });

    function applyNickname() {
        const newNick = $nick.value.trim();
        $nickError.classList.add('d-none');
        if (!newNick) {
            $nickError.textContent = 'Nickname can\'t be empty';
            $nickError.classList.remove('d-none');
            return;
        }
        const collision = state.participants.some(p =>
            p.id !== state.myParticipantId && p.nickname === newNick);
        if (collision) {
            $nickError.textContent = 'Nickname already in use';
            $nickError.classList.remove('d-none');
            return;
        }
        if (newNick === state.nickname) return;
        state.nickname = newNick;
        localStorage.setItem(NICK_KEY, newNick);
        // Wire protocol has no rename frame — reconnect to re-handshake.
        openSession();
    }

    $nickApply.addEventListener('click', applyNickname);
    $nick.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            e.preventDefault();
            applyNickname();
        }
    });
    $nick.addEventListener('input', () => $nickError.classList.add('d-none'));

    function sendMessage() {
        if (state.connectionState !== 'connected') return;
        const body = $msg.value.trim();
        if (!body) return;
        try {
            state.ws.send(JSON.stringify({
                kind: 'post',
                post: { body: body },
            }));
            $msg.value = '';
        } catch (err) {
            console.warn('post send failed:', err);
        }
    }

    $send.addEventListener('click', sendMessage);
    $msg.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            e.preventDefault();
            sendMessage();
        }
    });

    // --- Discovery loop -------------------------------------------------

    // /meta is on the same origin as the page; api.js is set up for the
    // /api/v1/* endpoints, so we just fetch /meta directly.
    async function pollMeta() {
        try {
            const resp = await fetch('/meta', { credentials: 'omit' });
            if (!resp.ok) return;
            const meta = await resp.json();
            const chat = meta.chat || {};
            const url  = chat.url || '';
            const sid  = chat.streamId || '';
            // Defensive: a poll that returns empty values almost always
            // means the host's DirectoryClient is briefly Error-state
            // between heartbeats (publish about to retry). Tearing the
            // chat down on every blip would make the panel flash
            // "idle ↔ connected" and re-resolve the room every few
            // seconds. Ignore empties; only act on a stable, non-empty
            // configuration. The cost is that "host genuinely stopped
            // publishing" doesn't snap the panel to idle immediately —
            // the user can refresh the page to clean up.
            if (!url || !sid) return;
            if (url !== state.chatUrl || sid !== state.streamId) {
                state.chatUrl  = url;
                state.streamId = sid;
                openSession();
            }
        } catch (err) {
            // Silent — we'll retry on the next poll.
        }
    }

    setState('idle');
    pollMeta();
    setInterval(pollMeta, 5000);
})();
