#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward decl — avoid leaking the IXWebSocket include into TUs that
// only need the Snapshot view of the world.
namespace ix
{
class WebSocket;
class WebSocketMessage;
}

/**
 * Chat client transport. Talks the wire protocol documented in
 * docs/CHAT_PROTOCOL.md against the chat service at platform/services/
 * chat — a stream-linked room is resolved over REST (`GET /rooms/by-
 * stream/<streamId>`), history is seeded over REST (`GET /rooms/<id>/
 * history`), and the live session runs over WebSocket (`GET /ws?room=
 * <id>`).
 *
 * Thread model:
 *   - The owner (Application / UIManager) calls connect/disconnect/
 *     post from the main thread.
 *   - All network I/O happens off-main:
 *       * REST resolve + history is done on a one-shot worker spawned
 *         at connect time.
 *       * The WebSocket session runs on IXWebSocket's internal thread,
 *         which fires the message callback we register.
 *   - State and the message buffer are guarded by a single mutex.
 *     getSnapshot() takes a copy under the lock so the UI never holds
 *     it past the function return.
 *
 * Reconnect:
 *   - IXWebSocket auto-reconnects on transport failure; the welcome /
 *     room_state handshake fires again on each successful reconnect,
 *     and we re-send the cached nickname.
 *   - State surfaces as Reconnecting between attempts so the OSD can
 *     show a discreet indicator.
 *
 * Lifetime:
 *   - Construct once, reuse across stream sessions. Repeated
 *     connect(newStreamId) tears the WS session down and rebuilds it.
 *   - destructor calls disconnect() — safe to drop a connected
 *     instance.
 */
class ChatClient
{
public:
    enum class State
    {
        Idle,
        Resolving,    // REST GET /rooms/by-stream in flight
        Connecting,   // WS handshake in flight
        Connected,    // welcome received, session live
        Reconnecting, // WS dropped, IXWebSocket backing off
        Error,        // resolve/connect refused — see lastError
    };

    struct Message
    {
        std::string id;
        std::string participantId;
        std::string nickname;
        std::string body;
        int64_t     postedAtMs = 0;
        bool        deleted    = false;
        bool        local      = false; // true == sent by this client
        bool        host       = false; // posted by the stream host (#84)
        bool        owner      = false; // posted by the standalone-room owner (#84)
    };

    struct Participant
    {
        std::string id;
        std::string nickname;
        bool        host  = false;
        bool        owner = false;
    };

    /// Single entry in the public room listing returned by
    /// listPublicRooms. Mirrors the server's GET /rooms payload.
    struct ListedRoom
    {
        std::string roomId;
        std::string slug;
        std::string title;
        bool        hasPassword      = false;
        int         participantCount = 0;
        int64_t     createdAtMs      = 0;
    };

    struct Snapshot
    {
        State                     state         = State::Idle;
        std::string               lastError;
        std::string               baseUrl;      // e.g. "ws://localhost:8082"
        std::string               streamId;     // empty when standalone room
        std::string               slug;         // non-empty when standalone room
        std::string               roomId;
        std::string               roomTitle;    // server-supplied; empty for unnamed stream-linked rooms
        std::string               ownerClientId; // standalone-room owner rc_<id>, empty otherwise
        bool                      iAmOwner      = false;
        std::string               nickname;
        std::string               myParticipantId;
        // #84 — Current room's host participant id (empty if no host
        // has claimed). Used by OSDChat to mark backlog messages
        // whose participant_id matches even when the wire frame
        // didn't carry is_host (older history rows).
        std::string               hostParticipantId;
        // Whether this client is the room's host (i.e. connected with
        // role="host" and won the claim).
        bool                      iAmHost       = false;
        std::vector<Message>      messages;     // oldest first
        std::vector<Participant>  participants;
        // Count of messages that arrived since the last markRead() call.
        // The OSD reads this to drive its unread badge — purely a hint
        // for the UI, never used in transport.
        size_t                    unreadCount   = 0;
    };

    ChatClient();
    ~ChatClient();

    ChatClient(const ChatClient &)            = delete;
    ChatClient &operator=(const ChatClient &) = delete;

    /// Set the chat-service base URL. Accepts ws:// or wss://; HTTP
    /// endpoint is derived by swapping the scheme. May be called
    /// before connect(); a call after connect() reconnects against
    /// the new URL.
    void setBaseUrl(const std::string &baseUrl);
    std::string getBaseUrl() const;

    /// Begin a session against `streamId`. Resolves the linked room,
    /// fetches recent history, opens the WS, and sends `hello` with
    /// the nickname. Pass `asHost=true` when this RetroCapture instance
    /// is the one publishing the stream — the server will tag
    /// outgoing messages with is_host=true so viewers can render a
    /// host badge. Idempotent: connect() with the same args while
    /// already Connected is a no-op; with different args it tears
    /// down and reconnects.
    void connect(const std::string &streamId,
                 const std::string &nickname,
                 bool               asHost = false);

    /// Connect to a standalone room by its slug. Optional `password`
    /// rides the next hello for password-protected rooms; empty
    /// means "no password" and is the default. `ownerSecret` is the
    /// per-room key from owned_rooms.json — when present and
    /// matching, the server grants is_owner. v0.5 standalone rooms
    /// have no host role — asHost is implicitly false. Same
    /// idempotency rules as connect().
    void connectBySlug(const std::string &slug,
                       const std::string &nickname,
                       const std::string &password    = "",
                       const std::string &ownerSecret = "");

    /// Create a standalone room via POST /rooms. Synchronous: blocks
    /// the calling thread on the HTTP round-trip (~50 ms in dev).
    /// Returns true on success; writes the server-assigned room_id
    /// + slug into the out params. `password` is hashed server-side;
    /// empty == no password. `ownerClientId` flags subsequent joins
    /// from the same identity as is_owner. `ownerSecret` is the
    /// per-room key the client is about to persist in
    /// owned_rooms.json; the server stores its hash and flags any
    /// hello carrying the matching plaintext as is_owner.
    bool createStandaloneRoom(const std::string &title,
                              const std::string &slug,
                              const std::string &password,
                              bool               listed,
                              const std::string &ownerClientId,
                              const std::string &ownerSecret,
                              std::string       &outRoomId,
                              std::string       &outSlug,
                              std::string       &outError);

    /// Delete a standalone room via DELETE /rooms/<roomId>. Synchronous.
    /// Auth: server matches sha256(ownerSecret) against the stored
    /// hash; either the secret or the rc_<id> stored as owner must
    /// be supplied. Cascades through chat_messages — the room is
    /// gone for good. On success, returns true; outError is populated
    /// with a server-side reason on failure (e.g. "not_owner",
    /// "room_not_found").
    bool deleteStandaloneRoom(const std::string &roomId,
                              const std::string &ownerSecret,
                              const std::string &ownerClientId,
                              std::string       &outError);

    /// Fetch the public-room listing (GET /rooms). Synchronous;
    /// fills `out` with the response and returns true on success.
    bool listPublicRooms(int limit, std::vector<ListedRoom> &out,
                         std::string &outError);

    /// Tear down the WS session. Idempotent.
    void disconnect();

    /// Queue a chat post. If we're not Connected yet the post is
    /// dropped (the user just typed at a closed transport — surfacing
    /// "your message was queued for 8 minutes" is worse UX than
    /// requiring re-press).
    void post(const std::string &body);

    /// Change nickname mid-session. Re-sends `hello` with the new value.
    void setNickname(const std::string &nick);

    /// Persistent identity token (rc_<hex>). Sent as client_id on
    /// every hello so the server adopts it as the participant_id —
    /// posts survive reconnects with the same identity. Empty
    /// disables the field; server falls back to its anon-id
    /// generator. Must be set before connect() to take effect on
    /// the current session.
    void setClientId(const std::string &clientId);
    std::string getClientId() const;

    /// Thread-safe snapshot copy. UI calls this once per frame.
    Snapshot getSnapshot() const;

    /// Reset the unread counter — the panel calls this when scrolled
    /// to the bottom.
    void markRead();

    bool isActive() const;

private:
    // --- transport helpers ------------------------------------------
    void startSession();        // called from the main thread under lock
    void stopSession();         // teardown WS + cancel resolver
    void joinResolver();
    void resolveAndConnect();   // body of the resolver thread
    bool fetchHistory(const std::string &roomId);

    // IXWebSocket callbacks (run on the IX thread)
    void onMessage(const ix::WebSocketMessage &msg);
    void handleFrame(const std::string &payload);

    void appendMessage(Message m);
    void applyRoomState(const std::vector<Participant> &p,
                        const std::vector<Message>      &history);
    void setStateLocked(State s);
    void setErrorLocked(const std::string &err);

    std::string httpBase() const; // derives http:// or https:// from m_baseUrl

    // --- state -------------------------------------------------------
    mutable std::mutex          m_mu;
    State                       m_state = State::Idle;
    std::string                 m_baseUrl;
    std::string                 m_streamId;
    std::string                 m_nickname;
    std::string                 m_slug;          // non-empty == standalone
    std::string                 m_clientId;      // rc_<hex> persistent identity, empty == anon
    std::string                 m_roomId;
    std::string                 m_roomTitle;     // populated from resolve response
    std::string                 m_ownerClientId; // standalone-room owner from welcome, empty otherwise
    std::string                 m_password;      // plaintext to send in hello (password-protected rooms)
    std::string                 m_ownerSecret;   // per-room secret from owned_rooms.json, sent in hello
    bool                        m_iAmOwner       = false;
    std::string                 m_myParticipantId;
    std::string                 m_hostParticipantId;
    bool                        m_asHost          = false;
    bool                        m_iAmHost         = false;
    std::string                 m_lastError;
    std::deque<Message>         m_messages;
    std::vector<Participant>    m_participants;
    size_t                      m_unreadCount     = 0;
    bool                        m_helloSent       = false;
    bool                        m_helloAcked      = false;
    std::atomic<bool>           m_stopRequested{false};

    // Resolver thread is a one-shot per connect() call.
    std::thread                 m_resolver;
    std::atomic<bool>           m_resolverRunning{false};

    // IXWebSocket owns its own thread; we hold it via unique_ptr so
    // the destructor's stop() chain is well-ordered.
    std::unique_ptr<ix::WebSocket> m_ws;

    // Hard caps so a runaway server doesn't unbound the UI buffer.
    static constexpr size_t kMaxMessages    = 500;
    static constexpr size_t kMaxParticipants = 200;
};
