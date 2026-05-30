// Linux system-tray backend — StatusNotifierItem (SNI) over D-Bus.
//
// Modern freedesktop tray standard: KDE/Plasma, XFCE 4.14+, Cinnamon,
// MATE support it natively; GNOME via the AppIndicator extension. No
// GTK dependency — we drive libdbus directly.
//
// We expose two objects on a private session-bus connection:
//   /StatusNotifierItem  → org.kde.StatusNotifierItem (icon + Activate)
//   /MenuBar             → com.canonical.dbusmenu      (the context menu)
// and register ourselves with org.kde.StatusNotifierWatcher.
//
// All D-Bus traffic is serviced from pump() on the main thread, so the
// menu/activate callbacks run there too — callers can touch app state
// directly.
//
// Compiled only when CMake defines RETROCAPTURE_TRAY_BACKEND_LINUX
// (i.e. libdbus-1 was found). createSystemTrayLinux() returns nullptr
// if no session bus / no watcher is present, so the factory falls back
// to the no-op stub.

#include "ISystemTray.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#include <dbus/dbus.h>
#include <png.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace retrocapture {

namespace {

constexpr const char *kSniIface     = "org.kde.StatusNotifierItem";
constexpr const char *kSniPath      = "/StatusNotifierItem";
constexpr const char *kMenuIface    = "com.canonical.dbusmenu";
constexpr const char *kMenuPath     = "/MenuBar";
constexpr const char *kWatcherName  = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherPath  = "/StatusNotifierWatcher";
constexpr const char *kPropsIface   = "org.freedesktop.DBus.Properties";

// ── small marshalling helpers ─────────────────────────────────────

void appendVariantString(DBusMessageIter *iter, const char *value)
{
    DBusMessageIter v;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(iter, &v);
}

void appendVariantBool(DBusMessageIter *iter, bool value)
{
    DBusMessageIter v;
    dbus_bool_t b = value ? TRUE : FALSE;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(iter, &v);
}

void appendVariantObjectPath(DBusMessageIter *iter, const char *path)
{
    DBusMessageIter v;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "o", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(iter, &v);
}

// Append one dbusmenu property entry {sv} into an open a{sv} iter.
void appendMenuPropString(DBusMessageIter *dict, const char *key, const char *value)
{
    DBusMessageIter entry;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    appendVariantString(&entry, value);
    dbus_message_iter_close_container(dict, &entry);
}

void appendMenuPropBool(DBusMessageIter *dict, const char *key, bool value)
{
    DBusMessageIter entry;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    appendVariantBool(&entry, value);
    dbus_message_iter_close_container(dict, &entry);
}

void appendMenuPropInt(DBusMessageIter *dict, const char *key, int32_t value)
{
    DBusMessageIter entry, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &value);
    dbus_message_iter_close_container(&entry, &v);
    dbus_message_iter_close_container(dict, &entry);
}

// ── icon pixmap (our logo → SNI IconPixmap) ───────────────────────

// Decode an RGBA8 PNG. Returns false on any error. Output is tightly
// packed width*height*4 RGBA.
bool loadPngRgba(const std::string &path, uint32_t &w, uint32_t &h,
                 std::vector<uint8_t> &rgba)
{
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); std::fclose(fp); return false; }
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    w = png_get_image_width(png, info);
    h = png_get_image_height(png, info);
    png_byte colorType = png_get_color_type(png, info);
    png_byte bitDepth  = png_get_bit_depth(png, info);

    // Normalise everything to 8-bit RGBA.
    if (bitDepth == 16) png_set_strip_16(png);
    if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY ||
        colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    rgba.resize(static_cast<size_t>(w) * h * 4);
    std::vector<png_bytep> rows(h);
    for (uint32_t y = 0; y < h; ++y)
        rows[y] = rgba.data() + static_cast<size_t>(y) * w * 4;
    png_read_image(png, rows.data());

    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return true;
}

// Nearest-neighbour downscale RGBA → RGBA. Tray hosts re-scale the
// pixmap anyway, so a cheap resample to ~48px keeps the D-Bus payload
// small without visible quality loss at icon sizes.
std::vector<uint8_t> downscaleRgba(const std::vector<uint8_t> &src,
                                   uint32_t sw, uint32_t sh,
                                   uint32_t dw, uint32_t dh)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint32_t sy = (y * sh) / dh;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t sx = (x * sw) / dw;
            const uint8_t *s = &src[(static_cast<size_t>(sy) * sw + sx) * 4];
            uint8_t *d = &dst[(static_cast<size_t>(y) * dw + x) * 4];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return dst;
}

// Build the SNI IconPixmap byte buffer (ARGB32, network/big-endian
// byte order: A,R,G,B per pixel) from RGBA8.
std::vector<uint8_t> rgbaToArgbNetwork(const std::vector<uint8_t> &rgba)
{
    std::vector<uint8_t> argb(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        argb[i + 0] = rgba[i + 3]; // A
        argb[i + 1] = rgba[i + 0]; // R
        argb[i + 2] = rgba[i + 1]; // G
        argb[i + 3] = rgba[i + 2]; // B
    }
    return argb;
}

class SystemTrayLinux : public ISystemTray
{
public:
    ~SystemTrayLinux() override { stop(); }

    bool isSupported() const override { return m_conn != nullptr; }

    bool start(const std::string &iconName, const std::string &tooltip) override;
    void stop() override;

    void setMenu(const std::vector<TrayMenuItem> &items) override
    {
        m_items = items;
        ++m_menuRevision;
        emitLayoutUpdated();
    }
    void updateMenu(const std::vector<TrayMenuItem> &items) override
    {
        setMenu(items);
    }
    void setOnActivate(std::function<void()> cb) override { m_onActivate = std::move(cb); }
    void notify(const std::string &title, const std::string &body) override;

    void pump() override;

    // Object-path message handlers (static thunks → member dispatch).
    DBusHandlerResult handleSni(DBusMessage *msg);
    DBusHandlerResult handleMenu(DBusMessage *msg);

private:
    void emitLayoutUpdated();
    void emitSignal(const char *path, const char *iface, const char *name);

    DBusHandlerResult sniGetProperty(DBusMessage *msg, const char *prop);
    DBusHandlerResult sniGetAll(DBusMessage *msg);
    void appendSniProp(DBusMessageIter *iter, const char *prop);

    void loadIconPixmap();
    void appendVariantIconPixmap(DBusMessageIter *iter);

    DBusConnection *m_conn = nullptr;
    std::string     m_iconName;
    std::string     m_tooltip;
    std::string     m_busName;

    // Branded icon as an SNI IconPixmap (ARGB32, network byte order).
    // Loaded from the bundled logo.png so the tray shows our icon on
    // any desktop without needing a themed icon installed.
    uint32_t             m_iconW = 0;
    uint32_t             m_iconH = 0;
    std::vector<uint8_t> m_iconArgb;

    std::vector<TrayMenuItem> m_items;
    uint32_t                  m_menuRevision = 1;

    std::function<void()> m_onActivate;
};

// Object-path vtable thunks.
DBusHandlerResult sniThunk(DBusConnection *, DBusMessage *msg, void *user)
{
    return static_cast<SystemTrayLinux *>(user)->handleSni(msg);
}
DBusHandlerResult menuThunk(DBusConnection *, DBusMessage *msg, void *user)
{
    return static_cast<SystemTrayLinux *>(user)->handleMenu(msg);
}

bool SystemTrayLinux::start(const std::string &iconName, const std::string &tooltip)
{
    m_iconName = iconName;
    m_tooltip  = tooltip;
    loadIconPixmap();

    DBusError err;
    dbus_error_init(&err);

    m_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err) || !m_conn)
    {
        LOG_WARN(std::string("tray(linux): no session bus: ") +
                 (err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        m_conn = nullptr;
        return false;
    }
    // We service the bus manually from pump(); don't let libdbus exit
    // the process if the bus drops.
    dbus_connection_set_exit_on_disconnect(m_conn, FALSE);

    // Unique per-process bus name required by the SNI spec.
    m_busName = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    int rc = dbus_bus_request_name(m_conn, m_busName.c_str(),
                                   DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err) || rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
        LOG_WARN(std::string("tray(linux): request_name failed: ") +
                 (err.message ? err.message : "not primary owner"));
        dbus_error_free(&err);
        stop();
        return false;
    }

    // Export our two objects.
    static DBusObjectPathVTable sniVtbl  = { nullptr, sniThunk,  nullptr, nullptr, nullptr, nullptr };
    static DBusObjectPathVTable menuVtbl = { nullptr, menuThunk, nullptr, nullptr, nullptr, nullptr };
    if (!dbus_connection_register_object_path(m_conn, kSniPath,  &sniVtbl,  this) ||
        !dbus_connection_register_object_path(m_conn, kMenuPath, &menuVtbl, this))
    {
        LOG_WARN("tray(linux): failed to register object paths");
        stop();
        return false;
    }

    // Register with the watcher. If there's no watcher on the bus the
    // call errors → no tray host → report unsupported so the app falls
    // back to quit-on-close.
    DBusMessage *reg = dbus_message_new_method_call(
        kWatcherName, kWatcherPath, kWatcherName,
        "RegisterStatusNotifierItem");
    if (reg)
    {
        const char *busNameC = m_busName.c_str();
        dbus_message_append_args(reg, DBUS_TYPE_STRING, &busNameC, DBUS_TYPE_INVALID);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(
            m_conn, reg, 2000, &err);
        dbus_message_unref(reg);
        if (dbus_error_is_set(&err) || !reply)
        {
            LOG_WARN(std::string("tray(linux): no StatusNotifierWatcher "
                     "(GNOME needs the AppIndicator extension): ") +
                     (err.message ? err.message : "no reply"));
            dbus_error_free(&err);
            stop();
            return false;
        }
        dbus_message_unref(reply);
    }

    LOG_INFO("tray(linux): registered StatusNotifierItem " + m_busName);
    return true;
}

void SystemTrayLinux::stop()
{
    if (m_conn)
    {
        dbus_connection_unregister_object_path(m_conn, kSniPath);
        dbus_connection_unregister_object_path(m_conn, kMenuPath);
        dbus_connection_close(m_conn);  // private connections must be closed
        dbus_connection_unref(m_conn);
        m_conn = nullptr;
    }
}

void SystemTrayLinux::notify(const std::string &title, const std::string &body)
{
    if (!m_conn) return;
    // org.freedesktop.Notifications.Notify(
    //   s app_name, u replaces_id, s app_icon, s summary, s body,
    //   as actions, a{sv} hints, i expire_timeout) -> u id
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify");
    if (!msg) return;

    DBusMessageIter args;
    dbus_message_iter_init_append(msg, &args);
    const char *appName = "RetroCapture";
    const char *appIcon = m_iconName.c_str(); // themed fallback name
    const char *summary = title.c_str();
    const char *bodyC   = body.c_str();
    dbus_uint32_t replacesId = 0;
    dbus_int32_t  expireMs   = 5000;
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &appName);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replacesId);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &appIcon);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &bodyC);
    // empty actions (as)
    DBusMessageIter actions;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actions);
    dbus_message_iter_close_container(&args, &actions);
    // empty hints (a{sv})
    DBusMessageIter hints;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hints);
    dbus_message_iter_close_container(&args, &hints);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &expireMs);

    // Fire-and-forget; the notification daemon may be absent and that's
    // fine (best-effort).
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(m_conn, msg, nullptr);
    dbus_message_unref(msg);
    dbus_connection_flush(m_conn);
}

void SystemTrayLinux::pump()
{
    if (!m_conn) return;
    // Non-blocking: read whatever arrived, then dispatch all queued.
    dbus_connection_read_write(m_conn, 0);
    while (dbus_connection_dispatch(m_conn) == DBUS_DISPATCH_DATA_REMAINS)
    {
    }
}

void SystemTrayLinux::emitSignal(const char *path, const char *iface, const char *name)
{
    if (!m_conn) return;
    DBusMessage *sig = dbus_message_new_signal(path, iface, name);
    if (!sig) return;
    dbus_connection_send(m_conn, sig, nullptr);
    dbus_message_unref(sig);
    dbus_connection_flush(m_conn);
}

void SystemTrayLinux::emitLayoutUpdated()
{
    if (!m_conn) return;
    DBusMessage *sig = dbus_message_new_signal(kMenuPath, kMenuIface, "LayoutUpdated");
    if (!sig) return;
    dbus_uint32_t rev = m_menuRevision;
    dbus_int32_t  parent = 0;
    dbus_message_append_args(sig,
        DBUS_TYPE_UINT32, &rev,
        DBUS_TYPE_INT32,  &parent,
        DBUS_TYPE_INVALID);
    dbus_connection_send(m_conn, sig, nullptr);
    dbus_message_unref(sig);
    dbus_connection_flush(m_conn);
}

// ── StatusNotifierItem object ─────────────────────────────────────

void SystemTrayLinux::loadIconPixmap()
{
    // Assets live under <base>/assets/ (getReadOnlyAssetsDir() returns
    // the dev/install BASE, not the assets dir itself — same as the
    // i18n/font loaders compose it).
    std::string path = Paths::getReadOnlyAssetsDir();
    if (!path.empty() && path.back() != '/') path += '/';
    path += "assets/logo.png";

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!loadPngRgba(path, w, h, rgba) || w == 0 || h == 0)
    {
        LOG_WARN("tray(linux): couldn't load icon " + path +
                 " — falling back to themed name '" + m_iconName + "'");
        return;
    }

    // Downscale large source (logo.png is 1024²) to a tray-friendly
    // size; hosts re-scale but a smaller payload keeps D-Bus light.
    constexpr uint32_t kTarget = 48;
    if (w > kTarget || h > kTarget)
    {
        rgba = downscaleRgba(rgba, w, h, kTarget, kTarget);
        w = kTarget; h = kTarget;
    }

    m_iconArgb = rgbaToArgbNetwork(rgba);
    m_iconW = w;
    m_iconH = h;
    LOG_INFO("tray(linux): loaded icon pixmap " + std::to_string(w) + "x" +
             std::to_string(h));
}

// Build a variant of type "a(iiay)" holding our single ARGB pixmap.
void SystemTrayLinux::appendVariantIconPixmap(DBusMessageIter *iter)
{
    DBusMessageIter var, arr, strct, bytes;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a(iiay)", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(iiay)", &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &strct);
    dbus_int32_t w = static_cast<dbus_int32_t>(m_iconW);
    dbus_int32_t h = static_cast<dbus_int32_t>(m_iconH);
    dbus_message_iter_append_basic(&strct, DBUS_TYPE_INT32, &w);
    dbus_message_iter_append_basic(&strct, DBUS_TYPE_INT32, &h);
    dbus_message_iter_open_container(&strct, DBUS_TYPE_ARRAY, "y", &bytes);
    if (!m_iconArgb.empty())
    {
        const uint8_t *p = m_iconArgb.data();
        int n = static_cast<int>(m_iconArgb.size());
        dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &p, n);
    }
    dbus_message_iter_close_container(&strct, &bytes);
    dbus_message_iter_close_container(&arr, &strct);
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(iter, &var);
}

void SystemTrayLinux::appendSniProp(DBusMessageIter *iter, const char *prop)
{
    if (std::strcmp(prop, "Category") == 0)        appendVariantString(iter, "ApplicationStatus");
    else if (std::strcmp(prop, "Id") == 0)         appendVariantString(iter, "retrocapture");
    else if (std::strcmp(prop, "Title") == 0)      appendVariantString(iter, m_tooltip.c_str());
    else if (std::strcmp(prop, "Status") == 0)     appendVariantString(iter, "Active");
    // When we have our branded pixmap, advertise an EMPTY IconName so
    // hosts that prefer a themed name don't override the pixmap with
    // the generic "camera-video" fallback. The themed name is only
    // used when the pixmap failed to load.
    else if (std::strcmp(prop, "IconName") == 0)
        appendVariantString(iter, m_iconArgb.empty() ? m_iconName.c_str() : "");
    else if (std::strcmp(prop, "IconPixmap") == 0) appendVariantIconPixmap(iter);
    else if (std::strcmp(prop, "Menu") == 0)       appendVariantObjectPath(iter, kMenuPath);
    else if (std::strcmp(prop, "ItemIsMenu") == 0) appendVariantBool(iter, false);
    else                                            appendVariantString(iter, "");
}

DBusHandlerResult SystemTrayLinux::sniGetProperty(DBusMessage *msg, const char *prop)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    appendSniProp(&iter, prop);
    dbus_connection_send(m_conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult SystemTrayLinux::sniGetAll(DBusMessage *msg)
{
    std::vector<const char *> props = {
        "Category", "Id", "Title", "Status", "IconName", "Menu", "ItemIsMenu"
    };
    if (!m_iconArgb.empty()) props.push_back("IconPixmap");
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    for (const char *p : props)
    {
        DBusMessageIter entry;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &p);
        appendSniProp(&entry, p);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&iter, &dict);
    dbus_connection_send(m_conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult SystemTrayLinux::handleSni(DBusMessage *msg)
{
    // org.freedesktop.DBus.Properties.Get / GetAll
    if (dbus_message_is_method_call(msg, kPropsIface, "Get"))
    {
        const char *iface = nullptr, *prop = nullptr;
        dbus_message_get_args(msg, nullptr,
            DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
        if (prop) return sniGetProperty(msg, prop);
    }
    if (dbus_message_is_method_call(msg, kPropsIface, "GetAll"))
    {
        return sniGetAll(msg);
    }

    // Activate(x,y) / SecondaryActivate / ContextMenu / Scroll.
    if (dbus_message_is_method_call(msg, kSniIface, "Activate") ||
        dbus_message_is_method_call(msg, kSniIface, "SecondaryActivate"))
    {
        if (m_onActivate) m_onActivate();
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(m_conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kSniIface, "ContextMenu") ||
        dbus_message_is_method_call(msg, kSniIface, "Scroll"))
    {
        // The host renders the Menu property itself; just ack.
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(m_conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// ── com.canonical.dbusmenu object ─────────────────────────────────

DBusHandlerResult SystemTrayLinux::handleMenu(DBusMessage *msg)
{
    // GetLayout(parentId i, depth i, props as) → (u, (ia{sv}av))
    if (dbus_message_is_method_call(msg, kMenuIface, "GetLayout"))
    {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);

        dbus_uint32_t rev = m_menuRevision;
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &rev);

        // Root item: (id=0, props, children av)
        DBusMessageIter root, rootProps, children;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr, &root);
        dbus_int32_t rootId = 0;
        dbus_message_iter_append_basic(&root, DBUS_TYPE_INT32, &rootId);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &rootProps);
        appendMenuPropString(&rootProps, "children-display", "submenu");
        dbus_message_iter_close_container(&root, &rootProps);

        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "v", &children);
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            const TrayMenuItem &it = m_items[i];
            DBusMessageIter childVar, childStruct, childProps, grandkids;
            dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &childVar);
            dbus_message_iter_open_container(&childVar, DBUS_TYPE_STRUCT, nullptr, &childStruct);

            dbus_int32_t id = static_cast<dbus_int32_t>(i + 1); // 0 reserved for root
            dbus_message_iter_append_basic(&childStruct, DBUS_TYPE_INT32, &id);

            dbus_message_iter_open_container(&childStruct, DBUS_TYPE_ARRAY, "{sv}", &childProps);
            if (it.type == TrayMenuItem::Type::Separator)
            {
                appendMenuPropString(&childProps, "type", "separator");
            }
            else
            {
                appendMenuPropString(&childProps, "label", it.label.c_str());
                appendMenuPropBool(&childProps, "enabled", it.enabled);
                appendMenuPropBool(&childProps, "visible", true);
                if (it.type == TrayMenuItem::Type::Checkbox)
                {
                    appendMenuPropString(&childProps, "toggle-type", "checkmark");
                    appendMenuPropInt(&childProps, "toggle-state", it.checked ? 1 : 0);
                }
            }
            dbus_message_iter_close_container(&childStruct, &childProps);

            // No grandchildren — empty av.
            dbus_message_iter_open_container(&childStruct, DBUS_TYPE_ARRAY, "v", &grandkids);
            dbus_message_iter_close_container(&childStruct, &grandkids);

            dbus_message_iter_close_container(&childVar, &childStruct);
            dbus_message_iter_close_container(&children, &childVar);
        }
        dbus_message_iter_close_container(&root, &children);
        dbus_message_iter_close_container(&iter, &root);

        dbus_connection_send(m_conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // Event(id i, eventId s, data v, timestamp u) → on "clicked" invoke
    if (dbus_message_is_method_call(msg, kMenuIface, "Event"))
    {
        DBusMessageIter args;
        if (dbus_message_iter_init(msg, &args))
        {
            dbus_int32_t id = 0;
            dbus_message_iter_get_basic(&args, &id);
            dbus_message_iter_next(&args);
            const char *eventId = nullptr;
            if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&args, &eventId);

            if (eventId && std::strcmp(eventId, "clicked") == 0)
            {
                size_t idx = static_cast<size_t>(id - 1);
                if (idx < m_items.size() && m_items[idx].onClick)
                {
                    m_items[idx].onClick();
                }
            }
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(m_conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // AboutToShow(id i) → b (return false = no layout change needed)
    if (dbus_message_is_method_call(msg, kMenuIface, "AboutToShow"))
    {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_bool_t needUpdate = FALSE;
        dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &needUpdate, DBUS_TYPE_INVALID);
        dbus_connection_send(m_conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // Properties.GetAll on the menu → Version/Status/TextDirection
    if (dbus_message_is_method_call(msg, kPropsIface, "GetAll"))
    {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter, dict;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
        {
            DBusMessageIter e, v;
            const char *k = "Version";
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
            dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
            dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "u", &v);
            dbus_uint32_t ver = 3;
            dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &ver);
            dbus_message_iter_close_container(&e, &v);
            dbus_message_iter_close_container(&dict, &e);
        }
        appendMenuPropString(&dict, "Status", "normal");
        appendMenuPropString(&dict, "TextDirection", "ltr");
        dbus_message_iter_close_container(&iter, &dict);
        dbus_connection_send(m_conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

} // namespace

std::unique_ptr<ISystemTray> createSystemTrayLinux()
{
    auto tray = std::make_unique<SystemTrayLinux>();
    // Don't start() here — the app calls start() with the icon name.
    // We always hand back the object; isSupported()/start() report
    // whether a real tray host accepted it.
    return tray;
}

} // namespace retrocapture
