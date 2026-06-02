#include "ScreenBackend.h"

// Real Linux screen-capture backend: xdg-desktop-portal ScreenCast
// (session set-up over D-Bus via libdbus) + a PipeWire stream for the
// frames. Works on Wayland and X11. Compiled only when both deps are
// present (see CMake / RETROCAPTURE_SCREEN_PIPEWIRE); otherwise the
// test-pattern stub in VideoCaptureScreen_stub.cpp stands in.
#ifdef RETROCAPTURE_SCREEN_PIPEWIRE

#include "../utils/Logger.h"

#include <dbus/dbus.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
constexpr const char *kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath    = "/org/freedesktop/portal/desktop";
constexpr const char *kScreenCastIf  = "org.freedesktop.portal.ScreenCast";
constexpr const char *kRequestIf     = "org.freedesktop.portal.Request";

// ── small libdbus helpers ────────────────────────────────────────────

void appendVariantString(DBusMessageIter *dict, const char *key, const char *val)
{
    DBusMessageIter entry, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

void appendVariantUint(DBusMessageIter *dict, const char *key, dbus_uint32_t val)
{
    DBusMessageIter entry, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &val);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

void appendVariantBool(DBusMessageIter *dict, const char *key, dbus_bool_t val)
{
    DBusMessageIter entry, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

// Map a PipeWire SPA raw video format to our sink's pixel format.
bool spaFormatToSink(uint32_t spaFmt, ScreenPixelFormat &out)
{
    switch (spaFmt)
    {
        case SPA_VIDEO_FORMAT_BGRA: out = ScreenPixelFormat::BGRA; return true;
        case SPA_VIDEO_FORMAT_BGRx: out = ScreenPixelFormat::BGRX; return true;
        case SPA_VIDEO_FORMAT_RGBA: out = ScreenPixelFormat::RGBA; return true;
        case SPA_VIDEO_FORMAT_RGBx: out = ScreenPixelFormat::RGBX; return true;
        default: return false;
    }
}

class PipeWireScreenBackend : public ScreenBackend
{
public:
    explicit PipeWireScreenBackend(IScreenFrameSink &sink) : m_sink(sink) {}
    ~PipeWireScreenBackend() override { stop(); }

    bool start(const std::string &target, bool captureCursor) override
    {
        if (m_running.exchange(true)) return true;
        m_thread = std::thread(&PipeWireScreenBackend::run, this, target, captureCursor);
        return true;
    }

    void stop() override
    {
        if (!m_running.exchange(false)) return;
        // Wake the D-Bus handshake loop / let the pipewire loop exit.
        if (m_thread.joinable()) m_thread.join();
    }

    std::vector<DeviceInfo> listTargets() override
    {
        // The portal's own dialog performs the real monitor/window pick;
        // we expose a single synthetic entry to drive the UI.
        DeviceInfo d;
        d.id        = "portal";
        d.name      = "Desktop (choose in system dialog)";
        d.driver    = "pipewire-portal";
        d.available = true;
        return {d};
    }

private:
    // ── PipeWire stream callbacks ────────────────────────────────────
    static void onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *param)
    {
        auto *self = static_cast<PipeWireScreenBackend *>(data);
        if (!param || id != SPA_PARAM_Format) return;

        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
        if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;

        spa_zero(self->m_format);
        if (spa_format_video_raw_parse(param, &self->m_format) < 0) return;

        LOG_INFO("VideoCaptureScreen(pw): negotiated " +
                 std::to_string(self->m_format.size.width) + "x" +
                 std::to_string(self->m_format.size.height));
    }

    static void onStreamProcess(void *data)
    {
        auto *self = static_cast<PipeWireScreenBackend *>(data);
        struct pw_buffer *b = pw_stream_dequeue_buffer(self->m_stream);
        if (!b) return;

        struct spa_buffer *buf = b->buffer;
        if (buf && buf->n_datas > 0 && buf->datas[0].data)
        {
            const struct spa_data &d = buf->datas[0];
            const uint32_t w = self->m_format.size.width;
            const uint32_t h = self->m_format.size.height;
            uint32_t stride = d.chunk ? static_cast<uint32_t>(d.chunk->stride) : 0;
            if (stride == 0 && w) stride = w * 4;

            ScreenPixelFormat fmt;
            if (w && h && stride && spaFormatToSink(self->m_format.format, fmt))
            {
                self->m_sink.onScreenFrame(static_cast<const uint8_t *>(d.data),
                                           w, h, stride, fmt);
            }
        }
        pw_stream_queue_buffer(self->m_stream, b);
    }

    // ── worker thread: portal handshake then pipewire loop ───────────
    void run(std::string /*target*/, bool captureCursor)
    {
        DBusError err;
        dbus_error_init(&err);
        // Private connection: we pump it from this worker thread, and a
        // shared one would race the tray's session-bus connection.
        m_dbus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        if (!m_dbus)
        {
            LOG_ERROR(std::string("VideoCaptureScreen(portal): no session bus — ") +
                      (dbus_error_is_set(&err) ? err.message : "?"));
            dbus_error_free(&err);
            m_running.store(false);
            return;
        }
        dbus_connection_set_exit_on_disconnect(m_dbus, FALSE);
        dbus_bus_add_match(m_dbus,
            "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
            &err);
        dbus_connection_flush(m_dbus);

        const std::string sender = uniqueSenderToken();
        std::string sessionHandle;
        uint32_t nodeId = 0;

        if (!createSession(sender, sessionHandle) ||
            !selectSources(sessionHandle, sender, captureCursor) ||
            !startPortal(sessionHandle, sender, nodeId))
        {
            LOG_WARN("VideoCaptureScreen(portal): handshake did not complete "
                     "(cancelled or unsupported)");
            cleanupDbus();
            m_running.store(false);
            return;
        }

        const int pwFd = openPipeWireRemote(sessionHandle);
        if (pwFd < 0)
        {
            LOG_ERROR("VideoCaptureScreen(portal): OpenPipeWireRemote failed");
            cleanupDbus();
            m_running.store(false);
            return;
        }

        runPipeWire(pwFd, nodeId);
        cleanupDbus();
        m_running.store(false);
    }

    // Predictable-ish unique token for handle_token / matching.
    std::string uniqueSenderToken()
    {
        return "rc" + std::to_string(static_cast<long>(::getpid())) + "_" +
               std::to_string(m_tokenSeq++);
    }

    // Send a ScreenCast method that returns a Request handle, then block
    // (respecting m_running) until its Response signal arrives. Returns
    // the Response (u, a{sv}) message — caller parses results — or null.
    DBusMessage *callAndWait(DBusMessage *call)
    {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *reply =
            dbus_connection_send_with_reply_and_block(m_dbus, call, 5000, &err);
        dbus_message_unref(call);
        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                LOG_WARN(std::string("VideoCaptureScreen(portal): call failed — ") + err.message);
                dbus_error_free(&err);
            }
            return nullptr;
        }
        // Reply carries the Request object path (o).
        const char *requestPath = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath,
                                   DBUS_TYPE_INVALID) || !requestPath)
        {
            dbus_message_unref(reply);
            if (dbus_error_is_set(&err)) dbus_error_free(&err);
            return nullptr;
        }
        std::string path = requestPath;
        dbus_message_unref(reply);

        // Pump the bus until the Response signal for `path` lands. The
        // Start request blocks on the user's portal dialog, so this can
        // take a while; bail promptly if stop() flips m_running.
        while (m_running.load())
        {
            if (!dbus_connection_read_write_dispatch(m_dbus, 200))
                return nullptr; // disconnected
            DBusMessage *msg = dbus_connection_pop_message(m_dbus);
            while (msg)
            {
                if (dbus_message_is_signal(msg, kRequestIf, "Response") &&
                    path == dbus_message_get_path(msg))
                {
                    return msg; // caller owns it
                }
                dbus_message_unref(msg);
                msg = dbus_connection_pop_message(m_dbus);
            }
        }
        return nullptr;
    }

    // results a{sv} → find a string value by key.
    static bool resultString(DBusMessageIter *results, const char *key, std::string &out)
    {
        DBusMessageIter arr = *results;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY)
        {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&arr, &entry);
            const char *k = nullptr;
            dbus_message_iter_get_basic(&entry, &k);
            dbus_message_iter_next(&entry);
            if (k && std::strcmp(k, key) == 0 &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT)
            {
                DBusMessageIter var;
                dbus_message_iter_recurse(&entry, &var);
                if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING)
                {
                    const char *v = nullptr;
                    dbus_message_iter_get_basic(&var, &v);
                    if (v) { out = v; return true; }
                }
            }
            dbus_message_iter_next(&arr);
        }
        return false;
    }

    // Position the iterator on the Response results dict (after the
    // leading 'u' response code). Returns response code, or -1.
    static int responseResults(DBusMessage *msg, DBusMessageIter *resultsOut)
    {
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it)) return -1;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return -1;
        dbus_uint32_t code = 0;
        dbus_message_iter_get_basic(&it, &code);
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return static_cast<int>(code);
        dbus_message_iter_recurse(&it, resultsOut);
        return static_cast<int>(code);
    }

    bool createSession(const std::string &sender, std::string &sessionHandle)
    {
        DBusMessage *call = dbus_message_new_method_call(
            kPortalService, kPortalPath, kScreenCastIf, "CreateSession");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(call, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        const std::string ht = "ct" + sender;
        const std::string st = "se" + sender;
        appendVariantString(&dict, "handle_token", ht.c_str());
        appendVariantString(&dict, "session_handle_token", st.c_str());
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage *resp = callAndWait(call);
        if (!resp) return false;
        DBusMessageIter results;
        const int code = responseResults(resp, &results);
        bool ok = (code == 0) && resultString(&results, "session_handle", sessionHandle);
        dbus_message_unref(resp);
        return ok;
    }

    bool selectSources(const std::string &session, const std::string &sender, bool cursor)
    {
        DBusMessage *call = dbus_message_new_method_call(
            kPortalService, kPortalPath, kScreenCastIf, "SelectSources");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(call, &args);
        const char *sh = session.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        const std::string ht = "ss" + sender;
        appendVariantString(&dict, "handle_token", ht.c_str());
        appendVariantUint(&dict, "types", 1u | 2u);    // monitor | window
        appendVariantBool(&dict, "multiple", FALSE);
        appendVariantUint(&dict, "cursor_mode", cursor ? 2u : 1u); // embedded | hidden
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage *resp = callAndWait(call);
        if (!resp) return false;
        DBusMessageIter results;
        const int code = responseResults(resp, &results);
        dbus_message_unref(resp);
        return code == 0;
    }

    bool startPortal(const std::string &session, const std::string &sender, uint32_t &nodeId)
    {
        DBusMessage *call = dbus_message_new_method_call(
            kPortalService, kPortalPath, kScreenCastIf, "Start");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(call, &args);
        const char *sh = session.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        const char *parent = "";
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        const std::string ht = "st" + sender;
        appendVariantString(&dict, "handle_token", ht.c_str());
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage *resp = callAndWait(call); // blocks on the user's dialog
        if (!resp) return false;
        DBusMessageIter results;
        const int code = responseResults(resp, &results);
        bool ok = false;
        if (code == 0)
        {
            // results["streams"] = a(ua{sv}); take the first node id.
            DBusMessageIter arr = results;
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY)
            {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&arr, &entry);
                const char *k = nullptr;
                dbus_message_iter_get_basic(&entry, &k);
                dbus_message_iter_next(&entry);
                if (k && std::strcmp(k, "streams") == 0 &&
                    dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT)
                {
                    DBusMessageIter var, streams, stream;
                    dbus_message_iter_recurse(&entry, &var);     // a(ua{sv})
                    if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY)
                    {
                        dbus_message_iter_recurse(&var, &streams);
                        if (dbus_message_iter_get_arg_type(&streams) == DBUS_TYPE_STRUCT)
                        {
                            dbus_message_iter_recurse(&streams, &stream);
                            if (dbus_message_iter_get_arg_type(&stream) == DBUS_TYPE_UINT32)
                            {
                                dbus_uint32_t id = 0;
                                dbus_message_iter_get_basic(&stream, &id);
                                nodeId = id;
                                ok = true;
                            }
                        }
                    }
                    break;
                }
                dbus_message_iter_next(&arr);
            }
        }
        dbus_message_unref(resp);
        return ok;
    }

    int openPipeWireRemote(const std::string &session)
    {
        DBusMessage *call = dbus_message_new_method_call(
            kPortalService, kPortalPath, kScreenCastIf, "OpenPipeWireRemote");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(call, &args);
        const char *sh = session.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_close_container(&args, &dict);

        DBusError err;
        dbus_error_init(&err);
        DBusMessage *reply =
            dbus_connection_send_with_reply_and_block(m_dbus, call, 5000, &err);
        dbus_message_unref(call);
        if (!reply)
        {
            if (dbus_error_is_set(&err)) dbus_error_free(&err);
            return -1;
        }
        int fd = -1;
        DBusError perr;
        dbus_error_init(&perr);
        if (!dbus_message_get_args(reply, &perr, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID))
        {
            if (dbus_error_is_set(&perr)) dbus_error_free(&perr);
            fd = -1;
        }
        dbus_message_unref(reply);
        return fd;
    }

    void runPipeWire(int pwFd, uint32_t nodeId)
    {
        pw_init(nullptr, nullptr);
        m_loop = pw_thread_loop_new("rc-screencast", nullptr);
        if (!m_loop) { ::close(pwFd); return; }

        pw_thread_loop_lock(m_loop);
        m_ctx = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
        if (!m_ctx) { pw_thread_loop_unlock(m_loop); teardownPipeWire(); ::close(pwFd); return; }

        m_core = pw_context_connect_fd(m_ctx, pwFd, nullptr, 0); // takes the fd
        if (!m_core) { pw_thread_loop_unlock(m_loop); teardownPipeWire(); return; }

        struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            nullptr);
        m_stream = pw_stream_new(m_core, "retrocapture-screencast", props);
        if (!m_stream) { pw_thread_loop_unlock(m_loop); teardownPipeWire(); return; }

        static const struct pw_stream_events kEvents = []() {
            struct pw_stream_events e {};
            e.version       = PW_VERSION_STREAM_EVENTS;
            e.param_changed = onStreamParamChanged;
            e.process       = onStreamProcess;
            return e;
        }();
        pw_stream_add_listener(m_stream, &m_streamListener, &kEvents, this);

        // Offer the packed-32-bit formats we can convert, no DMABUF
        // modifiers so the buffers come back as mappable memory.
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_rectangle defSize = SPA_RECTANGLE(1920, 1080);
        struct spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        struct spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);
        struct spa_fraction  defRate = SPA_FRACTION(60, 1);
        struct spa_fraction  minRate = SPA_FRACTION(0, 1);
        struct spa_fraction  maxRate = SPA_FRACTION(360, 1);
        const struct spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate)));

        pw_stream_connect(m_stream, PW_DIRECTION_INPUT, nodeId,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                       PW_STREAM_FLAG_MAP_BUFFERS),
                          params, 1);
        pw_thread_loop_unlock(m_loop);
        pw_thread_loop_start(m_loop);

        LOG_INFO("VideoCaptureScreen(pw): stream connected to node " + std::to_string(nodeId));

        // Hold here until stop() asks us to leave.
        while (m_running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        teardownPipeWire();
    }

    void teardownPipeWire()
    {
        if (m_loop) pw_thread_loop_stop(m_loop);
        if (m_stream) { pw_stream_destroy(m_stream); m_stream = nullptr; }
        if (m_core) { pw_core_disconnect(m_core); m_core = nullptr; }
        if (m_ctx) { pw_context_destroy(m_ctx); m_ctx = nullptr; }
        if (m_loop) { pw_thread_loop_destroy(m_loop); m_loop = nullptr; }
    }

    void cleanupDbus()
    {
        if (m_dbus)
        {
            // Private connections must be explicitly closed before unref.
            dbus_connection_close(m_dbus);
            dbus_connection_unref(m_dbus);
            m_dbus = nullptr;
        }
    }

    IScreenFrameSink     &m_sink;
    std::thread           m_thread;
    std::atomic<bool>     m_running{false};
    unsigned              m_tokenSeq = 0;

    DBusConnection       *m_dbus = nullptr;

    struct pw_thread_loop *m_loop   = nullptr;
    struct pw_context     *m_ctx    = nullptr;
    struct pw_core        *m_core   = nullptr;
    struct pw_stream      *m_stream = nullptr;
    struct spa_hook        m_streamListener {};
    struct spa_video_info_raw m_format {};
};
} // namespace

std::unique_ptr<ScreenBackend> createScreenBackend(IScreenFrameSink &sink)
{
    LOG_INFO("ScreenBackend: PipeWire + xdg-desktop-portal (Linux)");
    return std::make_unique<PipeWireScreenBackend>(sink);
}

#endif // RETROCAPTURE_SCREEN_PIPEWIRE
