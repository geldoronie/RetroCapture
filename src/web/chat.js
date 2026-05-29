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
 *   - Reads chat.url + chat.roomSlug from /meta. /meta lives on the
 *     same origin as this page (served by the host's web portal),
 *     so we don't need any cross-origin gymnastics for the API
 *     fetch itself — only the WebSocket connects out to the chat
 *     service.
 *
 * Lifecycle:
 *   - Auto-connects when /meta exposes a non-empty roomSlug.
 *   - Tears down + reconnects when either chat.url or chat.roomSlug
 *     changes between polls.
 *   - Idle (panel placeholder) when the host isn't published.
 */
(function () {
    'use strict';

    // --- DOM handles ----------------------------------------------------

    const $log         = document.getElementById('chatLog');
    const $stateBadge  = document.getElementById('chatStateBadge');
    const $count       = document.getElementById('chatParticipantCount');
    const $title       = document.getElementById('chatRoomTitle');
    const $parts       = document.getElementById('chatParticipantsPanel');
    const $unavailable = document.getElementById('chatUnavailable');
    const $profileBtn   = document.getElementById('chatProfileBtn');
    const $profileModal = document.getElementById('chatProfileModal');
    const $profileName  = document.getElementById('chatProfileName');
    const $profileNick  = document.getElementById('chatProfileNick');
    const $profileAge   = document.getElementById('chatProfileAge');
    const $profileId    = document.getElementById('chatProfileId');
    const $profileSave  = document.getElementById('chatProfileSave');
    const $profileClose = document.getElementById('chatProfileClose');
    const $profileError = document.getElementById('chatProfileError');
    const $profileSaved = document.getElementById('chatProfileSaved');
    const $profileGate  = document.getElementById('chatProfileGate');
    const $profileGateBtn = document.getElementById('chatProfileGateBtn');
    const $body         = document.querySelector('.home-chat-body');
    const $msgInput     = document.getElementById('chatMessageInput');
    const $sendBtn      = document.getElementById('chatSendBtn');
    const $nickRow      = document.querySelector('.home-chat-nick-row');
    const $composeRow   = document.querySelector('.home-chat-compose');
    const $roomBanner   = document.getElementById('chatRoomBanner');
    const $roomKind     = document.getElementById('chatRoomBannerKind');
    const $roomLabel    = document.getElementById('chatRoomBannerLabel');
    const $roomSub      = document.getElementById('chatRoomBannerSub');
    const $roomsBtn     = document.getElementById('chatRoomsBtn');
    const $roomsModal   = document.getElementById('chatRoomsModal');
    const $roomsList    = document.getElementById('chatRoomsList');
    const $roomsRefresh = document.getElementById('chatRoomsRefresh');
    const $roomsClose   = document.getElementById('chatRoomsClose');
    const $roomsOpenCreate = document.getElementById('chatRoomsOpenCreate');
    const $roomsOpenJoin   = document.getElementById('chatRoomsOpenJoin');
    const $roomsTabPublic  = document.getElementById('chatRoomsTabPublic');
    const $roomsTabOwned   = document.getElementById('chatRoomsTabOwned');
    const $roomsTabPanelPublic = document.getElementById('chatRoomsTabPanelPublic');
    const $roomsTabPanelOwned  = document.getElementById('chatRoomsTabPanelOwned');
    const $ownedList       = document.getElementById('chatOwnedList');
    const $ownedError      = document.getElementById('chatOwnedError');
    const $createModal       = document.getElementById('chatCreateModal');
    const $createClose       = document.getElementById('chatCreateClose');
    const $createError       = document.getElementById('chatCreateError');
    const $joinCustomModal   = document.getElementById('chatJoinCustomModal');
    const $joinClose         = document.getElementById('chatJoinClose');
    const $joinError         = document.getElementById('chatJoinError');
    const $disconnectBtn     = document.getElementById('chatDisconnectBtn');
    const $roomsJoinSlug = document.getElementById('chatRoomsJoinSlug');
    const $roomsJoinPass = document.getElementById('chatRoomsJoinPass');
    const $roomsJoinBtn  = document.getElementById('chatRoomsJoin');
    const $roomsCreateTitle  = document.getElementById('chatRoomsCreateTitle');
    const $roomsCreateSlug   = document.getElementById('chatRoomsCreateSlug');
    const $roomsCreatePass   = document.getElementById('chatRoomsCreatePass');
    const $roomsCreateListed = document.getElementById('chatRoomsCreateListed');
    const $roomsCreateBtn    = document.getElementById('chatRoomsCreate');
    const $roomsError        = document.getElementById('chatRoomsError');
    const $nick        = document.getElementById('chatNickInput');
    const $nickApply   = document.getElementById('chatNickApply');
    const $nickError   = document.getElementById('chatNickError');
    const $msg         = document.getElementById('chatMessageInput');
    const $send        = document.getElementById('chatSendBtn');

    if (!$log) return; // page hasn't loaded the chat card

    // --- State ----------------------------------------------------------

    const NICK_KEY     = 'rc_chat_nick';
    const IDENTITY_KEY = 'rc_chat_identity';
    // Per-room owner secrets. Mirrors the native client's
    // $XDG_DATA_HOME/retrocapture/owned_rooms.json — the JSON shape
    // is intentionally identical so a user could in principle hand-
    // copy between web and desktop, though there's no built-in
    // sync. Each entry: { room_id, slug, title, owner_secret,
    // createdAt }.
    const OWNED_KEY    = 'rc_chat_owned_rooms';

    function loadOwned() {
        try {
            const raw = localStorage.getItem(OWNED_KEY);
            if (!raw) return [];
            const j = JSON.parse(raw);
            return Array.isArray(j && j.rooms) ? j.rooms : [];
        } catch (err) {
            console.warn('owned-rooms load failed:', err);
            return [];
        }
    }
    function saveOwned(rooms) {
        try {
            localStorage.setItem(OWNED_KEY,
                JSON.stringify({ rooms: rooms || [] }));
            return true;
        } catch (err) {
            console.warn('owned-rooms save failed:', err);
            return false;
        }
    }
    function findOwnedBySlug(slug) {
        if (!slug) return null;
        const rooms = loadOwned();
        for (const r of rooms) {
            if (r.slug === slug) return r;
        }
        return null;
    }
    function appendOwned(rec) {
        const rooms = loadOwned();
        for (let i = 0; i < rooms.length; ++i) {
            if (rooms[i].slug === rec.slug) {
                rooms[i] = Object.assign({}, rooms[i], rec);
                return saveOwned(rooms);
            }
        }
        rooms.push(rec);
        return saveOwned(rooms);
    }
    function removeOwned(slug) {
        const rooms = loadOwned().filter(r => r.slug !== slug);
        return saveOwned(rooms);
    }
    // Probe whether a standalone room with `slug` exists on the
    // server. Used by the revive paths: if we own a slug locally
    // (entry in rc_chat_owned_rooms) and the server returns 404,
    // we re-POST with the saved secret to claim the slug back —
    // ownership survives the inactivity sweep. Returns one of
    // 'exists' / 'missing' / 'error'.
    async function probeRoomBySlug(slug) {
        if (!slug || !state.chatUrl) return 'error';
        try {
            const url = toHttpBase(state.chatUrl) +
                        '/rooms/by-slug/' + encodeURIComponent(slug);
            const resp = await fetch(url, { credentials: 'omit' });
            if (resp.status === 200) return 'exists';
            if (resp.status === 404) return 'missing';
            return 'error';
        } catch (_err) {
            return 'error';
        }
    }

    // POST /rooms with a known owner_secret to revive a swept
    // room. Re-uses the same shape as the create flow; the caller
    // is expected to already have the owned_rooms entry handy.
    async function reviveOwnedRoom(owned) {
        const httpBase = toHttpBase(state.chatUrl);
        const body = {
            slug:            owned.slug,
            title:           owned.title || '',
            owner_secret:    owned.owner_secret,
            owner_client_id: state.identity.id || '',
            listed:          true,
            is_stream_room:  false,
        };
        const resp = await fetch(httpBase + '/rooms', {
            method:      'POST',
            headers:     { 'Content-Type': 'application/json' },
            body:        JSON.stringify(body),
            credentials: 'omit',
        });
        const j = await resp.json().catch(() => ({}));
        if (resp.ok && j.data && j.data.slug) {
            // Refresh the local entry — fresh room_id, same
            // slug + secret.
            appendOwned({
                room_id:      j.data.room_id,
                slug:         j.data.slug,
                title:        owned.title || '',
                owner_secret: owned.owner_secret,
                createdAt:    owned.createdAt ||
                              new Date().toISOString(),
            });
            return true;
        }
        return false;
    }

    function generateOwnerSecret() {
        // 16 bytes via crypto.getRandomValues → 32 hex chars; same
        // entropy budget as the native std::random_device path.
        const buf = new Uint8Array(16);
        (crypto.getRandomValues || function () {})(buf);
        return [...buf].map(b => b.toString(16).padStart(2, '0')).join('');
    }

    // --- Identity (#84) -------------------------------------------------
    // Persisted in localStorage. The first save mints an immutable
    // rc_<12-hex> id from name + nickname + age + timestamp + random;
    // every subsequent save keeps the id and just refreshes the
    // editable fields. The server uses the id as participant_id, so
    // posts stay tied to "this browser-user" across sessions.

    function loadIdentity() {
        try {
            const raw = localStorage.getItem(IDENTITY_KEY);
            if (!raw) return { id: '', name: '', nickname: '', age: 0, createdAt: '' };
            const j = JSON.parse(raw);
            return {
                id:        j.id        || '',
                name:      j.name      || '',
                nickname:  j.nickname  || '',
                age:       parseInt(j.age, 10) || 0,
                createdAt: j.createdAt || '',
            };
        } catch (err) {
            console.warn('identity load failed:', err);
            return { id: '', name: '', nickname: '', age: 0, createdAt: '' };
        }
    }

    async function sha256Hex(input) {
        const enc = new TextEncoder();
        const buf = await crypto.subtle.digest('SHA-256', enc.encode(input));
        return [...new Uint8Array(buf)]
            .map(b => b.toString(16).padStart(2, '0'))
            .join('');
    }

    async function saveIdentity(idObj) {
        if (!idObj.id) {
            const ts   = new Date().toISOString();
            const rand = [...crypto.getRandomValues(new Uint32Array(4))]
                .map(n => n.toString(16).padStart(8, '0')).join('');
            const src  = `${idObj.name}\0${idObj.nickname}\0${idObj.age}\0${ts}\0${rand}`;
            const hash = await sha256Hex(src);
            idObj.id        = 'rc_' + hash.slice(0, 12);
            idObj.createdAt = ts;
        }
        localStorage.setItem(IDENTITY_KEY, JSON.stringify(idObj));
        return idObj;
    }

    const identityStored = loadIdentity();

    const state = {
        chatUrl:           '',
        identity:          identityStored,
        streamId:          '',
        slug:              '',  // set from window.location.hash; non-empty
                                // pins the panel to a standalone room and
                                // suppresses the /meta stream-linked auto-bind
        roomId:            '',
        roomTitle:         '',
        nickname:          identityStored.nickname || localStorage.getItem(NICK_KEY) || '',
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
        // Set by the in-header Disconnect button to suppress the
        // /meta auto-bind. Cleared the next time the user explicitly
        // joins a room (joinSlug clears it).
        userDisconnected:  false,
        // Plaintext password / per-room owner secret to ride the
        // next hello frame. joinSlug() sets these; the hello sender
        // reads them once and they decay (pendingPassword survives
        // across reconnects so we don't ask the user every time the
        // socket flaps).
        pendingPassword:    '',
        pendingOwnerSecret: '',
    };

    $nick.value = state.nickname;

    // --- Helpers --------------------------------------------------------

    // Local-dev convenience: if BOTH the chat URL and the browser are
    // at localhost-shaped hostnames, just keep the URL as configured.
    // This intentionally does NOT rewrite localhost → public hostname
    // when the page is served from a public domain — that just
    // pretends a chat service exists at host:8082 when nothing is
    // listening there. If the host wants browsers on other networks
    // to chat, they need to configure a publicly reachable chat URL
    // in Streaming → Advanced (default https://chat.retrocapture.com).
    function reachableFromBrowser(url) {
        return url || '';
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

    function deletedLabel() {
        return (typeof t === 'function')
            ? t('web.chat.message_removed')
            : '[message removed]';
    }

    function setState(newState) {
        state.connectionState = newState;
        $stateBadge.className = 'home-chat-state ' + newState;
        $stateBadge.textContent = newState;
        const sendable = newState === 'connected';
        $send.disabled = !sendable;
        $msg.disabled  = !sendable;
        if (newState === 'connected') {
            renderEmptyPlaceholder();
        }
        // Disconnect button visible only while a session is in flight
        // — there's no session to terminate when we're idle.
        if ($disconnectBtn) {
            $disconnectBtn.classList.toggle('d-none', newState === 'idle');
        }
    }

    function formatTime(epochMs) {
        if (!epochMs) return '';
        const d = new Date(epochMs);
        const hh = String(d.getHours()).padStart(2, '0');
        const mm = String(d.getMinutes()).padStart(2, '0');
        return `${hh}:${mm}`;
    }

    // Word-boundary match for `@<nickname>` in a message body. Case-
    // insensitive so "@Alice" highlights when the local user is
    // "alice" too. Only fires when the current user has a non-empty
    // nickname — no point matching @anon-xxxx.
    function mentionsMe(body) {
        if (!body || !state.nickname) return false;
        const escaped = state.nickname.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        const re = new RegExp('(^|[^A-Za-z0-9_])@' + escaped + '\\b', 'i');
        return re.test(body);
    }

    function renderMessage(m) {
        if (state.messageIds.has(m.id)) return;
        state.messageIds.add(m.id);
        removeEmptyPlaceholder();

        const isHost = m.is_host ||
            (state.hostParticipantId && m.participant_id === state.hostParticipantId);
        const isMention = !m.deleted &&
            m.participant_id !== state.myParticipantId &&
            mentionsMe(m.body);

        const row = document.createElement('div');
        row.className = 'home-chat-msg';
        row.dataset.msgId = m.id;
        if (m.deleted)  row.classList.add('deleted');
        if (isMention)  row.classList.add('mention');

        const time = formatTime(m.posted_at_ms);
        const nickClass = isHost
            ? 'home-chat-nick-label host'
            : (m.participant_id === state.myParticipantId
                ? 'home-chat-nick-label self'
                : 'home-chat-nick-label');

        const hostBadge = isHost
            ? '<span class="home-chat-host-badge">Host</span>'
            : '';

        const pid = m.participant_id || '';
        row.innerHTML =
            (time ? `<span class="home-chat-time">${escapeHtml(time)}</span>` : '') +
            hostBadge +
            `<span class="${nickClass}" title="${escapeHtml(pid)}">${escapeHtml(m.nickname)}</span>` +
            `<span class="home-chat-body">${escapeHtml(m.deleted ? deletedLabel() : m.body)}</span>`;

        $log.appendChild(row);
        if (state.autoScroll) {
            $log.scrollTop = $log.scrollHeight;
        }
    }

    function clearLog() {
        $log.innerHTML = '';
        state.messageIds.clear();
        renderEmptyPlaceholder();
    }

    // Drop-in "(no messages yet — say hi)" hint when the log is
    // visually empty + we're actually connected. Removed automatically
    // the moment a real message lands (renderMessage appends to $log
    // after the placeholder so the user gets a clean transition).
    function renderEmptyPlaceholder() {
        if (state.connectionState !== 'connected') return;
        if ($log.querySelector('.home-chat-msg')) return;
        const existing = $log.querySelector('.home-chat-empty-inline');
        if (existing) return;
        const div = document.createElement('div');
        div.className = 'home-chat-empty-inline';
        div.textContent = (typeof t === 'function')
            ? t('web.chat.empty_hint')
            : '(no messages yet — say hi)';
        $log.appendChild(div);
    }

    function removeEmptyPlaceholder() {
        const ph = $log.querySelector('.home-chat-empty-inline');
        if (ph) ph.remove();
    }

    function updateCount() {
        const n = state.participants.length;
        $count.textContent = n ? String(n) : '';
        $count.style.display = n ? '' : 'none';
        if ($parts && !$parts.classList.contains('d-none')) {
            renderParticipantsPanel();
        }
    }

    function renderParticipantsPanel() {
        if (!$parts) return;
        // Host first, rest alphabetical so the list doesn't jitter
        // as people come and go in random join order.
        const sorted = [...state.participants].sort((a, b) => {
            if (a.isHost !== b.isHost) return a.isHost ? -1 : 1;
            return (a.nickname || '').localeCompare(b.nickname || '');
        });
        $parts.innerHTML = '';
        for (const p of sorted) {
            const row = document.createElement('div');
            row.className = 'home-chat-part';
            if (p.isHost) row.classList.add('host');
            if (p.id === state.myParticipantId) row.classList.add('self');
            row.dataset.nick = p.nickname || '';
            row.title = p.id || '';
            const badge = p.isHost ? '<span class="home-chat-part-badge">Host</span>' : '';
            const meTag = (p.id === state.myParticipantId) ? ' <span class="text-muted small">(you)</span>' : '';
            row.innerHTML = badge + escapeHtml(p.nickname || '(anon)') + meTag;
            // Double-click drops "@nick " into the input — same as
            // double-clicking a name in the message list.
            row.addEventListener('dblclick', () => {
                if (p.id === state.myParticipantId) return;
                if (!p.nickname) return;
                $msg.value = ($msg.value || '') + '@' + p.nickname + ' ';
                $msg.focus();
            });
            $parts.appendChild(row);
        }
    }

    function setTitle() {
        if ($title) {
            $title.textContent = state.roomTitle
                ? `— ${state.roomTitle}`
                : (state.slug ? `— #${state.slug}` : '');
        }
        // Banner: prominent room kind + label + sub on a dedicated
        // line right under the header. Hidden when idle.
        if ($roomBanner) {
            const visible = state.connectionState !== 'idle' &&
                            (state.roomTitle || state.slug || state.streamId);
            $roomBanner.classList.toggle('d-none', !visible);
            if (visible) {
                const isStandalone = !!state.slug;
                $roomKind.textContent = isStandalone ? '[ROOM]' : '[STREAM]';
                $roomKind.className = 'home-chat-room-kind' +
                    (isStandalone ? '' : ' stream');
                $roomLabel.textContent = state.roomTitle ||
                    (state.slug ? `#${state.slug}` : 'Host stream chat');
                let sub = '';
                if (isStandalone && state.roomTitle && state.slug) {
                    sub = `#${state.slug}`;
                } else if (!isStandalone && state.hostParticipantId) {
                    const hostParticipant = state.participants.find(
                        p => p.id === state.hostParticipantId);
                    if (hostParticipant && hostParticipant.nickname) {
                        sub = `hosted by ${hostParticipant.nickname}`;
                    }
                }
                $roomSub.textContent = sub;
            }
        }
    }

    function hasProfile() {
        return !!(state.identity && state.identity.id);
    }

    function refreshGate() {
        if (!$profileGate) return;
        const needsProfile = !hasProfile();
        $profileGate.classList.toggle('d-none', !needsProfile);
        if ($body)       $body.classList.toggle('d-none',       needsProfile);
        if ($nickRow)    $nickRow.classList.toggle('d-none',    needsProfile);
        if ($composeRow) $composeRow.classList.toggle('d-none', needsProfile);
    }
    if ($profileGateBtn) {
        $profileGateBtn.addEventListener('click', () => openProfile());
    }

    // ---- Rooms modal (#84) ----------------------------------------
    async function refreshRoomsList() {
        if (!$roomsList) return;
        $roomsList.innerHTML = '<div class="home-chat-rooms-empty">Loading...</div>';
        try {
            const httpBase = toHttpBase(state.chatUrl);
            const resp = await fetch(httpBase + '/rooms?limit=50',
                { credentials: 'omit' });
            const j = await resp.json();
            const rooms = (j.data && j.data.rooms) || [];
            if (rooms.length === 0) {
                $roomsList.innerHTML = '<div class="home-chat-rooms-empty">(no public rooms)</div>';
                return;
            }
            $roomsList.innerHTML = '';
            for (const r of rooms) {
                const isStream = !!r.is_stream_room;
                const row = document.createElement('div');
                row.className = 'home-chat-rooms-item';
                const kindBadge = isStream
                    ? '<span class="kind stream">[STREAM]</span> '
                    : '<span class="kind room">[ROOM]</span> ';
                const lock = r.has_password
                    ? '<span class="lock">[lock]</span> '
                    : '';
                // Stream-linked rooms don't carry a slug — fall back
                // to the linked_stream_id so the user has something
                // to point at. v0.5 stream rooms also lack a title
                // (the chat service can't reach into the directory),
                // so the streamId is often what the user sees.
                let label;
                if (r.title) {
                    label = r.title;
                } else if (r.slug) {
                    label = '#' + r.slug;
                } else if (r.linked_stream_id) {
                    label = 'stream ' + r.linked_stream_id;
                } else {
                    label = r.room_id;
                }
                row.innerHTML = kindBadge + lock + escapeHtml(label) +
                    `<span class="count">${r.participant_count}</span>`;
                row.addEventListener('click', () => {
                    if (isStream) {
                        // Stream-linked rooms are password-less by
                        // design; bind to the streamId directly.
                        // joinStreamId mirrors joinSlug but for the
                        // stream-linked path.
                        joinStreamId(r.linked_stream_id);
                    } else if (r.has_password) {
                        $roomsJoinSlug.value = r.slug;
                        openJoinCustom();
                        if ($roomsJoinPass) $roomsJoinPass.focus();
                    } else {
                        // Auto-pass owner secret if this is one of ours.
                        const owned = findOwnedBySlug(r.slug);
                        joinSlug(r.slug, '', owned ? owned.owner_secret : '');
                    }
                });
                $roomsList.appendChild(row);
            }
        } catch (err) {
            $roomsList.innerHTML =
                `<div class="home-chat-rooms-empty">Fetch failed: ${escapeHtml(err.message)}</div>`;
        }
    }

    // Owned rooms list — populated from localStorage (mirrors the
    // native client's owned_rooms.json). Renders one row per entry
    // with a Join button and a destructive Delete (two-step confirm).
    function refreshOwnedList() {
        if (!$ownedList) return;
        $ownedError.classList.add('d-none');
        $ownedList.innerHTML = '';
        const owned = loadOwned();
        if (!owned.length) {
            $ownedList.innerHTML =
                '<div class="home-chat-rooms-empty">' +
                'You haven\'t created any rooms yet — click Create new... above.' +
                '</div>';
            return;
        }
        for (const r of owned) {
            const row = document.createElement('div');
            row.className = 'home-chat-rooms-item owned';
            const label  = r.title || `#${r.slug}`;
            row.innerHTML =
                `<span class="home-chat-rooms-item-label">${escapeHtml(label)}</span>` +
                `<span class="home-chat-rooms-item-slug">#${escapeHtml(r.slug)}</span>` +
                `<button type="button" class="btn btn-sm btn-outline-primary action-join">Join</button>` +
                `<button type="button" class="btn btn-sm btn-outline-danger action-del">Delete</button>`;
            const $join = row.querySelector('.action-join');
            const $del  = row.querySelector('.action-del');
            $join.addEventListener('click', async () => {
                // Probe + revive before joining. Server may have
                // reaped the room via the inactivity sweep while
                // we kept the local entry. Recreate transparently
                // with the saved secret so ownership survives.
                if (r.owner_secret) {
                    const probe = await probeRoomBySlug(r.slug);
                    if (probe === 'missing') {
                        await reviveOwnedRoom(r);
                    }
                }
                joinSlug(r.slug, '', r.owner_secret || '');
            });
            let armed = false;
            $del.addEventListener('click', async () => {
                if (!armed) {
                    armed = true;
                    $del.textContent = 'Confirm delete (forever)';
                    $del.classList.remove('btn-outline-danger');
                    $del.classList.add('btn-danger');
                    setTimeout(() => {
                        // Disarm after 4 s if user did nothing.
                        if (!armed) return;
                        armed = false;
                        $del.textContent = 'Delete';
                        $del.classList.remove('btn-danger');
                        $del.classList.add('btn-outline-danger');
                    }, 4000);
                    return;
                }
                // Fire the destructive DELETE. Auth is the
                // per-room owner_secret only — the server rejects
                // anything without the matching plaintext, on
                // purpose (client_id is sender-claimed and not
                // safe for an authoritative op like this).
                if (!r.owner_secret) {
                    $ownedError.textContent =
                        'Missing local owner secret; cannot delete remotely.';
                    $ownedError.classList.remove('d-none');
                    return;
                }
                try {
                    const httpBase = toHttpBase(state.chatUrl);
                    const resp = await fetch(httpBase + '/rooms/' + r.room_id, {
                        method: 'DELETE',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({
                            owner_secret: r.owner_secret,
                        }),
                        credentials: 'omit',
                    });
                    if (!resp.ok && resp.status !== 404) {
                        let msg = `HTTP ${resp.status}`;
                        try {
                            const j = await resp.json();
                            if (j && j.error && j.error.message) msg = j.error.message;
                        } catch (_) {}
                        $ownedError.textContent = 'Delete failed: ' + msg;
                        $ownedError.classList.remove('d-none');
                        return;
                    }
                    // 200 or 404 → entry is dead either way; drop the
                    // local record and bail out of any active session
                    // pointing at it.
                    removeOwned(r.slug);
                    if (state.slug === r.slug) {
                        userDisconnect();
                    }
                    refreshOwnedList();
                    refreshRoomsList();
                } catch (err) {
                    $ownedError.textContent = 'Delete failed: ' + err.message;
                    $ownedError.classList.remove('d-none');
                }
            });
            $ownedList.appendChild(row);
        }
    }

    function selectRoomsTab(which) {
        const pubActive = (which === 'public');
        if ($roomsTabPublic) $roomsTabPublic.classList.toggle('is-active', pubActive);
        if ($roomsTabOwned)  $roomsTabOwned.classList.toggle('is-active', !pubActive);
        if ($roomsTabPanelPublic) $roomsTabPanelPublic.classList.toggle('d-none', !pubActive);
        if ($roomsTabPanelOwned)  $roomsTabPanelOwned.classList.toggle('d-none',  pubActive);
        if (pubActive) refreshRoomsList(); else refreshOwnedList();
    }

    function openRooms() {
        if (!$roomsModal) return;
        $roomsError.classList.add('d-none');
        $roomsModal.classList.remove('d-none');
        selectRoomsTab('public');
    }
    function closeRooms() {
        if (!$roomsModal) return;
        $roomsModal.classList.add('d-none');
    }
    function openCreate() {
        if (!$createModal) return;
        $createError.classList.add('d-none');
        $createModal.classList.remove('d-none');
    }
    function closeCreate() {
        if (!$createModal) return;
        $createModal.classList.add('d-none');
    }
    function openJoinCustom() {
        if (!$joinCustomModal) return;
        $joinError.classList.add('d-none');
        $joinCustomModal.classList.remove('d-none');
    }
    function closeJoinCustom() {
        if (!$joinCustomModal) return;
        $joinCustomModal.classList.add('d-none');
    }
    function joinSlug(slug, password, ownerSecret) {
        if (!slug) return;
        state.slug     = slug;
        state.streamId = '';
        state.pendingPassword    = password    || '';
        state.pendingOwnerSecret = ownerSecret || '';
        // Joining a room clears the user-disconnected flag so the
        // /meta poll can resume normal stream-linked auto-binding
        // when this room is left.
        state.userDisconnected = false;
        window.location.hash = `#r/${slug}`;
        closeRooms();
        closeCreate();
        closeJoinCustom();
        openSession();
    }
    // Join a stream-linked chat directly by stream id (clicked from
    // the [STREAM]-badged entries in the Public listing). No slug,
    // no password — stream rooms are public-by-design.
    function joinStreamId(streamId) {
        if (!streamId) return;
        state.streamId = streamId;
        state.slug     = '';
        state.pendingPassword    = '';
        state.pendingOwnerSecret = '';
        state.userDisconnected   = false;
        // Drop any hash that pinned us to a standalone slug — the
        // /meta poll's stream-linked path will keep us bound now.
        if (window.location.hash) {
            try { history.replaceState(null, '', window.location.pathname); }
            catch (_) { window.location.hash = ''; }
        }
        closeRooms();
        openSession();
    }
    // User-initiated disconnect. Closes the live WS, drops the slug
    // pin, and arms a flag so the /meta auto-bind doesn't immediately
    // reconnect us to the host stream until the user explicitly opts
    // back in (Join from Rooms, URL hash, etc).
    function userDisconnect() {
        state.userDisconnected = true;
        state.slug             = '';
        state.streamId         = '';
        state.pendingPassword    = '';
        state.pendingOwnerSecret = '';
        if (window.location.hash) {
            try { history.replaceState(null, '', window.location.pathname); }
            catch (_) { window.location.hash = ''; }
        }
        teardown();
        setState('idle');
    }
    if ($roomsBtn)     $roomsBtn.addEventListener('click', openRooms);
    if ($roomsClose)   $roomsClose.addEventListener('click', closeRooms);
    if ($roomsRefresh) {
        $roomsRefresh.addEventListener('click', () => {
            refreshRoomsList();
            refreshOwnedList();
        });
    }
    if ($roomsTabPublic) $roomsTabPublic.addEventListener('click', () => selectRoomsTab('public'));
    if ($roomsTabOwned)  $roomsTabOwned.addEventListener('click',  () => selectRoomsTab('owned'));
    if ($roomsOpenCreate) $roomsOpenCreate.addEventListener('click', openCreate);
    if ($roomsOpenJoin)   $roomsOpenJoin.addEventListener('click',   openJoinCustom);
    if ($createClose) $createClose.addEventListener('click', closeCreate);
    if ($joinClose)   $joinClose.addEventListener('click',   closeJoinCustom);
    if ($disconnectBtn) $disconnectBtn.addEventListener('click', userDisconnect);
    if ($roomsJoinBtn) {
        $roomsJoinBtn.addEventListener('click', async () => {
            const slug = ($roomsJoinSlug.value || '').trim().toLowerCase();
            const pw   = $roomsJoinPass.value || '';
            if (!slug) {
                $joinError.textContent = 'Slug is required.';
                $joinError.classList.remove('d-none');
                return;
            }
            const owned  = findOwnedBySlug(slug);
            const secret = owned ? owned.owner_secret : '';
            // Revive: if we own the slug and the server returned
            // a 404 (sweep ate it), recreate transparently with
            // the saved secret so ownership survives.
            if (owned && secret) {
                const probe = await probeRoomBySlug(slug);
                if (probe === 'missing') {
                    await reviveOwnedRoom(owned);
                }
            }
            joinSlug(slug, pw, secret);
        });
    }
    if ($roomsCreateBtn) {
        $roomsCreateBtn.addEventListener('click', async () => {
            $createError.classList.add('d-none');
            const slug = $roomsCreateSlug.value.trim();
            // Revive-aware create: if the typed slug matches one
            // we already own, reuse the saved secret so the POST
            // either revives (slug free server-side) or hits a
            // slug-collision that we treat as "still ours".
            const existingOwned = slug ? findOwnedBySlug(slug) : null;
            const ownerSecret   = existingOwned
                ? existingOwned.owner_secret
                : generateOwnerSecret();
            const body = {};
            if ($roomsCreateTitle.value.trim()) body.title = $roomsCreateTitle.value.trim();
            if (slug)                           body.slug  = slug;
            if ($roomsCreatePass.value)         body.password = $roomsCreatePass.value;
            body.listed         = $roomsCreateListed.checked;
            body.is_stream_room = false;
            body.owner_secret   = ownerSecret;
            if (state.identity && state.identity.id) {
                body.owner_client_id = state.identity.id;
            }
            try {
                const httpBase = toHttpBase(state.chatUrl);
                const resp = await fetch(httpBase + '/rooms', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(body),
                    credentials: 'omit',
                });
                const j = await resp.json();
                let landedSlug, landedRoomId;
                if (resp.ok && j.data && j.data.slug) {
                    landedSlug   = j.data.slug;
                    landedRoomId = j.data.room_id;
                } else if (existingOwned) {
                    // Collision on a slug we own — the room
                    // still exists; the owner_secret in the
                    // hello will get us is_owner anyway. Skip
                    // the create error and fall through.
                    landedSlug   = existingOwned.slug;
                    landedRoomId = existingOwned.room_id;
                } else {
                    const code = (j.error && j.error.code) || resp.status;
                    const msg  = (j.error && j.error.message) || 'create failed';
                    $createError.textContent = `${code}: ${msg}`;
                    $createError.classList.remove('d-none');
                    return;
                }
                appendOwned({
                    room_id:      landedRoomId,
                    slug:         landedSlug,
                    title:        body.title ||
                                  (existingOwned ? existingOwned.title : ''),
                    owner_secret: ownerSecret,
                    createdAt:    (existingOwned && existingOwned.createdAt) ||
                                  new Date().toISOString(),
                });
                $roomsCreateTitle.value = '';
                $roomsCreateSlug.value  = '';
                $roomsCreatePass.value  = '';
                joinSlug(landedSlug, body.password || '', ownerSecret);
            } catch (err) {
                $createError.textContent = 'Create failed: ' + err.message;
                $createError.classList.remove('d-none');
            }
        });
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

    async function resolveRoom() {
        // Slug pins win — standalone rooms are user-chosen, stream-
        // linked is the fallback when nothing was pinned via URL.
        const base = toHttpBase(state.chatUrl);
        const url = state.slug
            ? base + '/rooms/by-slug/'   + encodeURIComponent(state.slug)
            : base + '/rooms/by-stream/' + encodeURIComponent(state.streamId);
        const resp = await fetch(url, { credentials: 'omit' });
        if (!resp.ok) throw new Error('resolve HTTP ' + resp.status);
        const j = await resp.json();
        if (!j || !j.data || !j.data.room_id) throw new Error('resolve missing room_id');
        // Both by-stream and by-slug return the room title for v0.5+;
        // empty when the room was never given one (legacy stream-linked).
        state.roomTitle = (j.data.title || '').trim();
        setTitle();
        return j.data.room_id;
    }

    // teardown clears the active session: drops the reconnect timer,
    // detaches all WS handlers BEFORE closing so the old socket's
    // async close event can't fire scheduleReconnect, then drops the
    // socket. Used by every code path that re-opens a session (poll
    // detected new URL, user changed nickname) — those paths follow
    // up with a fresh openSession that wires its own handlers on a
    // new socket.
    // Profile modal wiring (#84). Open populates fields from
    // state.identity; Save persists + reconnects so the next hello
    // carries the (possibly new) client_id and nickname.
    function openProfile() {
        if (!$profileModal) return;
        $profileName.value = state.identity.name || '';
        $profileNick.value = state.identity.nickname || '';
        $profileAge.value  = state.identity.age || '';
        $profileId.textContent = state.identity.id || 'not generated';
        $profileError.classList.add('d-none');
        $profileSaved.classList.add('d-none');
        $profileModal.classList.remove('d-none');
    }
    function closeProfile() {
        if (!$profileModal) return;
        $profileModal.classList.add('d-none');
    }
    if ($profileBtn)   $profileBtn.addEventListener('click', openProfile);
    if ($profileClose) $profileClose.addEventListener('click', closeProfile);
    if ($profileSave) {
        $profileSave.addEventListener('click', async () => {
            const nick = ($profileNick.value || '').trim();
            $profileError.classList.add('d-none');
            $profileSaved.classList.add('d-none');
            if (!nick) {
                $profileError.textContent = 'Nickname is required.';
                $profileError.classList.remove('d-none');
                return;
            }
            const next = {
                id:        state.identity.id || '',
                name:      ($profileName.value || '').trim(),
                nickname:  nick,
                age:       parseInt($profileAge.value, 10) || 0,
                createdAt: state.identity.createdAt || '',
            };
            try {
                const saved = await saveIdentity(next);
                state.identity = saved;
                state.nickname = saved.nickname;
                localStorage.setItem(NICK_KEY, saved.nickname);
                $profileId.textContent = saved.id;
                $profileSaved.classList.remove('d-none');
                // Re-evaluate the gate (now passing) and reconnect so
                // the server picks up the new client_id / nickname
                // on the next hello.
                refreshGate();
                openSession();
            } catch (err) {
                $profileError.textContent = 'Save failed: ' + err.message;
                $profileError.classList.remove('d-none');
            }
        });
    }

    // Toggle button for the participants panel.
    if ($count) {
        $count.addEventListener('click', () => {
            if (!$parts) return;
            const willOpen = $parts.classList.contains('d-none');
            $parts.classList.toggle('d-none', !willOpen);
            $count.classList.toggle('open', willOpen);
            if (willOpen) renderParticipantsPanel();
        });
    }

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
        state.roomTitle         = '';
        setTitle();
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
        // Profile gate (#84): without a saved identity we don't open
        // a WS at all — the server requires non-empty nickname and
        // we'd just bounce immediately. The CTA in the gate panel
        // points the user at Profile.
        if (!hasProfile()) {
            teardown();
            setState('idle');
            refreshGate();
            return;
        }
        const hasTarget = !!state.chatUrl && (!!state.streamId || !!state.slug);
        if (!hasTarget) {
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
            roomId = await resolveRoom();
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
                const helloPayload = { nickname: state.nickname };
                if (state.identity && state.identity.id) {
                    helloPayload.client_id = state.identity.id;
                }
                if (state.pendingPassword) {
                    helloPayload.password = state.pendingPassword;
                }
                if (state.pendingOwnerSecret) {
                    helloPayload.owner_secret = state.pendingOwnerSecret;
                }
                ws.send(JSON.stringify({
                    kind:  'hello',
                    hello: helloPayload,
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
                    row.classList.add('deleted');
                    const body = row.querySelector('.home-chat-body');
                    if (body) body.textContent = deletedLabel();
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
            $nickError.textContent = (typeof t === 'function')
                ? t('web.chat.nickname_empty')
                : "Nickname can't be empty";
            $nickError.classList.remove('d-none');
            return;
        }
        const collision = state.participants.some(p =>
            p.id !== state.myParticipantId && p.nickname === newNick);
        if (collision) {
            $nickError.textContent = (typeof t === 'function')
                ? t('web.chat.nickname_taken')
                : 'Nickname already in use';
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
            // Restore focus on the input — Enter keeps it natively,
            // but clicking the Send button moves focus to the button.
            // Either way, refocus so the user can keep typing without
            // a stray click on the textbox between every message.
            $msg.focus();
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

    // Parse the URL hash for a standalone-room pin: #r/<slug>. When
    // present the panel ignores /meta's stream-linked binding and
    // talks to the slug instead. Lets viewers share a link like
    // https://stream.example.com/#r/smash-sun without the host having
    // to do anything chat-side.
    function readHashSlug() {
        const h = (window.location.hash || '').replace(/^#/, '');
        const m = h.match(/^r\/([a-z0-9][a-z0-9-]{1,40})$/i);
        return m ? m[1].toLowerCase() : '';
    }

    // /meta is on the same origin as the page; api.js is set up for the
    // /api/v1/* endpoints, so we just fetch /meta directly.
    async function pollMeta() {
        try {
            const resp = await fetch('/meta', { credentials: 'omit' });
            if (!resp.ok) return;
            const meta = await resp.json();
            const chat = meta.chat || {};
            const url      = chat.url      || '';
            // #84 — The host's persistent chat room is now exposed
            // as `roomSlug`; the per-session `streamId` form is
            // gone. Older hosts that haven't shipped the update
            // simply won't expose anything here and the viewer
            // stays idle until they pick a room by hand.
            const hostSlug = chat.roomSlug || '';
            const slug     = readHashSlug() || hostSlug;

            // No host-side slug AND no hash → nothing to bind to.
            // Tear down any session we had so the panel goes idle
            // instead of pretending to be connected.
            if (!slug) {
                if (state.slug || state.streamId) {
                    teardown();
                    state.slug     = '';
                    state.streamId = '';
                    setState('idle');
                }
                return;
            }
            if (!url) return;

            // User pressed Disconnect — suppress auto-bind to the
            // host stream until the user explicitly opts back in.
            // Hash-pinned slugs bypass this gate because they're an
            // explicit opt-in by the user.
            if (state.userDisconnected && !readHashSlug()) return;

            if (url !== state.chatUrl || slug !== state.slug) {
                state.chatUrl  = url;
                state.streamId = '';
                state.slug     = slug;
                openSession();
            }
        } catch (err) {
            // Silent — we'll retry on the next poll.
        }
    }

    setState('idle');
    refreshGate();
    pollMeta();
    setInterval(pollMeta, 5000);

    // Hash change → re-resolve on next poll. Don't trigger openSession
    // synchronously here so we don't race pollMeta's in-flight fetch.
    window.addEventListener('hashchange', () => {
        // Force re-evaluation: clearing the cached slug makes the next
        // pollMeta tick treat the URL as new.
        const slug = readHashSlug();
        if (slug !== state.slug) pollMeta();
    });
})();
