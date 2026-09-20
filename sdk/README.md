# Spool provider SDK: experimental API 0.1

The compatibility revision is exactly `0.1`. Additive fixes retain that revision; a breaking change requires `0.2`. Spool and provider release versions are independent. No historical-version adapter is implemented.

## Worker profile

An ES module exports `createSource(configuration)`, returning an object whose own methods implement operations. Each operation receives `(arguments, host)` and returns a plain object or a Promise for one. Create per-source state in the factory closure, not in module globals. Use Promise syntax; Node, browser globals and async-function syntax are not part of this baseline.

`host.http(url, {method, headers, body})` returns a Promise for `{status, body}`. The source's authorised HTTP(S) origins are selected by native code. Redirects are returned to the provider, not automatically followed. Cookies are neither loaded nor saved implicitly. HTTP error statuses remain inspectable; transport failure rejects. Parse and normalise response text in the worker. Never return backend response payloads indiscriminately.

Native limits: 8 MiB decoded HTTP responses; 1 MiB request bodies; four concurrent HTTP requests per operation; eight operations per source; 32 active operations and 64 queued submissions per runtime; 16 source objects per runtime. Operation deadline is 15 seconds, transport inactivity deadline 10 seconds, and uninterrupted JS execution budget 500 ms. The watchdog covers Promise continuations as well as direct calls. Exceeding execution budget disables the module; create a fresh runtime for explicit recovery.

Results are plain owned native values, bounded to 50,000 values, nesting depth 20, arrays of 10,000 elements and 4 MiB of string data. Non-finite and unsafe numeric values are rejected. Represent large counters and exact timestamps as decimal strings. IDs are opaque strings. Source IDs are host authority, not provider-controlled credential selectors.

Removing a source cancels its outstanding native requests and completes pending operations with an error. Other sources remain available. Shutdown also completes outstanding operations. Native callers receive `QCoro::Task<QVariantMap>` on their calling thread. There are no cross-thread `QJSValue` objects.

This is a trusted/reviewed in-process execution profile, **not a sandbox**. Timers, durable secret/storage services, module manager and UI-to-worker RPC are not yet public services. Do not declare that the full portable-provider plan is implemented by this runtime alone.

## Contract runner

Build this directory with CMake and the host Qt development environment:

```
cmake -S sdk -B build/provider-sdk
cmake --build build/provider-sdk
build/provider-sdk/provider-contract-runner /path/to/provider/tests/contract.mjs
QV4_FORCE_INTERPRETER=1 build/provider-sdk/provider-contract-runner /path/to/provider/tests/contract.mjs
```

The test module exports `run()`, returning a Promise or throwing on failure. It runs in a real QJSEngine, not Node. The host's runtime tests additionally exercise actual asynchronous HTTP, thread ownership, source isolation, cancellation and runaway JS. Put a process timeout around external contract tests: this small test runner is not the production worker watchdog.

## Source packages

`tools/provider-package.py build PATH --output provider.zip` creates a deterministic source ZIP. `validate provider.zip` checks its manifest, required host features, known permissions, declared UI components/imports, paths, file types and size limits. Unknown offered and optional extensions are allowed; missing required extensions reject the package. Every QML component must be listed so it can be validated and warmed.

Validation does not authenticate publisher identity. A release digest or provenance attestation is not a substitute for an authenticated catalogue with freshness, rotation and revocation. The application does not yet install or activate these ZIPs. Do not enable downloaded code in store builds on the strength of this tool.
