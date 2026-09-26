#include "platform/webos/WebOSMpvRuntime.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSet>
#include <QStringList>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <mpv/client.h>
#include <mpv/render.h>

extern "C" {
#include "video/out/starfish/starfish_ctx.h"
}

// Every mpv_*/starfish_* function the app calls. The list must stay in sync
// with actual usage: an entry missing here fails at app link time (undefined
// reference), an entry missing in libmpv fails loudly at load time — both are
// impossible to ship by accident.
#define MPV_RUNTIME_SYMBOLS(X)                                                                                         \
    X(mpv_create)                                                                                                      \
    X(mpv_initialize)                                                                                                  \
    X(mpv_destroy)                                                                                                     \
    X(mpv_terminate_destroy)                                                                                           \
    X(mpv_wait_event)                                                                                                  \
    X(mpv_request_log_messages)                                                                                        \
    X(mpv_observe_property)                                                                                            \
    X(mpv_set_option_string)                                                                                           \
    X(mpv_set_property)                                                                                                \
    X(mpv_set_property_async)                                                                                          \
    X(mpv_set_property_string)                                                                                         \
    X(mpv_get_property)                                                                                                \
    X(mpv_get_property_async)                                                                                          \
    X(mpv_get_property_string)                                                                                         \
    X(mpv_free)                                                                                                        \
    X(mpv_free_node_contents)                                                                                          \
    X(mpv_command)                                                                                                     \
    X(mpv_command_async)                                                                                               \
    X(mpv_command_ret)                                                                                                 \
    X(mpv_command_string)                                                                                              \
    X(mpv_error_string)                                                                                                \
    X(mpv_get_audio_decode_cpu_time_ns)                                                                                \
    X(mpv_render_context_create)                                                                                       \
    X(mpv_render_context_free)                                                                                         \
    X(mpv_render_context_render)                                                                                       \
    X(mpv_render_context_report_swap)                                                                                  \
    X(mpv_render_context_set_update_callback)                                                                          \
    X(mpv_render_context_update)                                                                                       \
    X(starfish_overlay_set_callbacks)                                                                                  \
    X(starfish_exported_set_crop_cb)

namespace {

struct MpvApi {
#define DECLARE_MEMBER(name) decltype(&::name) name = nullptr;
    MPV_RUNTIME_SYMBOLS(DECLARE_MEMBER)
#undef DECLARE_MEMBER
};

MpvApi g_api;
std::atomic<bool> g_loaded { false };
std::mutex g_loadLock;
std::mutex g_loadCallbackLock;
std::vector<std::function<void()>> g_loadCallbacks;

std::vector<std::function<void()>> takeLoadCallbacks()
{
    std::lock_guard<std::mutex> lock(g_loadCallbackLock);
    return std::move(g_loadCallbacks);
}

void runLoadCallbacks()
{
    for (auto& callback : takeLoadCallbacks()) {
        if (callback)
            callback();
    }
}

const char *mapString(const mpv_node& node, const char *key)
{
    if (node.format != MPV_FORMAT_NODE_MAP || !node.u.list)
        return nullptr;
    const mpv_node_list *map = node.u.list;
    if (!map->values)
        return nullptr;
    for (int index = 0; index < map->num; ++index) {
        if (!map->keys || !map->keys[index] || strcmp(map->keys[index], key) != 0)
            continue;
        const mpv_node& value = map->values[index];
        return value.format == MPV_FORMAT_STRING ? value.u.string : nullptr;
    }
    return nullptr;
}

QStringList probeStarfishVideoCodecs()
{
    QStringList codecs;
    mpv_handle *handle = mpv_create();
    if (!handle) {
        qWarning() << "playback capabilities: failed to create mpv probe core";
        return codecs;
    }

    mpv_set_option_string(handle, "config", "no");
    mpv_set_option_string(handle, "terminal", "no");
    mpv_set_option_string(handle, "vo", "null");
    mpv_set_option_string(handle, "ao", "null");
    const int initializeResult = mpv_initialize(handle);
    if (initializeResult < 0) {
        qWarning() << "playback capabilities: failed to initialize mpv probe core"
                   << mpv_error_string(initializeResult);
        mpv_destroy(handle);
        return codecs;
    }

    mpv_node decoderList {};
    const int propertyResult = mpv_get_property(handle, "decoder-list", MPV_FORMAT_NODE, &decoderList);
    if (propertyResult >= 0 && decoderList.format == MPV_FORMAT_NODE_ARRAY && decoderList.u.list) {
        QSet<QString> seen;
        const mpv_node_list *entries = decoderList.u.list;
        if (!entries->values) {
            mpv_free_node_contents(&decoderList);
            mpv_terminate_destroy(handle);
            return codecs;
        }
        for (int index = 0; index < entries->num; ++index) {
            const mpv_node& entry = entries->values[index];
            const char *driver = mapString(entry, "driver");
            const char *codec = mapString(entry, "codec");
            if (!driver || !codec || strcmp(driver, "starfish") != 0)
                continue;
            const QString normalized = QString::fromUtf8(codec).trimmed().toLower();
            if (!normalized.isEmpty() && !seen.contains(normalized)) {
                seen.insert(normalized);
                codecs.push_back(normalized);
            }
        }
    } else {
        qWarning() << "playback capabilities: mpv decoder-list query failed" << mpv_error_string(propertyResult);
    }
    if (propertyResult >= 0)
        mpv_free_node_contents(&decoderList);
    mpv_terminate_destroy(handle);
    return codecs;
}

// The NativeAppWindow constructor registers the starfish OSD/crop callbacks
// long before libmpv is loaded. Record them here and replay once the library
// is up; g_starfishReady flips under the same lock as the replay so a setter
// racing the load can never be lost.
std::mutex g_starfishLock;
bool g_starfishReady = false;
starfish_overlay_acquire_cb g_overlayAcquireCb = nullptr;
starfish_overlay_present_cb g_overlayPresentCb = nullptr;
void *g_overlayOpaque = nullptr;
starfish_exported_crop_cb g_cropCb = nullptr;
void *g_cropOpaque = nullptr;

std::string libmpvPath()
{
    char exe[PATH_MAX] = {};
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0)
        return "libmpv.so.2";
    std::string dir(exe, static_cast<size_t>(len));
    const size_t slash = dir.rfind('/');
    if (slash == std::string::npos)
        return "libmpv.so.2";
    // <app>/bin/spool -> <app>/lib/libmpv.so.2 (matches the rpath
    // layout build-ipk.sh stages).
    return dir.substr(0, slash) + "/../lib/libmpv.so.2";
}

bool loadNow()
{
    QElapsedTimer timer;
    timer.start();

    const std::string path = libmpvPath();
    // RTLD_LAZY, not RTLD_NOW: LG's proprietary starfish libraries in
    // libmpv's dependency closure are under-linked against system libs (e.g.
    // a reference to curl_easy_header, which the TV's libcurl 7.7x does not
    // export). DT_NEEDED loading always used lazy PLT binding, so those
    // dangling references were and remain harmless — RTLD_NOW turns them
    // into a hard dlopen failure.
    void *handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        handle = dlopen("libmpv.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        qWarning() << "MpvRuntime: dlopen failed:" << dlerror();
        return false;
    }

    MpvApi api;
    bool ok = true;
#define RESOLVE_MEMBER(name)                                                                                           \
    api.name = reinterpret_cast<decltype(&::name)>(dlsym(handle, #name));                                              \
    if (!api.name) {                                                                                                   \
        qWarning() << "MpvRuntime: missing symbol" << #name;                                                           \
        ok = false;                                                                                                    \
    }
    MPV_RUNTIME_SYMBOLS(RESOLVE_MEMBER)
#undef RESOLVE_MEMBER
    if (!ok) {
        dlclose(handle);
        return false;
    }
    g_api = api;

    {
        std::lock_guard<std::mutex> lock(g_starfishLock);
        g_starfishReady = true;
        g_api.starfish_overlay_set_callbacks(g_overlayAcquireCb, g_overlayPresentCb, g_overlayOpaque);
        g_api.starfish_exported_set_crop_cb(g_cropCb, g_cropOpaque);
    }

    qInfo() << "MpvRuntime: libmpv loaded in" << timer.elapsed() << "ms from" << path.c_str();
    return true;
}

} // namespace

namespace Spool::WebOSMpvRuntime {

bool ensureLoaded()
{
    if (g_loaded.load(std::memory_order_acquire))
        return true;

    bool loadedNow = false;
    // Not call_once: a failed attempt must stay retryable, otherwise one
    // transient dlopen failure kills playback for the whole session. Repeat
    // attempts are user-triggered (play) and cheap.
    {
        std::lock_guard<std::mutex> lock(g_loadLock);
        if (g_loaded.load(std::memory_order_relaxed))
            return true;
        if (!loadNow())
            return false;
        g_loaded.store(true, std::memory_order_release);
        loadedNow = true;
    }

    if (loadedNow)
        runLoadCallbacks();
    return true;
}

int64_t audioDecodeCpuTimeNs()
{
    if (!g_loaded.load(std::memory_order_acquire))
        return -1;
    return static_cast<int64_t>(g_api.mpv_get_audio_decode_cpu_time_ns());
}

void runAfterLoaded(std::function<void()> callback)
{
    if (!callback)
        return;

    bool runNow = false;
    {
        std::lock_guard<std::mutex> lock(g_loadCallbackLock);
        runNow = g_loaded.load(std::memory_order_acquire);
        if (!runNow)
            g_loadCallbacks.push_back(std::move(callback));
    }

    if (runNow)
        callback();
}

void preloadAsync()
{
    std::thread([] { ensureLoaded(); }).detach();
}

void probeStarfishVideoCodecsAsync(std::function<void(QStringList)> callback)
{
    if (!callback)
        return;
    // Preserve the first-frame lazy-load boundary: registering this work does
    // not load libmpv. preloadAsync() releases it after the UI is visible.
    runAfterLoaded([callback = std::move(callback)]() mutable {
        std::thread([callback = std::move(callback)]() mutable { callback(probeStarfishVideoCodecs()); }).detach();
    });
}

} // namespace Spool::WebOSMpvRuntime

// ---------------------------------------------------------------------------
// mpv client API entry points. Declarations come from the real headers, so
// signatures cannot drift. All of them funnel through ensureLoaded(): the
// common case is a no-op atomic load; the cold case (playback started before
// the post-first-frame preload finished) blocks on the dlopen.

mpv_handle *mpv_create(void)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return nullptr;
    return g_api.mpv_create();
}

int mpv_initialize(mpv_handle *ctx)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_initialize(ctx);
}

void mpv_destroy(mpv_handle *ctx)
{
    if (ctx && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_destroy(ctx);
}

void mpv_terminate_destroy(mpv_handle *ctx)
{
    if (ctx && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_terminate_destroy(ctx);
}

mpv_event *mpv_wait_event(mpv_handle *ctx, double timeout)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return nullptr;
    return g_api.mpv_wait_event(ctx, timeout);
}

int mpv_request_log_messages(mpv_handle *ctx, const char *min_level)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_request_log_messages(ctx, min_level);
}

int mpv_observe_property(mpv_handle *mpv, uint64_t reply_userdata, const char *name, mpv_format format)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_observe_property(mpv, reply_userdata, name, format);
}

int mpv_set_option_string(mpv_handle *ctx, const char *name, const char *data)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_set_option_string(ctx, name, data);
}

int mpv_set_property(mpv_handle *ctx, const char *name, mpv_format format, void *data)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_set_property(ctx, name, format, data);
}

int mpv_set_property_async(mpv_handle *ctx, uint64_t reply_userdata, const char *name, mpv_format format, void *data)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_set_property_async(ctx, reply_userdata, name, format, data);
}

int mpv_set_property_string(mpv_handle *ctx, const char *name, const char *data)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_set_property_string(ctx, name, data);
}

int mpv_get_property(mpv_handle *ctx, const char *name, mpv_format format, void *data)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_get_property(ctx, name, format, data);
}

int mpv_get_property_async(mpv_handle *ctx, uint64_t reply_userdata, const char *name, mpv_format format)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_get_property_async(ctx, reply_userdata, name, format);
}

void mpv_free_node_contents(mpv_node *node)
{
    if (node && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_free_node_contents(node);
}

char *mpv_get_property_string(mpv_handle *ctx, const char *name)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return nullptr;
    return g_api.mpv_get_property_string(ctx, name);
}

// This frees what mpv allocated, so it has to reach mpv's own allocator rather
// than ::free. A handle that never loaded has nothing outstanding to release.
void mpv_free(void *data)
{
    if (data && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_free(data);
}

int mpv_command(mpv_handle *ctx, const char **args)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_command(ctx, args);
}

int mpv_command_ret(mpv_handle *ctx, const char **args, mpv_node *result)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_command_ret(ctx, args, result);
}

int mpv_command_async(mpv_handle *ctx, uint64_t reply_userdata, const char **args)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_command_async(ctx, reply_userdata, args);
}

int mpv_command_string(mpv_handle *ctx, const char *args)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_command_string(ctx, args);
}

const char *mpv_error_string(int error)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return "libmpv is not loaded";
    return g_api.mpv_error_string(error);
}

// Render API: the Starfish path does not use it, but software-decoded video
// renders through MpvVideoItem and therefore requires these runtime forwards.

int mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv, mpv_render_param *params)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_render_context_create(res, mpv, params);
}

void mpv_render_context_free(mpv_render_context *ctx)
{
    if (ctx && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_render_context_free(ctx);
}

int mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params)
{
    if (!Spool::WebOSMpvRuntime::ensureLoaded())
        return MPV_ERROR_GENERIC;
    return g_api.mpv_render_context_render(ctx, params);
}

void mpv_render_context_report_swap(mpv_render_context *ctx)
{
    if (ctx && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_render_context_report_swap(ctx);
}

void mpv_render_context_set_update_callback(mpv_render_context *ctx, mpv_render_update_fn callback, void *callback_ctx)
{
    if (ctx && Spool::WebOSMpvRuntime::ensureLoaded())
        g_api.mpv_render_context_set_update_callback(ctx, callback, callback_ctx);
}

uint64_t mpv_render_context_update(mpv_render_context *ctx)
{
    if (!ctx || !Spool::WebOSMpvRuntime::ensureLoaded())
        return 0;
    return g_api.mpv_render_context_update(ctx);
}

// Starfish OSD/crop callback setters: called from the NativeAppWindow
// constructor before libmpv exists. Store-and-replay instead of forcing a
// synchronous load on the startup path.

void starfish_overlay_set_callbacks(
    starfish_overlay_acquire_cb acquireCb, starfish_overlay_present_cb presentCb, void *opaque)
{
    std::lock_guard<std::mutex> lock(g_starfishLock);
    g_overlayAcquireCb = acquireCb;
    g_overlayPresentCb = presentCb;
    g_overlayOpaque = opaque;
    if (g_starfishReady)
        g_api.starfish_overlay_set_callbacks(acquireCb, presentCb, opaque);
}

void starfish_exported_set_crop_cb(starfish_exported_crop_cb cb, void *opaque)
{
    std::lock_guard<std::mutex> lock(g_starfishLock);
    g_cropCb = cb;
    g_cropOpaque = opaque;
    if (g_starfishReady)
        g_api.starfish_exported_set_crop_cb(cb, opaque);
}
