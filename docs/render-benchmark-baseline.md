# Render benchmark: the numbers before qmltc

Recorded on `c43e69c` (v0.7.3), before any QML Type Compiler work, so the effect
of that work can be stated rather than felt.

Read the cold table, not the warm one. Warm switches reuse a page that is
already built and already cost what they are going to cost -- 6-7 ms, nothing
in it to win. The number qmltc is aimed at is **construct**, which is 17-20 ms
of the ~22 ms a cold switch takes: the QML engine walking a compiled document
and building an object tree. That is the work a compiled C++ class does not do.

## How these were taken

    nix develop .#native -c env SPOOL_BENCH=routes SPOOL_BENCH_ITERATIONS=6 \
      bash tools/launch-native-app.sh build/linux-release/install/bin/jellyfin-native

    # and again with SPOOL_BENCH_COLD=1

Six iterations, offscreen, `QT_QUICK_BACKEND=software`. Software rasterisation
means the paint costs here are not what a real GPU pays; construct time is CPU
work either way and is the comparable figure. Machine: AMD Ryzen 9 7900 12-Core Processor.

Re-run both on the same machine after the change. A comparison taken on another
machine says nothing -- see the noise-floor reasoning in
`tools/compare-render-benchmark.py`.

## Warm -- pages already built

Configured core.hooksPath=tools/git-hooks (pre-commit + pre-push)
### Render benchmark (warm)

Frame budget 16.67 ms · 36 switches · 6 iterations

Machine noise floor 0.7 ms (idle timer drift, 90th percentile) · gap threshold 16.7 ms

| route | worst frame gap | wall | gui cpu | construct | swaps | worst gap |
|---|---|---|---|---|---|---|
| home | 0.0 ms | 6.5 ms | 2.2 ms | 0.0 ms | 1 | 0.0 ms |
| libraryGrid | 0.0 ms | 6.0 ms | 1.7 ms | 0.0 ms | 1 | 0.0 ms |
| openSourceNotices | 0.0 ms | 6.6 ms | 2.2 ms | 0.0 ms | 1 | 0.0 ms |
| search | 0.0 ms | 6.3 ms | 2.0 ms | 0.0 ms | 1 | 0.0 ms |
| settings | 0.0 ms | 7.2 ms | 2.8 ms | 0.0 ms | 1 | 0.0 ms |

Worst single frame gap across every switch: **0.0 ms** (informational; the gate is on the per-route median).

## Cold -- every page dropped between switches

Configured core.hooksPath=tools/git-hooks (pre-commit + pre-push)
### Render benchmark (cold)

Frame budget 16.67 ms · 36 switches · 6 iterations

Machine noise floor 0.7 ms (idle timer drift, 90th percentile) · gap threshold 16.7 ms

| route | worst frame gap | wall | gui cpu | construct | swaps | worst gap |
|---|---|---|---|---|---|---|
| home | 2.6 ms | 22.7 ms | 5.2 ms | 19.1 ms | 2 | 3.0 ms |
| libraryGrid | 2.2 ms | 22.5 ms | 5.5 ms | 18.7 ms | 2 | 2.5 ms |
| openSourceNotices | 3.0 ms | 24.6 ms | 7.9 ms | 19.6 ms | 2 | 3.1 ms |
| search | 2.1 ms | 22.2 ms | 5.1 ms | 18.7 ms | 2 | 2.9 ms |
| settings | 7.3 ms | 45.3 ms | 10.9 ms | 16.7 ms | 4 | 10.9 ms |

Worst single frame gap across every switch: **10.9 ms** (informational; the gate is on the per-route median).

## What to expect, and what would count as failure

Construct is the target. Wall should follow it down; gui cpu should fall with
it. Swaps, delegate counts, and worst frame gap should not move at all -- if
they do, the compiled page is behaving differently from the interpreted one,
which is a correctness problem and not a win.

Settings is the outlier worth watching: 45 ms cold across 4 swaps, against
22-25 ms and 2 swaps everywhere else. It is the one page whose cost is not
mostly construct, so it is also the one qmltc should help least.

## What the first attempt at enabling it found

A spike on this branch turned the type compiler on across the whole module and
built it. None of it is committed -- the build does not pass yet -- but what it
learned is worth not learning twice.

**Turning it on is one line.** `qt_add_qml_module(... ENABLE_TYPE_COMPILER)`.
The standalone `qt_target_compile_qml_to_cpp()` is deprecated in 6.11 and warns
that it is going away; do not reach for it.

**96 of 100 documents compile as they stand.** That was the surprise. The four
that do not:

| document | why |
|---|---|
| `qml/primitives/ActionButton.qml` | binds an object carrying an `id` to `contentItem` |
| `qml/primitives/ChoiceStrip.qml` | same |
| `qml/primitives/SettingRow.qml` | same |
| `qml/shell/VideoSurface.qml` | `MpvVideoItem` cannot be resolved to C++ from here |

`contentItem` is a deferred property on `Control`. qmltc refuses an `id` inside
one because the id would force the assignment to happen immediately, which
changes when the object is built -- the diagnostic is `deferred-property-id`.
The fix is to restructure so no id lives under the deferred property; the ids
in question are referenced elsewhere in their files, so it is real work, not a
deletion.

**Skipping a file is not a way around this.** `QT_QML_SKIP_TYPE_COMPILER` is
per-file and looks like an escape hatch, but a document that skips compilation
still has to exist as a C++ base for anything that derives from it.
`ItemDetailsPage.qml` declares `component DetailAction: ActionButton`, so with
`ActionButton` skipped the generated `ItemDetailsPage_DetailAction` inherits
nothing and every property access on it fails to compile
(`has no member named 'setPointerHovered'`). The three primitives have to be
fixed, not excluded.

**The generated code needs Qt's private modules.** Six of them, found and
linked: `QmlPrivate`, `QuickPrivate`, `QuickLayoutsPrivate`,
`QuickEffectsPrivate`, `QuickControls2Private`, `QuickTemplates2Private`. Each
generated file includes the private header of whatever Qt type its QML touches,
so the list grows with what the UI uses. This is a build requirement of the
type compiler, not our code reaching into Qt internals -- but it does mean the
build now depends on Qt private API, which is not source-compatible across
minor releases.

**Past those, more remained.** `appshell.h` failed with `expected class-name`,
which is an unresolved base class. That was as far as the spike went.

## The part that decides whether any of this is worth it

qmltc emits a C++ class per document, and **those classes only run if something
constructs them**. Spool builds pages by URL, in `RouteStack.pageSource()`,
handed to `Loader.setSource()` -- the engine instantiates from the compiled
bytecode, and a generated class sitting in the binary is never reached.

So enabling the type compiler is necessary but not sufficient. The construct
time above does not move until the route host asks C++ for a page instead of a
URL, which means a page factory and a fallback path for whatever is not
compiled. **Measure that on one page before converting a hundred**: HomePage or
LibraryGridPage, factory-constructed, against the cold numbers here. If
construct does not fall for one page, it will not fall for all of them, and the
private-API dependency is not worth carrying.
