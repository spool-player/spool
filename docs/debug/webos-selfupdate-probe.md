# webOS self-update probe

Date: 2026-09-03

## Question

Can the app install an IPK by itself on a television that has not been rooted?
If it can, Spool can update itself; if it cannot, every update stays a manual
sideload.

LS2 will not answer this by inspection. A role is not readable as a capability
list, so the only way to find out is to make the call and read which kind of
"no" comes back:

- **Denied by the bus** — `Denied method call ... for security reasons`, or a
  call that never dispatches at all. The role forbids it. No amount of
  retrying or rephrasing changes it.
- **An error from the service** — the call was allowed through and only the
  request was wrong. Solvable.

Those two look nothing alike and mean opposite things, so the probe logs every
reply verbatim and judges nothing.

## How it runs

- `src/platform/webos/WebOSSelfUpdateProbe.cpp` — compiled in only when
  `SPOOL_WEBOS_SELFUPDATE_PROBE` is set; it never ships. Fires five seconds
  after launch, reports which rolegen directories exist, then makes each call
  in the plan.
- `tools/webos/selfupdate-probe.sh` — builds the diagnostic IPK with the target
  URL baked in (a webOS app gets no environment of its own to read at launch),
  installs it through `verify-device.sh`, serves a real IPK over HTTP from this
  machine, launches over LS2, and prints the transcript.

The plan opens with two controls — `listLaunchPoints` and `getAppInfo` — that
should succeed for any app. If those are denied too, the probe itself is
broken and the install denials say nothing.

## Results, run 2

Rooted TV, probe build 0.7.7.

**No LS2 security denial anywhere.** All five calls dispatched and every
service answered. Both controls returned normally, so the run is trustworthy
rather than blanket-denied. Permission is not what stops this on a rooted set.

| Route | Outcome |
| --- | --- |
| `control/listLaunchPoints` | Full launch-point list returned |
| `control/getAppInfo` | Full `appInfo` for `com.sachk.spool` returned |
| `appInstallService/dev/install` | Accepted, progressed, then `download failed` |
| `appInstallService/install` | **No verdict** — rejected as a duplicate |
| `hbchannel/install` | Accepted, fetched too late, `connect ETIMEDOUT` |

`dev/install` accepted the app as the install client — the status payloads
carry `"client":"com.sachk.spool"` — and reached statusValue 262 and 263
before failing:

```text
app/install failed Server error: [A.001.01 Wrong App ID] appFile is missing
"state":"download failed"
```

`appInstallService/install` never gave an answer at all:

```text
duplicate command while current command has not completed, ignoring
```

That is a defect in the probe, not a property of the television: the plan
fires all five calls back to back, so the production path arrived while
`dev/install` was still in flight for the same app id.

`hbchannel/install` was accepted and does take a URL, but it only attempted
the fetch at roughly 137 s, long after the script's 30 s settle had killed the
HTTP server.

## What the HTTP server saw

Nothing from the television. The only request in the access log came from the
preflight `curl` on the host itself.

So `dev/install` reported a download failure **without ever downloading**.
Reachability was verified immediately afterwards: the television pulled the
whole 24 MB IPK from the host on port 18927, HTTP 200, in 0.8 s. The blocked
port that spoiled run 1 is genuinely fixed and is not what happened here.

## Interpretation

`dev/install` most likely wants a **local file path**, not an `ipkUrl`. That is
how `ares-install` drives it from outside: copy the IPK to
`/media/developer/temp`, then call `dev/install` with the path. A service that
never issues an HTTP request, and then complains that `appFile is missing`,
fits a path argument it could not resolve better than it fits a download it
attempted and lost.

If that holds, a working self-update looks like: the app downloads the IPK
itself over ordinary HTTP into its own directory, then hands `dev/install` a
path. That is the fixable class of failure — the request was wrong, not
forbidden.

This is a hypothesis. It has not been tested.

## Defects found in the probe itself

- It read `/tmp/com.sachk.spool.log`, which holds a stale copy from an older
  layout, so it reported "no probe output found" while the transcript sat in
  the app's own `.cache/logs/`. Fixed.
- Every reply was labelled with garbage bytes. The webos-helpers header names
  the callback's third argument `userdata`, and what arrives is the `HContext`;
  the callback now resolves the pointer as either. **The fix is untested** — the
  IPK used in run 2 was built minutes before the fix landed, so the labels were
  still garbage. The routes were read off the payloads instead, which are
  self-identifying.
- The plan is not serialised, which is why the production path returned no
  verdict.
- The settle period is shorter than Homebrew Channel's own lag, and the HTTP
  server dies with the script.

## What this does not answer

The set is rooted. Every rolegen directory came back absent:

```text
/var/palm/ls2-dev/roles/pub          absent
/var/palm/ls2-dev/roles/prv          absent
/var/palm/ls2-dev/client-permissions.d  absent
/var/palm/ls2-dev/api-permissions.d     absent
/var/palm/ls2/roles/pub              absent
```

That is not what a retail television looks like. A pass here means "worth
trying on an unrooted set", never "this works". The unrooted question needs a
webOS 26 set reached through developer mode — `ares-install` as `prisoner`
rather than ssh as root.

## Next experiments

1. Rebuild so the label fix is actually in the binary.
2. Serialise the plan, waiting for each attempt to settle, so
   `appInstallService/install` gives a real verdict.
3. Hand `dev/install` a local path: download the IPK in-app to the app's own
   directory, then call with that path. Most likely of these to work.
4. Raise the settle period past Homebrew Channel's ~150 s lag and keep the
   server alive for the whole window.

A television left running the diagnostic build should be given a normal build
again afterwards, and handed back to HDMI1.
