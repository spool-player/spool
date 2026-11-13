param([switch] $Clean)
. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsMpvBuildEnvironment
$root = Get-RepositoryRoot
$msys = Get-Msys2Root
$bash = Join-Path $msys 'usr\bin\bash.exe'
$cygpath = Join-Path $msys 'usr\bin\cygpath.exe'
$sources = Join-Path $root 'build\windows-deps\streaming-sources'
$builds = Join-Path $root 'build\windows-deps\streaming-build'
$prefix = Join-Path $root 'build\windows-deps\streaming-prefix'
$unixRoot = (& $cygpath -u $root).Trim()
$unixSources = (& $cygpath -u $sources).Trim()
& $bash "$unixRoot/tools/prepare-streaming-sources.sh" --root $unixRoot --output $unixSources
if ($LASTEXITCODE -ne 0) { throw 'Preparing verified streaming sources failed.' }
$fingerprintArgs = @((Join-Path $root 'tools\streaming-fingerprint.py'), '--root', $root,
    '--target', "windows-x64:static:MD:$(& clang --version)", '--wrapper', $PSCommandPath)
$pin = (& python @fingerprintArgs).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Computing streaming dependency identity failed.' }
$stamp = Join-Path $prefix '.spool-streaming-configuration'
if (-not $Clean -and (Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq $pin) { return }
if (Test-Path $builds) { Remove-Item $builds -Recurse -Force }
if (Test-Path $prefix) { Remove-Item $prefix -Recurse -Force }
$opensslBuild = Join-Path $builds 'openssl'
New-Item -ItemType Directory -Force $opensslBuild | Out-Null
Push-Location $opensslBuild
try {
    & perl (Join-Path $sources 'openssl\Configure') VC-WIN64A no-shared no-apps no-docs no-tests `
        no-comp no-dtls no-srp no-ssl3 no-weak-ssl-ciphers -MD "--prefix=$prefix" --libdir=lib
    if ($LASTEXITCODE -ne 0) { throw 'Configuring OpenSSL failed.' }
    & nmake build_libs
    if ($LASTEXITCODE -ne 0) { throw 'Building OpenSSL failed.' }
    & nmake install_dev
    if ($LASTEXITCODE -ne 0) { throw 'Installing OpenSSL failed.' }
} finally { Pop-Location }
$env:PKG_CONFIG_PATH = (& $cygpath -u "$prefix/lib/pkgconfig").Trim()
foreach ($name in @('nghttp2', 'nghttp3', 'ngtcp2', 'curl')) {
    & cmake -S (Join-Path $sources $name) -B (Join-Path $builds $name) -GNinja `
        "-DSTREAMING_DEPENDENCY=$name" -DSTREAMING_SHARED=OFF `
        -C (Join-Path $root 'tools\cmake\StreamingDependencies.cmake') `
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_INSTALL_PREFIX=$prefix" `
        "-DCMAKE_PREFIX_PATH=$prefix" "-DOPENSSL_ROOT_DIR=$prefix" -DOPENSSL_USE_STATIC_LIBS=ON `
        -DCURL_ZLIB=OFF -DCURL_USE_SCHANNEL=OFF
    if ($LASTEXITCODE -ne 0) { throw "Configuring $name failed." }
    & cmake --build (Join-Path $builds $name) --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "Building $name failed." }
    & cmake --install (Join-Path $builds $name)
    if ($LASTEXITCODE -ne 0) { throw "Installing $name failed." }
}
# curl-config is a shell script even in a Windows cross-compatible install.
$unixPrefix = (& $cygpath -u $prefix).Trim()
$features = (& $bash "$unixPrefix/bin/curl-config" --features) -join ' '
if ($LASTEXITCODE -ne 0 -or $features -notmatch '\bHTTP2\b' -or $features -notmatch '\bHTTP3\b' -or $features -notmatch '\balt-svc\b') {
    throw 'Installed curl lacks HTTP2, HTTP3 or alt-svc.'
}
[IO.File]::WriteAllText($stamp, "$pin`n")
