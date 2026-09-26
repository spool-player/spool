#pragma once

#include <QStringList>

#include <cstdint>
#include <functional>

// webOS-only lazy loader for libmpv.
//
// The app deliberately does not link libmpv (and therefore ffmpeg/lua) at
// build time: as DT_NEEDED entries they would be mapped, relocated and
// symbol-resolved by the dynamic linker before main() runs, which costs a
// large share of cold-launch time for ~40 MB of player code the home screen
// never touches. Instead, WebOSMpvRuntime.cpp defines
// every mpv_*/starfish_* entry point the app uses and forwards through
// dlsym'd pointers; the library is dlopen'd on a background thread after the
// first frame, or on demand at the first mpv call, whichever comes first.
namespace Spool::WebOSMpvRuntime {

// Start loading libmpv on a detached background thread. Idempotent; call
// after the first frame has been presented.
void preloadAsync();

// After the lazy libmpv load completes, initialize a disposable mpv core on a
// worker and return the codecs registered by the custom Starfish decoder. The
// callback runs on that worker; callers must marshal QObject access back to
// its owning thread.
void probeStarfishVideoCodecsAsync(std::function<void(QStringList)> callback);

// Register work to run once libmpv has loaded successfully. If libmpv is
// already loaded, the callback runs immediately on the calling thread; otherwise
// it runs on the thread that completes the load.
void runAfterLoaded(std::function<void()> callback);

// Block until libmpv is loaded (performs the dlopen on the calling thread if
// the preload has not run yet). Returns false if loading failed; the failure
// is sticky and logged once.
bool ensureLoaded();

// Return cumulative libavcodec audio-decoder thread CPU time, or -1 while
// libmpv is not loaded. Unlike the forwarding entry points, this never forces
// a load from the startup diagnostics timer.
int64_t audioDecodeCpuTimeNs();

} // namespace Spool::WebOSMpvRuntime
