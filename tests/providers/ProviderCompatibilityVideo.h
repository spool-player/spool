#pragma once

#include "TestRequire.h"
#include "player/MpvOptionProfile.h"

#include <QElapsedTimer>
#include <mpv/client.h>

#include <memory>

namespace ProviderCompatibility {

inline void verifyDecodedVideo(const Spool::PlaybackSession& playback, const QByteArray& headers = {},
    const std::vector<Spool::MpvOption>& transportOptions = {})
{
    using SpoolTests::require;
    std::unique_ptr<mpv_handle, decltype(&mpv_terminate_destroy)> player(mpv_create(), mpv_terminate_destroy);
    require(bool(player), "create the linked libmpv client");
    for (const auto& [key, value] :
        { std::pair { "config", "no" }, { "vo", "null" }, { "ao", "null" }, { "ytdl", "no" }, { "pause", "yes" } })
        require(mpv_set_option_string(player.get(), key, value) >= 0, "set isolated libmpv option");
    for (const auto& [key, value] : transportOptions)
        require(mpv_set_option_string(player.get(), key.constData(), value.constData()) >= 0,
            "set the fixture's isolated transport option");
    require(mpv_initialize(player.get()) >= 0, "initialize the linked libmpv client");
    require(Spool::MpvOptionProfile::applyRequestHeaders(player.get(), headers), "set the app's media headers");
    const QByteArray url = playback.url.toUtf8();
    const char *command[] { "loadfile", url.constData(), nullptr };
    require(mpv_command(player.get(), command) >= 0, "load the resolved provider stream");
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 15000) {
        const auto *event = mpv_wait_event(player.get(), 0.25);
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            int64_t width = 0;
            if (mpv_get_property(player.get(), "video-out-params/w", MPV_FORMAT_INT64, &width) >= 0 && width == 160)
                return;
        }
        if (event->event_id == MPV_EVENT_END_FILE)
            break;
    }
    require(false, "the bundled provider stream did not decode the synthetic video frame");
}

} // namespace ProviderCompatibility
