#!/usr/bin/env python3
"""End-to-end Jellyfin + latency/window/slow-start proxy test and benchmark harness.

Starts an isolated real Jellyfin server with a freshly generated high-bitrate
DirectPlay media file, fronts it with tools/network-shaper.py (simulating WAN
RTT, TCP IW10 slow-start, and per-socket window ceilings) and optionally an
nginx TLS/HTTP2/HTTP3 reverse proxy, and exercises:
  1. Spool's real bundled Jellyfin provider (`spool-tests live-jellyfin`)
     through the shaped proxy.
  2. Real libmpv playback, mid-stream uncached forward seek, and behind-cache
     backward seek over HTTP and HTTPS (HTTP/1.1 vs HTTP/2 vs auto).

Exits 77 (CTest SKIP) when `jellyfin` is unavailable in PATH or `--jellyfin-bin`.
Never prints or serializes tokens, credentials, or authenticated URLs.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import json
import math
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


def free_port(host: str = "127.0.0.1") -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


def find_jellyfin_binary(explicit: str | None) -> str | None:
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    env_bin = os.environ.get("SPOOL_JELLYFIN_BIN")
    if env_bin and os.path.isfile(env_bin):
        return env_bin
    return shutil.which("jellyfin")


def generate_high_bitrate_movie(ffmpeg_bin: str, target_path: Path, *, duration_s: int, video_mbps: int) -> dict:
    started = time.monotonic()
    target_path.parent.mkdir(parents=True, exist_ok=True)
    # Use noisy testsrc2 with CBR-like x264 ultrafast so the file has genuine
    # high entropy (~video_mbps) and regular 1-second IDR keyframes for seeking.
    rate_arg = f"{video_mbps}M"
    buf_arg = f"{max(video_mbps // 2, 4)}M"
    subprocess.run(
        [
            ffmpeg_bin,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            f"testsrc2=size=1280x720:rate=30:duration={duration_s},noise=alls=45:allf=t",
            "-f",
            "lavfi",
            "-i",
            f"sine=frequency=440:sample_rate=48000:duration={duration_s}",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-tune",
            "zerolatency",
            "-b:v",
            rate_arg,
            "-minrate",
            rate_arg,
            "-maxrate",
            rate_arg,
            "-bufsize",
            buf_arg,
            "-g",
            "30",
            "-keyint_min",
            "30",
            "-c:a",
            "aac",
            "-b:a",
            "192k",
            str(target_path),
        ],
        check=True,
    )
    size_bytes = target_path.stat().st_size
    actual_bitrate_bps = round(size_bytes * 8 / duration_s)
    return {
        "duration_s": duration_s,
        "size_bytes": size_bytes,
        "actual_bitrate_bps": actual_bitrate_bps,
        "actual_mbps": round(actual_bitrate_bps / 1_000_000, 2),
        "generation_seconds": round(time.monotonic() - started, 3),
    }


class JellyfinTestServer:
    def __init__(self, root: Path, jellyfin_bin: str, media_dir: Path, *, username: str, password: str) -> None:
        self.root = root
        self.jellyfin_bin = jellyfin_bin
        self.media_dir = media_dir
        self.username = username
        self.password = password
        self.port = free_port()
        self.proc: subprocess.Popen | None = None
        self.token: str = ""
        self.user_id: str = ""
        self.item_id: str = ""
        self.media_source_id: str = ""
        self.media_size_bytes: int = 0
        self.media_bitrate_bps: int = 0
        self.server_version: str = ""

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def _request(self, method: str, path: str, body: dict | None = None, *, use_token: bool = False):
        auth = 'MediaBrowser Client="spool-e2e", Device="harness", DeviceId="spool-e2e-harness", Version="1.0"'
        if use_token and self.token:
            auth += f', Token="{self.token}"'
        headers = {"Authorization": auth}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base_url + path, data=data, headers=headers, method=method)
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read()
            ctype = resp.headers.get("Content-Type", "")
            payload = json.loads(raw) if raw and "json" in ctype else raw
            return resp.status, payload

    def start(self) -> dict:
        started = time.monotonic()
        config_dir = self.root / "jf-config"
        data_dir = self.root / "jf-data"
        cache_dir = self.root / "jf-cache"
        log_dir = self.root / "jf-log"
        for d in (config_dir, data_dir, cache_dir, log_dir):
            d.mkdir(parents=True, exist_ok=True)
        (config_dir / "network.xml").write_text(
            f"""<?xml version="1.0" encoding="utf-8"?>
<NetworkConfiguration xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">
  <InternalHttpPort>{self.port}</InternalHttpPort>
  <PublicHttpPort>{self.port}</PublicHttpPort>
  <EnableHttps>false</EnableHttps>
  <EnableUPnP>false</EnableUPnP>
  <AutoDiscovery>false</AutoDiscovery>
  <LocalNetworkAddresses><string>127.0.0.1</string></LocalNetworkAddresses>
</NetworkConfiguration>
"""
        )
        env = dict(os.environ)
        env["JELLYFIN_PublishedServerUrl"] = self.base_url
        env["no_proxy"] = "127.0.0.1,localhost"
        env["NO_PROXY"] = "127.0.0.1,localhost"
        self.log_path = self.root / "jellyfin-stderr.log"
        self._log_file = self.log_path.open("w+b")
        self.proc = subprocess.Popen(
            [
                self.jellyfin_bin,
                "--datadir",
                str(data_dir),
                "--configdir",
                str(config_dir),
                "--cachedir",
                str(cache_dir),
                "--logdir",
                str(log_dir),
                "--nowebclient",
                "--nonetchange",
                "--published-server-url",
                self.base_url,
            ],
            stdout=self._log_file,
            stderr=subprocess.STDOUT,
            env=env,
        )
        deadline = time.monotonic() + 60.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self._log_file.flush()
                stderr_tail = self.log_path.read_text(errors="replace")[-1500:]
                raise RuntimeError(f"Jellyfin exited prematurely (code {self.proc.returncode}): {stderr_tail}")
            try:
                status, info = self._request("GET", "/System/Info/Public")
                if status == 200 and isinstance(info, dict) and info.get("Version"):
                    self.server_version = str(info["Version"])
                    break
            except Exception:
                pass
            time.sleep(0.2)
        else:
            raise RuntimeError("Jellyfin failed to become ready within 60s")

        # Complete startup wizard
        self._request(
            "POST",
            "/Startup/Configuration",
            {"UICulture": "en-US", "MetadataCountryCode": "US", "PreferredMetadataLanguage": "en"},
        )
        self._request("GET", "/Startup/FirstUser")
        self._request("POST", "/Startup/User", {"Name": self.username, "Password": self.password})
        self._request(
            "POST",
            "/Startup/RemoteAccess",
            {"EnableRemoteAccess": True, "EnableAutomaticPortMapping": False},
        )
        self._request("POST", "/Startup/Complete", {})

        _, auth = self._request(
            "POST",
            "/Users/AuthenticateByName",
            {"Username": self.username, "Pw": self.password},
        )
        self.token = str(auth["AccessToken"])
        self.user_id = str(auth["User"]["Id"])

        self._request(
            "POST",
            "/Library/VirtualFolders?name=Movies&collectionType=movies&refreshLibrary=true",
            {
                "LibraryOptions": {
                    "PathInfos": [{"Path": str(self.media_dir)}],
                    "EnableRealtimeMonitor": False,
                    "EnableChapterImageExtraction": False,
                    "EnableTrickplayImageExtraction": False,
                    "DisabledSubtitleFetchers": ["Open Subtitles"],
                    "TypeOptions": [
                        {
                            "Type": "Movie",
                            "MetadataFetchers": [],
                            "ImageFetchers": [],
                        }
                    ],
                }
            },
            use_token=True,
        )

        scan_deadline = time.monotonic() + 60.0
        next_refresh = time.monotonic() + 10.0
        while time.monotonic() < scan_deadline:
            if time.monotonic() >= next_refresh:
                try:
                    self._request("POST", "/Library/Refresh", {}, use_token=True)
                except Exception:
                    pass
                next_refresh = time.monotonic() + 10.0
            _, items = self._request(
                "GET",
                f"/Users/{self.user_id}/Items?Recursive=true&IncludeItemTypes=Movie&Fields=MediaSources",
                use_token=True,
            )
            found = items.get("Items") or []
            if found:
                item = found[0]
                sources = item.get("MediaSources") or []
                if sources and sources[0].get("Size"):
                    ms = sources[0]
                    self.item_id = str(item["Id"])
                    self.media_source_id = str(ms["Id"])
                    self.media_size_bytes = int(ms["Size"])
                    if ms.get("Bitrate"):
                        self.media_bitrate_bps = int(ms["Bitrate"])
                        break
                    if time.monotonic() + 30.0 < scan_deadline:
                        time.sleep(0.25)
                        continue
                    break
            time.sleep(0.25)
        else:
            tail = self.log_path.read_text(errors="replace")[-1500:] if getattr(self, "log_path", None) and self.log_path.exists() else ""
            raise RuntimeError(f"Jellyfin failed to scan benchmark movie: {tail}")

        return {
            "version": self.server_version,
            "boot_and_scan_seconds": round(time.monotonic() - started, 3),
            "media_size_bytes": self.media_size_bytes,
            "media_bitrate_bps": self.media_bitrate_bps,
        }

    def direct_stream_path(self) -> str:
        return f"/Videos/{self.item_id}/stream?static=true&mediaSourceId={self.media_source_id}"

    def auth_header_value(self) -> str:
        return (
            'MediaBrowser Client="spool-e2e", Device="harness", '
            f'DeviceId="spool-e2e-harness", Version="1.0", Token="{self.token}"'
        )

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None
        if getattr(self, "_log_file", None) is not None:
            try:
                self._log_file.close()
            except Exception:
                pass
            self._log_file = None


class NetworkShaperProcess:
    def __init__(
        self,
        shaper_script: Path,
        *,
        upstream_port: int,
        rtt_ms: float,
        download_mbps: float = 0.0,
        per_connection_mbps: float = 0.0,
        initial_window_bytes: int = 0,
        max_window_bytes: int = 0,
        jitter_ms: float = 0.0,
        summary_file: Path | None = None,
    ) -> None:
        self.shaper_script = shaper_script
        self.upstream_port = upstream_port
        self.port = free_port()
        self.rtt_ms = rtt_ms
        self.download_mbps = download_mbps
        self.per_connection_mbps = per_connection_mbps
        self.initial_window_bytes = initial_window_bytes
        self.max_window_bytes = max_window_bytes
        self.jitter_ms = jitter_ms
        self.summary_file = summary_file
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        cmd = [
            sys.executable,
            str(self.shaper_script),
            "--listen",
            f"127.0.0.1:{self.port}",
            "--upstream",
            f"127.0.0.1:{self.upstream_port}",
            "--rtt-ms",
            str(self.rtt_ms),
            "--jitter-ms",
            str(self.jitter_ms),
            "--download-mbps",
            str(self.download_mbps),
            "--per-connection-mbps",
            str(self.per_connection_mbps),
            "--initial-window-bytes",
            str(self.initial_window_bytes),
            "--max-window-bytes",
            str(self.max_window_bytes),
            "--metrics-interval",
            "0",
        ]
        if self.summary_file is not None:
            cmd.extend(["--summary-file", str(self.summary_file)])
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert self.proc.stdout is not None
        line = self.proc.stdout.readline()
        if not line:
            err = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError(f"network-shaper failed to start: {err}")
        event = json.loads(line)
        if event.get("event") != "listening":
            raise RuntimeError(f"unexpected network-shaper startup event: {event}")

    def stop(self) -> dict:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None
        if self.summary_file is not None and self.summary_file.is_file():
            return json.loads(self.summary_file.read_text())
        return {}


class NginxTlsProxy:
    """Reverse-proxies Jellyfin over TLS with HTTP/1.1, HTTP/2, and HTTP/3 (QUIC)."""

    def __init__(self, root: Path, upstream_port: int) -> None:
        self.root = root / "nginx-tls"
        self.upstream_port = upstream_port
        self.port = free_port()
        self.cert_path = self.root / "cert.pem"
        self.key_path = self.root / "key.pem"
        self.access_log = self.root / "access.log"
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-keyout",
                str(self.key_path),
                "-out",
                str(self.cert_path),
                "-days",
                "1",
                "-subj",
                "/CN=127.0.0.1",
                "-addext",
                "subjectAltName=IP:127.0.0.1",
            ],
            check=True,
            capture_output=True,
        )
        conf_path = self.root / "nginx.conf"
        conf_path.write_text(
            f"""worker_processes 1;
pid {self.root}/nginx.pid;
error_log {self.root}/error.log warn;
events {{ worker_connections 256; }}
http {{
  client_body_temp_path {self.root}/client_body;
  proxy_temp_path {self.root}/proxy;
  fastcgi_temp_path {self.root}/fastcgi;
  uwsgi_temp_path {self.root}/uwsgi;
  scgi_temp_path {self.root}/scgi;
  log_format proto '$server_protocol $status $request_length $bytes_sent $http_range';
  access_log {self.access_log} proto;
  ssl_certificate {self.cert_path};
  ssl_certificate_key {self.key_path};
  ssl_protocols TLSv1.2 TLSv1.3;
  ssl_session_cache shared:SSL:2m;
  proxy_buffering off;
  proxy_http_version 1.1;
  proxy_set_header Connection "";
  server {{
    listen 127.0.0.1:{self.port} ssl;
    listen 127.0.0.1:{self.port} quic reuseport;
    http2 on;
    add_header Alt-Svc 'h3=":{self.port}"';
    location / {{
      proxy_pass http://127.0.0.1:{self.upstream_port};
      proxy_set_header Host $http_host;
    }}
  }}
}}
"""
        )
        self.proc = subprocess.Popen(
            ["nginx", "-e", "stderr", "-p", str(self.root), "-c", str(conf_path), "-g", "daemon off;"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                err = self.proc.stderr.read().decode(errors="replace") if self.proc.stderr else ""
                raise RuntimeError(f"nginx failed to start: {err}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("nginx did not become ready")

    def protocol_counts(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        if not self.access_log.is_file():
            return counts
        for line in self.access_log.read_text().splitlines():
            parts = line.split(" ", 2)
            if len(parts) >= 2 and parts[1] in ("200", "206"):
                counts[parts[0]] = counts.get(parts[0], 0) + 1
        return counts

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None


class PropertyEvent(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("format", ctypes.c_int), ("data", ctypes.c_void_p)]


def run_libmpv_scenario(
    *,
    helper_module,
    libmpv_path: str,
    url: str,
    origin: str,
    auth_header: str,
    media_size_bytes: int,
    media_duration_s: float,
    media_bitrate_bps: int,
    http_mode_name: str,
    http_mode_int: int,
    fixed_requests: int,
    max_requests: int,
    receive_mode: int,
    max_request_size: int,
    ca_file: Path | None = None,
    play_seconds: float = 6.0,
    forward_seek_target_s: float = 14.0,
    backward_seek_target_s: float = 11.0,
    demuxer_max_bytes: int = 32 << 20,
    demuxer_max_back_bytes: int = 16 << 20,
    buffer_size: int = 4 << 20,
    timeout_s: float = 55.0,
) -> dict:
    os.environ["no_proxy"] = "127.0.0.1,localhost"
    os.environ["NO_PROXY"] = "127.0.0.1,localhost"
    options = {
        "vo": "null",
        "ao": "null",
        "ao-null-untimed": "no",
        "video": "auto",
        "audio": "auto",
        "cache": "yes",
        "cache-pause": "yes",
        "cache-pause-initial": "yes",
        "demuxer-max-bytes": str(demuxer_max_bytes),
        "demuxer-max-back-bytes": str(demuxer_max_back_bytes),
        "demuxer-readahead-secs": "30",
        "curl-http-mode": http_mode_name,
        "curl-fixed-requests": str(fixed_requests),
        "curl-parallel-requests": str(max_requests),
        "curl-receive-mode": {0: "os-auto", 1: "measured", 2: "fixed"}[receive_mode],
        "curl-buffer-size": str(buffer_size),
        "curl-max-request-size": str(max_request_size),
        "http-header-fields": f"Authorization: {auth_header}",
        "http-proxy": "",
    }
    if ca_file is not None:
        options["tls-verify"] = "yes"
        options["tls-ca-file"] = str(ca_file)

    player = helper_module.Player(libmpv_path, **options)
    try:
        player.configure(
            1,
            max_requests=max_requests,
            fixed_requests=fixed_requests,
            http_mode=http_mode_int,
            receive_mode=receive_mode,
            direct_file=True,
            source_size_bytes=media_size_bytes,
            source_duration_seconds=media_duration_s,
            source_bitrate=media_bitrate_bps,
            primary_origin=origin,
        )
        player.check(
            player.transport_control(
                {
                    "format": 2,
                    "op": 1,
                    "playback-epoch": 1,
                    "joint-extra-grant-bytes": 64 << 20,
                    "background-network": False,
                }
            ),
            "grant",
        )

        observe = player.lib.mpv_observe_property
        observe.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int]
        observe.restype = ctypes.c_int
        player.check(observe(player.handle, 1, b"paused-for-cache", 3), "observe-cache")

        started = time.monotonic()
        player.command("loadfile", url)

        file_loaded_s: float | None = None
        first_frame_s: float | None = None
        steady_started_at: float | None = None
        paused_for_cache = False
        pause_started_at: float | None = None
        startup_buffering_s = 0.0
        rebuffer_events = 0
        rebuffer_seconds = 0.0
        phase = "startup"
        seek_started_at: float | None = None
        seek_restarted = False
        seek_baseline_pos: float | None = None
        forward_seek_s: float | None = None
        backward_seek_s: float | None = None
        phase_started_at: float | None = None
        peak_requests = 0
        peak_connections = 0
        observed_http_versions: set[int] = set()
        peak_range_bytes = 0
        last_metrics: dict = {}
        next_sample_at = 0.0
        playback_error: str | None = None

        while True:
            now = time.monotonic()
            if now - started > timeout_s:
                playback_error = f"timeout-in-phase-{phase}"
                break
            event = player.lib.mpv_wait_event(player.handle, 0.025).contents
            now = time.monotonic()
            elapsed = now - started

            if event.id == 8:  # MPV_EVENT_FILE_LOADED
                if file_loaded_s is None:
                    file_loaded_s = elapsed
            elif event.id == 21:  # MPV_EVENT_PLAYBACK_RESTART
                if phase in ("forward_seek", "backward_seek"):
                    seek_restarted = True
            elif event.id == 7:  # MPV_EVENT_END_FILE
                end_file = ctypes.cast(event.data, ctypes.POINTER(helper_module.EndFile)).contents
                if end_file.reason == 4:
                    playback_error = f"mpv-error-{end_file.error}"
                break
            elif event.id == 22 and event.userdata == 1:  # paused-for-cache
                prop = ctypes.cast(event.data, ctypes.POINTER(PropertyEvent)).contents
                if prop.format == 3 and prop.data:
                    new_paused = bool(ctypes.cast(prop.data, ctypes.POINTER(ctypes.c_int)).contents.value)
                    if new_paused != paused_for_cache:
                        if new_paused:
                            pause_started_at = now
                            if phase == "play":
                                rebuffer_events += 1
                        else:
                            if pause_started_at is not None:
                                dur = now - pause_started_at
                                if phase == "startup":
                                    startup_buffering_s += dur
                                elif phase == "play":
                                    rebuffer_seconds += dur
                                pause_started_at = None
                        paused_for_cache = new_paused

            pos = player.get("time-pos")
            if pos is not None and isinstance(pos, (int, float)) and math.isfinite(pos):
                if first_frame_s is None and pos > 0.05:
                    first_frame_s = elapsed
                if phase == "startup" and file_loaded_s is not None and pos > 0.25 and not paused_for_cache:
                    phase = "play"
                    steady_started_at = now
                elif phase == "play" and steady_started_at is not None and (now - steady_started_at) >= play_seconds:
                    phase = "forward_seek"
                    seek_started_at = now
                    seek_restarted = False
                    seek_baseline_pos = None
                    player.command("seek", str(forward_seek_target_s), "absolute")
                elif phase == "forward_seek" and seek_started_at is not None:
                    if seek_restarted and not paused_for_cache and pos >= forward_seek_target_s - 1.2:
                        if seek_baseline_pos is None:
                            seek_baseline_pos = pos
                        elif pos >= seek_baseline_pos + 0.05:
                            forward_seek_s = now - seek_started_at
                            phase = "post_forward_play"
                            phase_started_at = now
                elif (
                    phase == "post_forward_play"
                    and phase_started_at is not None
                    and (now - phase_started_at) >= 1.5
                ):
                    phase = "backward_seek"
                    seek_started_at = now
                    seek_restarted = False
                    seek_baseline_pos = None
                    player.command("seek", str(backward_seek_target_s), "absolute")
                elif phase == "backward_seek" and seek_started_at is not None:
                    if seek_restarted and not paused_for_cache and abs(pos - backward_seek_target_s) <= 1.5:
                        if seek_baseline_pos is None:
                            seek_baseline_pos = pos
                        elif pos >= seek_baseline_pos + 0.05:
                            backward_seek_s = now - seek_started_at
                            phase = "done"
                            break

            if now >= next_sample_at:
                next_sample_at = now + 0.2
                metrics = player.get("curl-transport-metrics") or {}
                if metrics:
                    last_metrics = metrics
                    peak_requests = max(peak_requests, int(metrics.get("peak-active-requests", 0)))
                    peak_connections = max(peak_connections, int(metrics.get("physical-connections", 0)))
                    peak_range_bytes = max(peak_range_bytes, int(metrics.get("range-bytes", 0)))
                    for row in metrics.get("connections", []):
                        ver = int(row.get("http-version", 0))
                        if ver > 0:
                            observed_http_versions.add(ver)
                    for row in metrics.get("streams", []):
                        ver = int(row.get("effective-http-version", 0))
                        if ver > 0:
                            observed_http_versions.add(ver)
                cache_dur = player.get("demuxer-cache-duration") or 0.0
                player.transport_control(
                    {
                        "format": 2,
                        "op": 2,
                        "playback-epoch": 1,
                        "playback-buffer-seconds": float(cache_dur),
                        "playback-demand-bits-per-second": float(media_bitrate_bps),
                        "playback-user-paused": False,
                    }
                )

        if pause_started_at is not None:
            dur = time.monotonic() - pause_started_at
            if phase == "startup":
                startup_buffering_s += dur
            elif phase == "play":
                rebuffer_seconds += dur

        demux_max = player.get("demuxer-max-bytes")
        demux_back = player.get("demuxer-max-back-bytes")
        # Keep only cumulative cost counters from the last live snapshot.
        # An empty or unsupported counter is distinct from a measured zero.
        transport_cost = {key: last_metrics[key] for key in (
            "consumer-delivered-bytes", "copy-in-bytes", "copy-out-bytes", "reorder-copy-bytes",
            "body-callbacks", "worker-iterations", "coalesced-wakeups",
        ) if key in last_metrics}
        delivered_mb = transport_cost.get("consumer-delivered-bytes", 0) / 1_000_000
        for thread in ("curl", "consumer"):
            if last_metrics.get(f"{thread}-thread-cpu-available"):
                cpu_seconds = last_metrics[f"{thread}-thread-cpu-seconds"]
                transport_cost[f"{thread}-thread-cpu-seconds"] = cpu_seconds
                if delivered_mb > 0:
                    transport_cost[f"{thread}-thread-cpu-ms-per-delivered-mb"] = round(cpu_seconds * 1000 / delivered_mb, 3)
        return {
            "completed": phase == "done" and playback_error is None,
            "error": playback_error,
            "phase_reached": phase,
            "file_loaded_s": round(file_loaded_s, 3) if file_loaded_s is not None else None,
            "first_frame_s": round(first_frame_s, 3) if first_frame_s is not None else None,
            "startup_buffering_s": round(startup_buffering_s, 3),
            "rebuffer_events": rebuffer_events,
            "rebuffer_seconds": round(rebuffer_seconds, 3),
            "forward_seek_s": round(forward_seek_s, 3) if forward_seek_s is not None else None,
            "backward_seek_s": round(backward_seek_s, 3) if backward_seek_s is not None else None,
            "peak_active_requests": peak_requests,
            "peak_physical_connections": peak_connections,
            "peak_range_bytes": peak_range_bytes,
            "observed_http_versions": sorted(observed_http_versions),
            "selected_requests": int(last_metrics.get("selected-requests", 0)),
            "capacity_mbps": round(float(last_metrics.get("capacity-bytes-per-second", 0.0)) * 8 / 1_000_000, 2),
            "avg_received_mbps": round(
                float(last_metrics.get("average-received-bytes-per-second", 0.0)) * 8 / 1_000_000, 2
            ),
            "ttfb_ms": round(float(last_metrics.get("ttfb-ms", 0.0)), 1),
            "received_bytes": int(last_metrics.get("received-bytes", 0)),
            "transport_cost": transport_cost,
            "failed_requests": int(last_metrics.get("failed-requests", 0)),
            "demuxer_max_bytes": demux_max,
            "demuxer_max_back_bytes": demux_back,
        }
    finally:
        player.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libmpv", required=True, help="Path to libmpv.so")
    parser.add_argument("--transport-helper", type=Path, required=True, help="Path to mpv/test/curl_transport.py")
    parser.add_argument("--shaper-script", type=Path, required=True, help="Path to tools/network-shaper.py")
    parser.add_argument("--jellyfin-bin", default=None, help="Path to jellyfin executable")
    parser.add_argument("--ffmpeg-bin", default=None, help="Path to ffmpeg executable")
    parser.add_argument("--spool-tests", default=None, help="Optional path to spool-tests binary for live-jellyfin")
    parser.add_argument("--duration-s", type=int, default=20, help="Generated movie duration in seconds")
    parser.add_argument("--video-mbps", type=int, default=48, help="Generated movie bitrate in Mbps")
    parser.add_argument("--rtt-ms", type=float, default=150.0, help="Simulated WAN round-trip latency in ms")
    parser.add_argument("--initial-window-bytes", type=int, default=14600, help="Simulated TCP IW in bytes")
    parser.add_argument("--max-window-bytes", type=int, default=262144, help="Simulated per-socket max window in bytes")
    parser.add_argument("--download-mbps", type=float, default=200.0, help="Aggregate WAN download ceiling in Mbps")
    parser.add_argument("--max-request-size", type=int, default=1 << 20, help="curl-max-request-size in bytes")
    parser.add_argument("--output", type=Path, default=None, help="Write benchmark JSON report to this path")
    parser.add_argument("--benchmark-only", action="store_true", help="Record metrics without failing performance thresholds")
    args = parser.parse_args()

    jellyfin_bin = find_jellyfin_binary(args.jellyfin_bin)
    if not jellyfin_bin:
        print("SKIP: jellyfin binary not found (set --jellyfin-bin or SPOOL_JELLYFIN_BIN)", file=sys.stderr)
        return 77

    ffmpeg_bin = args.ffmpeg_bin or shutil.which("ffmpeg")
    if not ffmpeg_bin:
        print("SKIP: ffmpeg binary not found", file=sys.stderr)
        return 77

    spec = importlib.util.spec_from_file_location("curl_transport", args.transport_helper)
    assert spec and spec.loader
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)

    report: dict = {
        "config": {
            "duration_s": args.duration_s,
            "video_mbps": args.video_mbps,
            "rtt_ms": args.rtt_ms,
            "initial_window_bytes": args.initial_window_bytes,
            "max_window_bytes": args.max_window_bytes,
            "download_mbps": args.download_mbps,
            "max_request_size": args.max_request_size,
        },
        "scenarios": {},
    }

    with tempfile.TemporaryDirectory(prefix="spool-jellyfin-e2e-") as tmp:
        root = Path(tmp)
        media_dir = root / "movies"
        movie_path = media_dir / "LatencyBenchmark (2026).mkv"
        media_info = generate_high_bitrate_movie(
            ffmpeg_bin, movie_path, duration_s=args.duration_s, video_mbps=args.video_mbps
        )
        report["media"] = media_info

        jf = JellyfinTestServer(root, jellyfin_bin, media_dir, username="spooltest", password="spooltestpass")
        try:
            report["jellyfin"] = jf.start()
            effective_bitrate = jf.media_bitrate_bps or media_info["actual_bitrate_bps"]

            # 1. Optional live-jellyfin C++ provider E2E through a 60ms RTT proxy
            if args.spool_tests and os.path.isfile(args.spool_tests):
                live_summary = root / "live-proxy-summary.json"
                live_proxy = NetworkShaperProcess(
                    args.shaper_script,
                    upstream_port=jf.port,
                    rtt_ms=60.0,
                    download_mbps=200.0,
                    summary_file=live_summary,
                )
                live_proxy.start()
                try:
                    t0 = time.monotonic()
                    env = dict(os.environ)
                    env["SPOOL_LIVE_JELLYFIN"] = f"http://127.0.0.1:{live_proxy.port}"
                    env["SPOOL_LIVE_USER"] = jf.username
                    env["SPOOL_LIVE_PASSWORD"] = jf.password
                    env["no_proxy"] = "127.0.0.1,localhost"
                    env["NO_PROXY"] = "127.0.0.1,localhost"
                    res = subprocess.run(
                        [args.spool_tests, "live-jellyfin"],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=90,
                    )
                    report["scenarios"]["live_jellyfin_provider"] = {
                        "passed": res.returncode == 0,
                        "returncode": res.returncode,
                        "seconds": round(time.monotonic() - t0, 3),
                        "proxy": live_proxy.stop(),
                    }
                    if res.returncode != 0:
                        print("live-jellyfin failed:\n" + res.stdout + "\n" + res.stderr, file=sys.stderr)
                finally:
                    live_proxy.stop()

            # 2. HTTP over WAN proxy: single-connection control vs auto adaptive
            for name, fixed_req in [("http_single_lane_control", 1), ("http_auto_adaptive", 0)]:
                summary_path = root / f"{name}-proxy.json"
                proxy = NetworkShaperProcess(
                    args.shaper_script,
                    upstream_port=jf.port,
                    rtt_ms=args.rtt_ms,
                    download_mbps=args.download_mbps,
                    initial_window_bytes=args.initial_window_bytes,
                    max_window_bytes=args.max_window_bytes,
                    summary_file=summary_path,
                )
                proxy.start()
                try:
                    stream_url = f"http://127.0.0.1:{proxy.port}{jf.direct_stream_path()}"
                    result = run_libmpv_scenario(
                        helper_module=helper,
                        libmpv_path=args.libmpv,
                        url=stream_url,
                        origin=f"http://127.0.0.1:{proxy.port}",
                        auth_header=jf.auth_header_value(),
                        media_size_bytes=jf.media_size_bytes,
                        media_duration_s=float(args.duration_s),
                        media_bitrate_bps=effective_bitrate,
                        http_mode_name="auto",
                        http_mode_int=0,
                        fixed_requests=fixed_req,
                        max_requests=8,
                        receive_mode=1,
                        max_request_size=args.max_request_size,
                    )
                finally:
                    proxy_stats = proxy.stop()
                result["proxy"] = proxy_stats
                report["scenarios"][name] = result

            # 3. HTTPS (nginx HTTP/1.1 + HTTP/2 + HTTP/3 Alt-Svc) over WAN TCP slow-start proxy:
            #    Compare forced HTTP/2 (`tcp`) vs production default (`auto`)
            if shutil.which("nginx") and shutil.which("openssl"):
                nginx = NginxTlsProxy(root, jf.port)
                nginx.start()
                try:
                    for name, mode_name, mode_int, extra_kwargs in [
                        ("https_forced_h2_single_socket", "tcp", 1, {}),
                        ("https_auto_adaptive", "auto", 0, {}),
                        (
                            "https_production_scaled_cache",
                            "auto",
                            0,
                            {
                                "buffer_size": 8 << 20,
                                "max_request_size": 4 << 20,
                                "demuxer_max_bytes": 512 << 20,
                                "demuxer_max_back_bytes": 128 << 20,
                            },
                        ),
                    ]:
                        summary_path = root / f"{name}-proxy.json"
                        proxy = NetworkShaperProcess(
                            args.shaper_script,
                            upstream_port=nginx.port,
                            rtt_ms=args.rtt_ms,
                            download_mbps=args.download_mbps,
                            initial_window_bytes=args.initial_window_bytes,
                            max_window_bytes=args.max_window_bytes,
                            summary_file=summary_path,
                        )
                        proxy.start()
                        try:
                            stream_url = f"https://127.0.0.1:{proxy.port}{jf.direct_stream_path()}"
                            kwargs = {
                                "max_request_size": args.max_request_size,
                                **extra_kwargs,
                            }
                            result = run_libmpv_scenario(
                                helper_module=helper,
                                libmpv_path=args.libmpv,
                                url=stream_url,
                                origin=f"https://127.0.0.1:{proxy.port}",
                                auth_header=jf.auth_header_value(),
                                media_size_bytes=jf.media_size_bytes,
                                media_duration_s=float(args.duration_s),
                                media_bitrate_bps=effective_bitrate,
                                http_mode_name=mode_name,
                                http_mode_int=mode_int,
                                fixed_requests=0,
                                max_requests=8,
                                receive_mode=1,
                                ca_file=nginx.cert_path,
                                **kwargs,
                            )
                        finally:
                            proxy_stats = proxy.stop()
                        result["proxy"] = proxy_stats
                        result["nginx_protocols"] = nginx.protocol_counts()
                        report["scenarios"][name] = result
                finally:
                    nginx.stop()
        finally:
            jf.stop()

    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n")
    print(rendered, flush=True)

    if args.benchmark_only:
        return 0

    failures: list[str] = []
    live = report["scenarios"].get("live_jellyfin_provider")
    if live is not None and not live.get("passed"):
        failures.append("live_jellyfin_provider failed")

    http_auto = report["scenarios"].get("http_auto_adaptive", {})
    if not http_auto.get("completed"):
        failures.append(f"http_auto_adaptive did not complete: {http_auto.get('error')}")
    elif http_auto.get("rebuffer_seconds", 99.0) > 1.0:
        failures.append(f"http_auto_adaptive rebuffered too long: {http_auto.get('rebuffer_seconds')}s")
    if http_auto.get("peak_physical_connections", 0) < 2:
        failures.append("http_auto_adaptive failed to scale physical connections past single-socket ceiling")

    https_auto = report["scenarios"].get("https_auto_adaptive")
    if https_auto is not None:
        if not https_auto.get("completed"):
            failures.append(f"https_auto_adaptive did not complete: {https_auto.get('error')}")
        elif https_auto.get("rebuffer_seconds", 99.0) > 1.5:
            failures.append(f"https_auto_adaptive rebuffered too long: {https_auto.get('rebuffer_seconds')}s")
        if https_auto.get("peak_physical_connections", 0) < 2 and 30 not in https_auto.get("observed_http_versions", []):
            failures.append("https_auto_adaptive stayed trapped on a single TCP socket")

    if failures:
        for msg in failures:
            print("FAIL:", msg, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
