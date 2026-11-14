#!/usr/bin/env python3
"""150 Mbps playback acceptance at the real libmpv/TLS transport boundary.

Run inside the wrapper's private, shaped network namespace. This program never
changes networking. A local decode and a timed CPU prerequisite precede network
measurements. Capacity, exact decoded output, and timed playback are separate
claims; insufficient capacity is inconclusive and exits nonzero.
"""

import argparse
import contextlib
import ctypes
import hashlib
import http.client
import importlib.util
import json
import math
import multiprocessing
import os
from pathlib import Path
import resource
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time


RATE = 195312
CHANNELS = 32
ALIGN = 96
DURATION = 60
BYTE_RATE = RATE * ALIGN
PAYLOAD_BYTES = BYTE_RATE * DURATION
FILE_BYTES = PAYLOAD_BYTES + 68
PCM_BYTES = 48000 * DURATION * 2 * 2
MODES = {"http1": (2, 11), "tcp": (1, 20), "prefer-h3": (3, 30)}
FILTER = "lavfi=[pan=stereo|c0=c0|c1=c1,aresample=48000]"

# Only these public numeric metrics enter an artifact. Never serialize a raw
# transport node, exception message, nginx diagnostic, URL, or request header.
METRIC_KEYS = """
playback-epoch stale-controls rejected-controls primary-origin-match
active-requests peak-active-requests selected-requests requests-per-stream
range-bytes ring-bytes received-bytes ordered-available-bytes
consumer-delivered-bytes readable-bytes retained-bytes reorder-bytes
started-requests finished-requests failed-requests retry-attempts
capacity-bytes-per-second capacity-samples ttfb-ms
average-received-bytes-per-second current-received-bytes-per-second
curl-thread-cpu-seconds curl-thread-wall-seconds curl-thread-cpu-available
consumer-thread-cpu-seconds consumer-thread-cpu-available
consumer-wait-seconds head-of-line-seconds reorder-wait-seconds oldest-gap-seconds
perform-wall-seconds dispatch-wall-seconds poll-wall-seconds ring-wait-seconds
recent-scheduling-lateness-ms peak-scheduling-lateness-ms
scheduling-lateness-p50-ms scheduling-lateness-p99-ms scheduling-lateness-available
playback-buffer-seconds playback-demand-bits-per-second declared-bits-per-second
playback-state-updates playback-user-paused physical-connections
allocated-raw-bytes committed-joint-extra-grant-bytes snapshot-truncated
""".split()
TABLE_KEYS = {
    "streams": "stream-id effective-http-version http-fallbacks state last-reason urgency",
    "connections": "http-version transport active-requests receive-requested-bytes "
                   "receive-effective-bytes receive-status tcp-rtt-ms tcp-rtt-available "
                   "udp-rcvbuf-requested udp-rcvbuf-effective udp-drops udp-drops-available",
    "transport-samples": "requests http-version range-bytes ordered-bytes-per-second "
                         "ttfb-ms samples source",
    "protocol-samples": "http-version ordered-available-bytes cpu-ns-per-ordered-byte "
                        "cpu-ns-per-ordered-byte-available",
    "concurrency-samples": "requests capacity-bytes-per-second ttfb-ms samples",
}


class AcceptanceFailure(Exception):
    """The message is a fixed, public failure code, never a library diagnostic."""


def require(condition, code):
    if not condition:
        raise AcceptanceFailure(code)


def numeric_fields(node, keys):
    return {key: node[key] for key in keys if key in node
            and isinstance(node[key], (int, float)) and math.isfinite(node[key])}


def clean_metrics(node):
    node = node or {}
    result = numeric_fields(node, METRIC_KEYS)
    for table, keys in TABLE_KEYS.items():
        result[table] = [numeric_fields(row, keys.split())
                         for row in node.get(table, [])[:32]]
    return result


def generate_wave(path):
    """Write <1 MiB at a time; channel counters do not repeat within the file."""
    pcm_guid = struct.pack("<IHH8s", 1, 0, 0x10, b"\x80\x00\x00\xaa\x00\x38\x9b\x71")
    fmt = struct.pack("<HHIIHHHHI", 0xFFFE, CHANNELS, RATE, BYTE_RATE,
                      ALIGN, 24, 22, 24, 0) + pcm_guid
    header = (b"RIFF" + struct.pack("<I", FILE_BYTES - 8) + b"WAVEfmt "
              + struct.pack("<I", len(fmt)) + fmt
              + b"data" + struct.pack("<I", PAYLOAD_BYTES))
    require(len(header) == 68, "invalid-wave-header")
    started = time.monotonic()
    with path.open("wb") as target:
        target.write(header)
        for start in range(0, RATE * DURATION, 8192):
            frames = min(8192, RATE * DURATION - start)
            block = bytearray(frames * ALIGN)
            for index in range(frames):
                # Odd multipliers permute the 24-bit counter space; distinct
                # channels and energetic high bits survive S16 resampling.
                left = ((start + index) * 1664525 + 1013904223) & 0xFFFFFF
                right = ((start + index) * 22695477 + 0x534F4C) & 0xFFFFFF
                struct.pack_into("<IH", block, index * ALIGN,
                                 left | ((right & 255) << 24), right >> 8)
            target.write(block)
    require(path.stat().st_size == FILE_BYTES, "invalid-wave-size")
    return {"seconds": time.monotonic() - started, "header-bytes": 68,
            "payload-bytes": PAYLOAD_BYTES, "file-bytes": FILE_BYTES,
            "sample-rate": RATE, "channels": CHANNELS, "bits-per-sample": 24,
            "channel-mask": 0, "block-align": ALIGN, "bytes-per-second": BYTE_RATE,
            "bits-per-second": BYTE_RATE * 8, "duration-seconds": DURATION,
            "generation-block-bytes-max": 8192 * ALIGN}


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class PropertyEvent(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("format", ctypes.c_int),
                ("data", ctypes.c_void_p)]


def playback_worker(pipe, settings):
    """Own libmpv in a killable process, including initialize and destroy."""
    # libcurl can inherit proxy settings even with mpv's empty proxy option.
    # Keep every fixture request on loopback without inspecting those settings.
    os.environ["no_proxy"] = "127.0.0.1"
    os.environ["NO_PROXY"] = "127.0.0.1"
    # terminal=no is also set by Player. Keep unexpected native diagnostics
    # private rather than allowing a future dependency to print request data.
    with open(os.devnull, "wb") as sink:
        os.dup2(sink.fileno(), 1)
        os.dup2(sink.fileno(), 2)
    player = None
    result = {"status": "failed", "samples": [], "cache-events": [],
              "eof": False, "max-output-position-seconds": 0.0,
              "max-audio-pts-seconds": 0.0}
    started = time.monotonic()
    cpu_started = time.process_time()
    try:
        spec = importlib.util.spec_from_file_location("high_bitrate_transport", settings["helper"])
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        timed = settings["timed"]
        options = {"af": FILTER, "audio-format": "s16", "audio-samplerate": "48000",
                   "ao": "null" if timed else "pcm", "ao-null-untimed": "no",
                   "curl-max-request-size": str(16 << 20), "tls-verify": "yes",
                   "http-proxy": "", "cache-pause-initial": "yes",
                   "demuxer-max-bytes": str(32 << 20), "demuxer-readahead-secs": "2"}
        if not timed:
            options.update({"ao-pcm-file": settings["pcm"], "ao-pcm-waveheader": "no"})
        if settings.get("mode"):
            mode = settings["mode"]
            count = settings["count"]
            options.update({"tls-ca-file": settings["ca"], "curl-http-mode": mode,
                            "curl-fixed-requests": str(count), "curl-receive-mode": "measured"})
        player = helper.Player(settings["library"], **options)
        if settings.get("mode"):
            caps = player.get("curl-transport-capabilities") or {}
            result["capabilities"] = numeric_fields(
                caps, ["http2", "http3", "alt-svc", "quic-batching", "hw-aes", "curl-version"])
            require(caps.get("http2") and caps.get("http3"), "missing-http2-or-http3")
            epoch = max(settings["epoch"],
                        (player.get("curl-transport-metrics") or {}).get("playback-epoch", 0) + 1)
            player.configure(epoch, max_requests=4, fixed_requests=count,
                             http_mode=MODES[mode][0], receive_mode=1,
                             direct_file=True, source_size_bytes=FILE_BYTES,
                             source_duration_seconds=DURATION, source_bitrate=BYTE_RATE * 8,
                             primary_origin=settings["origin"])
            player.check(player.transport_control({"format": 2, "op": 1,
                         "playback-epoch": epoch, "joint-extra-grant-bytes": 64 << 20,
                         "background-network": False}), "grant")
            result["playback-epoch"] = epoch
        observe = player.lib.mpv_observe_property
        observe.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int]
        observe.restype = ctypes.c_int
        player.check(observe(player.handle, 1, b"paused-for-cache", 3), "observe-cache")
        player.command("loadfile", settings["source"])
        loaded = False
        first = None
        cache_start = None
        pause_seconds = 0.0
        last_advance = time.monotonic()
        last_consumed = 0.0
        next_sample = 0.0
        previous_sample = None
        while True:
            event = player.lib.mpv_wait_event(player.handle, 0.05).contents
            now = time.monotonic()
            elapsed = now - started
            if event.id == 8:
                loaded = True
                result["file-loaded-seconds"] = elapsed
                result["media-duration-seconds"] = player.get("duration")
            if event.id == 22 and event.userdata == 1:
                prop = ctypes.cast(event.data, ctypes.POINTER(PropertyEvent)).contents
                if prop.format == 3 and prop.data:
                    paused = bool(ctypes.cast(prop.data, ctypes.POINTER(ctypes.c_int)).contents.value)
                    result["cache-events"].append({"seconds": elapsed, "paused": paused,
                                                   "post-startup": first is not None})
                    if first is not None and paused and cache_start is None:
                        cache_start = elapsed
                    if not paused and cache_start is not None:
                        pause_seconds += elapsed - cache_start
                        cache_start = None
            position = player.get("time-pos")
            audio_pts = player.get("audio-pts")
            if isinstance(audio_pts, (int, float)) and math.isfinite(audio_pts):
                result["max-audio-pts-seconds"] = max(result["max-audio-pts-seconds"], audio_pts)
            if isinstance(position, (int, float)) and math.isfinite(position):
                result["max-output-position-seconds"] = max(
                    result["max-output-position-seconds"], position)
                if first is None and position > .05:
                    first = elapsed
                    result["first-progress-seconds"] = first
            # ao=pcm is untimed: its playback clock can lag submitted audio.
            # Detect a stalled decode using actual PCM output, not that clock.
            consumed = result["max-output-position-seconds"]
            if not timed:
                pcm = Path(settings["pcm"])
                consumed = pcm.stat().st_size / (48000 * 4) if pcm.exists() else 0
            if consumed > last_consumed + .01:
                last_advance = now
                last_consumed = consumed
            if elapsed >= next_sample:
                metrics = clean_metrics(player.get("curl-transport-metrics"))
                sample = {"seconds": elapsed, "position-seconds": position,
                          "audio-pts-seconds": audio_pts, "metrics": metrics}
                if previous_sample:
                    seconds = elapsed - previous_sample["seconds"]
                    for key in ("ordered-available-bytes", "consumer-delivered-bytes"):
                        if key in metrics and key in previous_sample["metrics"]:
                            sample[key + "-per-second"] = max(
                                0, metrics[key] - previous_sample["metrics"][key]) / seconds
                result["samples"].append(sample)
                previous_sample = sample
                next_sample = elapsed + 1
                # Parent persists progress if a native call subsequently hangs.
                pipe.send(("progress", result))
            if event.id == 7:
                end = ctypes.cast(event.data, ctypes.POINTER(helper.EndFile)).contents
                result.update({"end-reason": end.reason, "end-error": end.error})
                require(loaded and end.reason == 0, "playback-ended-without-eof")
                result["eof"] = True
                result["status"] = "complete"
                break
            if settings.get("stop-at") and result["max-output-position-seconds"] >= settings["stop-at"]:
                result["status"] = "complete"
                break
            if elapsed >= settings["deadline"]:
                result.update(status="inconclusive", failure="playback-deadline")
                break
            if settings.get("mode") and (now - last_advance >= 25
                    or (elapsed >= 45 and consumed < 10)):
                result.update(status="inconclusive", failure="insufficient-capacity-or-stall")
                break
        if cache_start is not None:
            pause_seconds += time.monotonic() - started - cache_start
        result["post-startup-cache-pause-seconds"] = pause_seconds
        result["post-startup-cache-pause-count"] = sum(
            row["paused"] and row["post-startup"] for row in result["cache-events"])
        result["final-metrics"] = clean_metrics(player.get("curl-transport-metrics"))
    except AcceptanceFailure as error:
        result["failure"] = str(error)
    except BaseException as error:
        result["failure"] = "libmpv-or-worker-error"
        result["error-type"] = type(error).__name__
    finally:
        result["seconds"] = time.monotonic() - started
        result["process-cpu-seconds"] = time.process_time() - cpu_started
        result["process-peak-rss-kib"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        # Send before destruction: a hanging native destructor cannot lose data.
        pipe.send(("result", result))
        if player:
            player.close()
        pipe.close()


def run_player(settings):
    context = multiprocessing.get_context("spawn")
    reader, writer = context.Pipe(duplex=False)
    process = context.Process(target=playback_worker, args=(writer, settings))
    process.start()
    writer.close()
    end = time.monotonic() + settings["deadline"] + 10
    result = {"status": "failed", "failure": "worker-exited", "samples": []}
    complete = False
    try:
        while time.monotonic() < end:
            if reader.poll(.2):
                try:
                    kind, result = reader.recv()
                except EOFError:
                    break
                if kind == "result":
                    complete = True
                    break
            elif not process.is_alive():
                break
        if not complete:
            result.update(status="inconclusive", failure="worker-deadline-or-exit")
        process.join(timeout=3 if complete else 0)
    finally:
        if process.is_alive():
            process.terminate()
            process.join(timeout=2)
        if process.is_alive():
            process.kill()
            process.join(timeout=2)
        reader.close()
    return result


def free_ports():
    # Hold TCP and UDP reservations until all three distinct ports are chosen.
    with contextlib.ExitStack() as stack:
        ports = []
        while len(ports) < len(MODES):
            tcp = stack.enter_context(socket.socket())
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            udp = stack.enter_context(socket.socket(type=socket.SOCK_DGRAM))
            try:
                udp.bind(("127.0.0.1", port))
            except OSError:
                continue
            ports.append(port)
        return dict(zip(MODES, ports))


@contextlib.contextmanager
def nginx_fixture(root):
    require(all(shutil.which(tool) for tool in ("nginx", "openssl")), "missing-nginx-or-openssl")
    try:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem"),
                        "-days", "1", "-subj", "/CN=127.0.0.1", "-addext",
                        "subjectAltName=IP:127.0.0.1"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
    except (OSError, subprocess.SubprocessError):
        raise AcceptanceFailure("certificate-generation-failed") from None
    ports = free_ports()

    def quoted(path):
        return '"' + str(path).replace("\\", "\\\\").replace('"', '\\"') + '"'

    servers = []
    for mode, port in ports.items():
        servers.append(f"""server {{
  listen 127.0.0.1:{port} ssl;
  {'listen 127.0.0.1:' + str(port) + ' quic reuseport;' if mode == 'prefer-h3' else ''}
  {'http2 on;' if mode != 'http1' else ''}
  access_log {quoted(root / (mode + '.access.log'))} measurement;
  add_header Alt-Svc 'h3=":{port}"';
  root {quoted(root)};
}}""")
    # In the private mapped-root namespace the worker must retain uid 0 to
    # traverse the private fixture directory. Ordinary reference use needs no user.
    config = ("user root root;\n" if os.geteuid() == 0 else "") + f"""
worker_processes 1;
pid {quoted(root / 'nginx.pid')};
error_log {quoted(root / 'error.log')} warn;
events {{ worker_connections 256; }}
http {{
  client_body_temp_path {quoted(root / 'client-body')};
  proxy_temp_path {quoted(root / 'proxy')};
  fastcgi_temp_path {quoted(root / 'fastcgi')};
  uwsgi_temp_path {quoted(root / 'uwsgi')};
  scgi_temp_path {quoted(root / 'scgi')};
  log_format measurement '$server_protocol $status';
  ssl_certificate {quoted(root / 'cert.pem')};
  ssl_certificate_key {quoted(root / 'key.pem')};
  ssl_protocols TLSv1.2 TLSv1.3;
  sendfile on;
  {''.join(servers)}
}}
"""
    (root / "nginx.conf").write_text(config)
    with (root / "nginx.stderr").open("wb") as errors:
        server = subprocess.Popen(["nginx", "-e", "stderr", "-p", str(root),
                                   "-c", str(root / "nginx.conf"), "-g", "daemon off;"],
                                  stdout=subprocess.DEVNULL, stderr=errors,
                                  start_new_session=True)
        try:
            deadline = time.monotonic() + 20
            context = ssl.create_default_context(cafile=str(root / "cert.pem"))
            for port in ports.values():
                while True:
                    require(server.poll() is None, "nginx-startup-failed")
                    remaining = deadline - time.monotonic()
                    require(remaining > 0, "nginx-readiness-deadline")
                    connection = http.client.HTTPSConnection(
                        "127.0.0.1", port, context=context, timeout=max(1, min(3, remaining)))
                    try:
                        connection.request("HEAD", "/fixture.wav")
                        response = connection.getresponse()
                        require(response.status == 200 and
                                int(response.getheader("Content-Length", "0")) == FILE_BYTES,
                                "nginx-fixture-metadata-mismatch")
                        break
                    except (OSError, http.client.HTTPException):
                        time.sleep(.1)
                    finally:
                        connection.close()
            yield ports
        finally:
            # Own the worker processes as well as nginx's master, even when a
            # native server shutdown hangs or the master exits first.
            with contextlib.suppress(ProcessLookupError):
                os.killpg(server.pid, signal.SIGTERM)
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(server.pid, signal.SIGKILL)
                server.wait(timeout=3)
            finally:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(server.pid, signal.SIGKILL)


def protocol_evidence(result, mode, root, log_offset):
    versions = {row.get("effective-http-version") for sample in result.get("samples", [])
                for row in sample["metrics"].get("streams", [])}
    versions.update(row.get("effective-http-version")
                    for row in result.get("final-metrics", {}).get("streams", []))
    result["observed-stream-http-versions"] = sorted(v for v in versions if v in (11, 20, 30))
    expected = MODES[mode][1]
    expected_name = {11: "HTTP/1.1", 20: "HTTP/2.0", 30: "HTTP/3.0"}[expected]
    # Each port has its own numeric/enum-only log. Scope evidence to this run.
    with (root / (mode + ".access.log")).open("rb") as log:
        log.seek(log_offset)
        matches = sum(line.split() == [expected_name.encode(), b"206"] for line in log)
    result["server-expected-protocol-range-responses"] = matches
    if expected in versions and matches > 0:
        result["protocol-scope"] = mode
        return True
    if mode == "prefer-h3":
        # Safety gates can legitimately retain TCP on constrained hosts. Keep
        # that acceptance distinct from a claim that HTTP/3 met this workload.
        streams = [row for sample in result.get("samples", [])
                   for row in sample["metrics"].get("streams", [])]
        streams.extend(result.get("final-metrics", {}).get("streams", []))
        reasons = sorted({row.get("last-reason") for row in streams
                          if row.get("last-reason") in (25, 29, 30, 31, 32, 33, 37)})
        with (root / (mode + ".access.log")).open("rb") as log:
            log.seek(log_offset)
            tcp_responses = sum(line.split() in ([b"HTTP/2.0", b"206"], [b"HTTP/1.1", b"206"])
                                for line in log)
        if reasons and versions.intersection((11, 20)) and tcp_responses:
            result["protocol-scope"] = "tcp-fallback"
            result["http3-unavailable-reasons"] = reasons
            return True
    return False


def save_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libmpv", required=True)
    parser.add_argument("--transport-helper", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference-only", action="store_true")
    parser.add_argument("--counts", default="1,4,auto")
    parser.add_argument("--protocols", default="http1,tcp,prefer-h3")
    parser.add_argument("--loss-percent", type=float)
    args = parser.parse_args()
    counts = args.counts.split(",")
    protocols = args.protocols.split(",")
    if not counts or any(count not in ("1", "4", "auto") for count in counts):
        parser.error("counts must be a comma separated selection of 1,4,auto")
    if not protocols or any(mode not in MODES for mode in protocols):
        parser.error("protocols must be a comma separated selection of http1,tcp,prefer-h3")
    if args.loss_percent is not None and (not math.isfinite(args.loss_percent)
                                          or not 0 <= args.loss_percent <= 100):
        parser.error("loss-percent must be between 0 and 100")
    args.counts = list(dict.fromkeys(counts))
    args.protocols = list(dict.fromkeys(protocols))
    return args


def main():
    args = parse_args()
    report = {"schema": 1, "status": "running", "reference-only": args.reference_only,
              "loss-percent": args.loss_percent, "prerequisites": {}, "results": [],
              "requested-path-mbps": float(os.environ.get("SPOOL_TRANSPORT_RATE_MBIT", "200")),
              "requested-rtt-ms": float(os.environ.get("SPOOL_TRANSPORT_RTT_MS", "150")),
              "measured-rtt-ms": float(os.environ.get("SPOOL_TRANSPORT_MEASURED_RTT_MS", "0")),
              "path-configured-by-wrapper": os.environ.get("SPOOL_TRANSPORT_NETNS") == "1",
              "capacity-margin": 1.05, "request-size-limit-bytes": 16 << 20,
              "timed-deadline-seconds": 100, "capacity-deadline-seconds": 180,
              "seek": {"status": "not-measured", "reason": "continuous-playback-acceptance"}}
    exit_code = 1
    try:
        require(args.reference_only or report["path-configured-by-wrapper"], "isolated-shaped-path-required")
        with tempfile.TemporaryDirectory(prefix="spool-high-bitrate-") as directory:
            root = Path(directory)
            report["prerequisites"]["fixture"] = generate_wave(root / "fixture.wav")
            base = {"library": args.libmpv, "helper": str(args.transport_helper.resolve()),
                    "source": str(root / "fixture.wav"), "pcm": str(root / "reference.pcm")}
            reference = run_player({**base, "timed": False, "deadline": 90})
            report["prerequisites"]["local-decode"] = reference
            save_report(args.output, report)
            require(reference["status"] == "complete" and reference["eof"], "local-decode-failed")
            require((root / "reference.pcm").stat().st_size == PCM_BYTES,
                    "local-decode-duration-or-format-mismatch")
            reference["pcm-bytes"] = PCM_BYTES
            reference["decoded-duration-seconds"] = PCM_BYTES / (48000 * 4)
            reference["pcm-sha256"] = file_hash(root / "reference.pcm")
            require(abs((reference.get("media-duration-seconds") or 0) - DURATION) < .01,
                    "local-source-duration-mismatch")
            cpu = run_player({**base, "timed": True, "deadline": 65, "stop-at": 45})
            report["prerequisites"]["local-timed-cpu"] = cpu
            save_report(args.output, report)
            require(cpu["status"] == "complete" and cpu["max-output-position-seconds"] >= 45
                    and cpu.get("post-startup-cache-pause-count", 0) == 0
                    and cpu["seconds"] <= 50, "local-timed-cpu-prerequisite-failed")
            if args.reference_only:
                report["status"] = "reference-passed"
                exit_code = 0
            else:
                with nginx_fixture(root) as ports:
                    epoch = 0
                    for mode in args.protocols:
                        capacities = {}
                        # Timed auto always needs a separately measured auto capacity.
                        for label in dict.fromkeys([*args.counts, "auto"]):
                            epoch += 1
                            origin = f"https://127.0.0.1:{ports[mode]}"
                            network = {**base, "source": origin + "/fixture.wav",
                                       "origin": origin, "mode": mode, "epoch": epoch,
                                       "count": 0 if label == "auto" else int(label),
                                       "ca": str(root / "cert.pem"),
                                       "pcm": str(root / "actual.pcm")}
                            log = root / (mode + ".access.log")
                            offset = log.stat().st_size if log.exists() else 0
                            row = run_player({**network, "timed": False, "deadline": 180})
                            row.update({"phase": "capacity", "protocol": mode, "count": label,
                                        "supplemental-auto": label not in args.counts})
                            report["results"].append(row)
                            save_report(args.output, report)
                            require(row.get("failure") != "missing-http2-or-http3", "missing-http2-or-http3")
                            if row["status"] == "complete":
                                actual = root / "actual.pcm"
                                row["pcm-bytes"] = actual.stat().st_size
                                row["pcm-sha256"] = file_hash(actual)
                                row["decoded-duration-seconds"] = row["pcm-bytes"] / (48000 * 4)
                                require(row["pcm-bytes"] == PCM_BYTES and
                                        row["pcm-sha256"] == reference["pcm-sha256"], "network-pcm-corruption")
                                require(protocol_evidence(row, mode, root, offset), "negotiated-protocol-mismatch")
                                row["decoded-input-bits-per-second"] = PAYLOAD_BYTES * 8 / row["seconds"]
                                row["capacity-sufficient"] = (row["decoded-input-bits-per-second"]
                                                               >= BYTE_RATE * 8 * 1.05)
                                row["status"] = "passed" if row["capacity-sufficient"] else "insufficient-capacity"
                            else:
                                row["capacity-sufficient"] = False
                            capacities[label] = row
                            if (root / "actual.pcm").exists():
                                (root / "actual.pcm").unlink()
                            save_report(args.output, report)
                            print(json.dumps({"phase": "capacity", "protocol": mode,
                                              "count": label, "status": row["status"]}), flush=True)
                        epoch += 1
                        network.update({"count": 0, "epoch": epoch})
                        offset = log.stat().st_size
                        timed = run_player({**network, "timed": True, "deadline": 100})
                        timed.update({"phase": "timed", "protocol": mode, "count": "auto",
                                      "capacity-sufficient": capacities["auto"]["capacity-sufficient"]})
                        report["results"].append(timed)
                        if timed["status"] == "complete":
                            require(protocol_evidence(timed, mode, root, offset), "negotiated-protocol-mismatch")
                        if not timed["capacity-sufficient"]:
                            timed.update(status="inconclusive", failure="insufficient-capacity")
                        elif (timed["status"] != "complete" or not timed["eof"]
                              or timed["max-output-position-seconds"] < 59
                              or timed["max-audio-pts-seconds"] < 59
                              or timed.get("post-startup-cache-pause-count", 0) > 0
                              or timed.get("post-startup-cache-pause-seconds", 0) > 0
                              or timed["seconds"] - (timed.get("first-progress-seconds") or 0) > 63):
                            timed.update(status="failed", failure="timed-playback-acceptance")
                        else:
                            timed["status"] = "passed"
                        save_report(args.output, report)
                        print(json.dumps({"phase": "timed", "protocol": mode,
                                          "count": "auto", "status": timed["status"]}), flush=True)
                # Fixed-count rows are comparisons, including a slow baseline.
                # Every auto capacity and timed row must meet the actual gate.
                automatic = [row for row in report["results"] if row["count"] == "auto"]
                report["acceptance-scope"] = "automatic-capacity-and-timed-playback"
                report["status"] = ("passed" if automatic and all(row["status"] == "passed"
                                                   for row in automatic) else "failed-or-inconclusive")
                exit_code = 0 if report["status"] == "passed" else 1
    except AcceptanceFailure as error:
        report.update(status="failed", failure=str(error))
    except KeyboardInterrupt:
        report.update(status="interrupted", failure="interrupted")
        exit_code = 130
    except Exception as error:
        report.update(status="failed", failure="harness-error", **{"error-type": type(error).__name__})
    finally:
        save_report(args.output, report)
    print(json.dumps({"status": report["status"], "failure": report.get("failure")}), flush=True)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
