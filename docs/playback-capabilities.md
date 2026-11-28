# Playback capabilities in 0.10.2

The audit used the artifacts from `6f843d93` and mpv `d029fd1ea` in [release build 45649321213](https://github.com/spool-player/spool/actions/runs/45649321213). The test host was x86-64 NixOS with isolated loopback, network namespaces and software OpenGL/Vulkan. Physical devices, hardware acceleration, display HDR and physical audio output were outside the runner's coverage.

| Package | Transport evidence from the package | Video and audio paths | Runtime evidence |
| --- | --- | --- | --- |
| Linux x86-64 portable tarball | Loaded bundled curl 8.23.0 with OpenSSL, nghttp2, nghttp3 and ngtcp2. Verified HTTP/1.1, HTTP/2 and HTTP/3 range traffic. | The shipped core lists `libmpv`, `vaapi` and `vdpau` VOs and PipeWire, PulseAudio and ALSA AOs. Spool uses the `libmpv` render API with `auto-copy` for hardware decoding and defaults to mpv's audio device selection. | Independent server range logs and byte-exact decoded PCM over all three protocols. H.264 playback and seeks against Jellyfin. |
| macOS arm64 and x86-64 app payloads | Both payloads contain `libmpv`, curl and the nghttp2/nghttp3/ngtcp2 closure. The curl Mach-O load commands resolve the packaged dylibs. | Embedded `libmpv` with `auto-safe`. The FFmpeg build includes VideoToolbox and the mpv build targets CoreAudio. | Artifact inspection on Linux. VideoToolbox and HDR negotiation require macOS hardware. |
| Windows x64 portable | Extracted `mpv-2.dll` contains the static ngtcp2/nghttp3 transport and imports Winsock and Crypt32. | Embedded `libmpv` renders into Qt's target. FFmpeg/mpv enable D3D11VA and DXVA2 decoding, with WASAPI as the native audio output. Native mpv D3D11/Vulkan VOs are disabled, independently of Qt's rendering backend. | Artifact inspection on Linux. Native hardware decoder selection and WASAPI device routing require Windows. |
| Android arm64, armeabi-v7a and x86-64 APKs | All three extracted libmpv binaries contain the static ngtcp2/nghttp3 transport. The x86-64 binary imports Media NDK, OpenSL ES and the packaged OpenSSL libraries. | `mediacodec_embed` needs an Android Surface and a working hardware decoder. The Qt render API uses `mediacodec-copy` with software fallback. AudioTrack precedes OpenSL ES by default. AAudio is built and the default avoids it because of the existing seek-clock failure. | Artifact inspection on Linux. Phone and TV emulator tests exercise the x86-64 build in CI. The NVIDIA Shield still needs device testing for Surface, copy-back, profile and HDR behavior. |
| iOS and tvOS arm64 device IPAs | The app binaries contain the static ngtcp2/nghttp3 transport and import the Security framework. | Embedded `libmpv` uses `videotoolbox-copy` and AudioUnit with exclusive playback session settings. | Device-package inspection on Linux. The tvOS simulator audio test requires an advancing AudioUnit clock and checks the AVAudioSession category. Hardware decoding and HDR require a device. |
| webOS ARM IPK | The extracted libmpv depends on packaged curl. Curl's dynamic dependencies include OpenSSL, nghttp2, nghttp3 and ngtcp2. | The native path uses Starfish. Selected legacy video codecs use libmpv software decoding. The package links ALSA and PulseAudio. Starfish audio is available with the native video path. | Artifact inspection on Linux. Starfish, ALSA device routing, and decoder/profile support require a TV. |

For hardware decoders, codec registration establishes what may be tried. The negotiated `hwdec-current`, `current-vo`, `current-ao`, decoded pixel format and target transfer establish the path used for a particular file. The Linux release core registered H.264, HEVC, VP9, AV1, and MPEG-2 in its runtime decoder list. The checked-in FFmpeg manifest describes the platform-specific accelerators and software decoders in more detail. Profile, bit depth, HDR and audio passthrough need separate runtime samples on the target.

## Transport and cache results

The packaged Linux core has HTTP/3 and Alt-Svc support. It reports QUIC batching unavailable, which keeps the automatic H3 eligibility gate closed in this build. Explicit Prefer HTTP/3 negotiates QUIC. A stale same-port H3 advertisement with silently dropped UDP used HTTP/2 and retained all 16 MiB of decoded PCM. Reset tests exercised suffix retry and process-wide downgrade cooldowns. With a window-capped 140 ms HTTPS path, automatic mode escaped from HTTP/2 to HTTP/1.1 and reached four physical connections.

The Jellyfin 12.2.0 baseline covered authenticated Direct Play over loopback and a bandwidth/window shaper. First-frame and seek times are seconds. The first-frame marker here comes from libmpv playback progress through a null video output.

| RTT and actual bitrate | HTTPS profile | First frame | Forward seek | Backward seek | Rebuffer | Peak sockets |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 40 ms, 24.42 Mb/s | Auto | 1.444 | 0.234 | 0.201 | 0 | 8 |
| 40 ms, 24.42 Mb/s | Forced H2, one socket | 1.294 | 2.533 | 0.166 | 0 | 1 |
| 150 ms, about 48 Mb/s | Auto | 5.057 | 2.651 | 2.318 | 0 | 8 |
| 150 ms, about 48 Mb/s | Forced H2, one socket | 8.047 | 17.575 | 9.794 | 3.368 | 1 |

The controller sizes the demuxer cache from platform memory headroom. At the default setting, automatic growth caps forward and backward at 512 MiB and 128 MiB. The active scaler can shrink on the next sample, while growth after initialization needs a confirming sample. The manual forward floor is independent of the automatic ceiling. The Jellyfin fixture reads mpv's effective limits and also exercises the manually applied larger limits. It does not inject operating system memory pressure.

Instrumented repeats used the same release library. At 40 ms, adaptive HTTPS used 4.33 ms of curl CPU and 0.74 ms of consumer CPU per delivered MB. At 150 ms, the values were 2.72 and 0.26 ms/MB, versus 5.71 and 0.45 ms/MB for forced single-socket H2. The 150 ms adaptive sample delivered 117.6 MB, with 125.4 MB copied into the transport buffer, 117.6 MB copied out, 14,196 worker iterations and 18 coalesced wakeups. These are host measurements and the counter snapshots include work for seeks. They do not establish embedded CPU savings.

The isolated packet harness measured 150 ms RTT and targeted a 200 Mb/s path. For the 150 Mb/s fixture, H1 and H2 completed byte-exact capacity decodes below the 157.5 Mb/s acceptance floor. H3 negotiated with one physical UDP socket and made range progress. Its capacity runs stopped before continuous playback began. The run was inconclusive for continuous playback, so it gives no basis for a production transport change. An aria2 comparison needs an implementation that can meet the existing playback stream, seek, cancellation, authentication, TLS and memory contracts.

## Reproducing the Linux checks

Download `spool-linux` from the build above, extract the portable tarball, and point `LIBMPV` at its `usr/lib/libmpv.so`. Set `JELLYFIN` to a local Jellyfin 12.2 executable. Run from this checkout with the pinned mpv submodule initialized. The report directory should be outside the checkout.

```sh
nix develop .#native -c python3 tests/transport/protocols.py \
  --libmpv "$LIBMPV" --transport-helper mpv/test/curl_transport.py \
  --output /tmp/spool-protocols.json
nix develop .#native -c python3 tests/e2e/jellyfin_latency_e2e.py \
  --libmpv "$LIBMPV" --transport-helper mpv/test/curl_transport.py \
  --shaper-script tools/network-shaper.py --jellyfin-bin "$JELLYFIN" \
  --rtt-ms 150 --video-mbps 48 --output /tmp/spool-jellyfin-150-48.json
nix develop .#native -c tools/test-transport-netns.sh \
  --libmpv "$LIBMPV" --output /tmp/spool-packet-150.json
```
