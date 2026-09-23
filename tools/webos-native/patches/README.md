Local patches for third-party source used in the webOS native toolchain.

These are applied by `tools/webos-native/build-qt6-611.sh` when building the
Qt host/target prefixes consumed by `build-ipk.sh`. They are named by Qt
*series* (`6.11`) rather than the exact patch release, and the build script
references them via `$QT_SERIES`, so a 6.11.x point bump (e.g. 6.11.0 ->
6.11.1) needs no rename — only `QT_VERSION` changes. Bump to a new series only
if a patch stops applying.

`qtbase-6.11-webos-qstorageinfo-linux.patch`
- Fixes `QStorageInfo` on the webOS ARM sysroot, whose glibc `struct statfs64`
  does not expose `f_flags`.
- Keeps space reporting on `statfs64`, but falls back to `statvfs64().f_flag`
  for readonly detection.

`qtbase-6.11-webos-qelfparser.patch`
- Fixes `qelfparser_p.cpp` against the older webOS SDK `elf.h`, which lacks
  the `EM_AARCH64` constant used in a debug switch.

`qtbase-6.11-webos-qplatformwindow-private-moc.patch`
- Includes `moc_qplatformwindow_p.cpp` so the private `QPlatformWindow` moc
  output links in the static build.

`qtbase-6.11-webos-wayland-no-opengl-forward-decl.patch`
- Moves the `QPlatformOpenGLContext` forward declaration out of the
  `QT_CONFIG(opengl)` guard so the non-OpenGL webOS Wayland build compiles.

`qtbase-6.11-webos-no-cursor-set.patch`
- Adds a `SPOOL_QT_NO_CURSOR_SURFACE=1` opt-out that skips client-side
  `set_cursor` in the Wayland QPA, so the app never displaces or hides the
  LSM-owned magic-remote pointer (mirrors xbmc's no-op `SetCursor`).

`qtdeclarative-6.11-qmlimportscanner-exclude-subtrees.patch`
- Makes `qmlimportscanner -exclude <directory>` ignore the complete directory
  subtree. Upstream's iterator otherwise descends into excluded in-tree build
  directories and parses Qt's intentionally invalid QML parser fixtures.

`qtwayland-6.11-webos-wayland-version.patch`
- Lowers the Wayland package version gate from `1.15` to `1.11` so QtWayland
  can target the older Wayland stack shipped in the webOS SDK sysroot.
