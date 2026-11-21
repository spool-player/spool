param([switch] $Clean)
. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsMpvBuildEnvironment
$root = Get-RepositoryRoot
$nativePerl = Get-NativeWindowsPerl
if (-not $nativePerl) {
    throw 'OpenSSL VC-WIN64A requires native Windows Perl. Run tools\windows\install-build-tools.ps1 to install Strawberry Perl.'
}
$msys = Get-Msys2Root
$bash = Join-Path $msys 'usr\bin\bash.exe'
$cygpath = Join-Path $msys 'usr\bin\cygpath.exe'
$sources = Join-Path $root 'build\windows-deps\streaming-sources'
$builds = Join-Path $root 'build\windows-deps\streaming-build'
$prefix = Join-Path $root 'build\windows-deps\streaming-prefix'
function Convert-StreamingPath {
    param([string] $FilePath)
    $converted = & $cygpath -u $FilePath
    if ($LASTEXITCODE -ne 0 -or -not $converted) { throw "Converting streaming path failed: $FilePath" }
    return ($converted -join "`n").Trim()
}
$unixRoot = Convert-StreamingPath $root
$unixSources = Convert-StreamingPath $sources
$env:PKG_CONFIG = Join-Path $msys 'usr\bin\pkgconf.exe'
& $env:PKG_CONFIG --version | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'The target pkg-config resolver is unavailable.' }
Remove-Item Env:PKG_CONFIG_SYSROOT_DIR -ErrorAction SilentlyContinue
& $bash "$unixRoot/tools/prepare-streaming-sources.sh" --root $unixRoot --output $unixSources
if ($LASTEXITCODE -ne 0) { throw 'Preparing verified streaming sources failed.' }
$fingerprintArgs = @((Join-Path $root 'tools\streaming-fingerprint.py'), '--root', $root,
    '--target', "windows-x64:static:MD:$(& clang --version)", '--wrapper', $PSCommandPath)
$fingerprintOutput = & python @fingerprintArgs
if ($LASTEXITCODE -ne 0) { throw 'Computing streaming dependency identity failed.' }
$pin = ($fingerprintOutput -join "`n").Trim()
if ($pin -notmatch '^[0-9a-f]{64}$') { throw 'Invalid streaming dependency identity.' }
$stamp = Join-Path $prefix '.spool-streaming-configuration'
if (-not $Clean -and (Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq $pin) { return }
if (Test-Path $builds) { Remove-Item $builds -Recurse -Force }
if (Test-Path $prefix) { Remove-Item $prefix -Recurse -Force }
$opensslBuild = Join-Path $builds 'openssl'
New-Item -ItemType Directory -Force $opensslBuild | Out-Null
$originalStreamingPath = $env:PATH
$originalCC = $env:CC
$originalCXX = $env:CXX
$env:PATH = "$(Split-Path $nativePerl -Parent);$env:PATH"
# OpenSSL's VC-WIN64A target writes cl-style flags but takes its compiler
# from CC, which the mpv toolchain sets to the GNU-style clang driver.
$env:CC = 'cl'
$env:CXX = 'cl'
Push-Location $opensslBuild
try {
    & $nativePerl (Join-Path $sources 'openssl\Configure') VC-WIN64A no-shared no-apps no-docs no-tests `
        no-comp no-dtls no-srp no-ssl3 no-weak-ssl-ciphers -MD "--prefix=$prefix" --libdir=lib
    if ($LASTEXITCODE -ne 0) { throw 'Configuring OpenSSL failed.' }
    & nmake build_libs
    if ($LASTEXITCODE -ne 0) { throw 'Building OpenSSL failed.' }
    & nmake install_dev
    if ($LASTEXITCODE -ne 0) { throw 'Installing OpenSSL failed.' }
} finally {
    Pop-Location
    $env:PATH = $originalStreamingPath
    $env:CC = $originalCC
    $env:CXX = $originalCXX
}
# OpenSSL's MSVC install_dev writes CMake config files but omits pkg-config
# files, which libngtcp2_crypto_ossl.pc and libcurl.pc require via
# Requires.private when Meson resolves static libcurl.
$opensslVersionHeader = Get-Content -LiteralPath (Join-Path $prefix 'include\openssl\opensslv.h') -Raw
if ($opensslVersionHeader -notmatch '#\s*define\s+OPENSSL_VERSION_STR\s+"([^"]+)"') {
    throw 'Failed to read installed OpenSSL version from opensslv.h.'
}
$opensslVersion = $Matches[1]
$pkgConfigDir = Join-Path $prefix 'lib\pkgconfig'
New-Item -ItemType Directory -Force $pkgConfigDir | Out-Null
$prefixForward = $prefix -replace '\\', '/'
$utf8NoBom = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $pkgConfigDir 'libcrypto.pc'), @"
prefix=$prefixForward
exec_prefix=`${prefix}
libdir=`${exec_prefix}/lib
includedir=`${prefix}/include

Name: OpenSSL-libcrypto
Description: OpenSSL cryptography library
Version: $opensslVersion
Libs: -L`${libdir} -llibcrypto
Libs.private: -lws2_32 -lgdi32 -lcrypt32 -ladvapi32 -luser32
Cflags: -I`${includedir}
"@.Replace("`r`n", "`n") + "`n", $utf8NoBom)
[IO.File]::WriteAllText((Join-Path $pkgConfigDir 'libssl.pc'), @"
prefix=$prefixForward
exec_prefix=`${prefix}
libdir=`${exec_prefix}/lib
includedir=`${prefix}/include

Name: OpenSSL-libssl
Description: Secure Sockets Layer and cryptography libraries
Version: $opensslVersion
Requires.private: libcrypto
Libs: -L`${libdir} -llibssl
Cflags: -I`${includedir}
"@.Replace("`r`n", "`n") + "`n", $utf8NoBom)
[IO.File]::WriteAllText((Join-Path $pkgConfigDir 'openssl.pc'), @"
prefix=$prefixForward
exec_prefix=`${prefix}
libdir=`${exec_prefix}/lib
includedir=`${prefix}/include

Name: OpenSSL
Description: Secure Sockets Layer and cryptography libraries and tools
Version: $opensslVersion
Requires: libssl libcrypto
"@.Replace("`r`n", "`n") + "`n", $utf8NoBom)
$env:PKG_CONFIG_PATH = Convert-StreamingPath "$prefix/lib/pkgconfig"
foreach ($name in @('nghttp2', 'nghttp3', 'ngtcp2', 'curl')) {
    & cmake -S (Join-Path $sources $name) -B (Join-Path $builds $name) -GNinja `
        "-DSTREAMING_DEPENDENCY=$name" -DSTREAMING_SHARED=OFF `
        -C (Join-Path $root 'tools\cmake\StreamingDependencies.cmake') `
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_INSTALL_PREFIX=$prefix" `
        "-DCMAKE_PREFIX_PATH=$prefix" "-DOPENSSL_ROOT_DIR=$prefix" -DOPENSSL_USE_STATIC_LIBS=ON `
        -DCURL_ZLIB=OFF -DCURL_USE_SCHANNEL=OFF -DCURL_CA_NATIVE=ON
    if ($LASTEXITCODE -ne 0) { throw "Configuring $name failed." }
    & cmake --build (Join-Path $builds $name) --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "Building $name failed." }
    & cmake --install (Join-Path $builds $name)
    if ($LASTEXITCODE -ne 0) { throw "Installing $name failed." }
}
& cmake -S (Join-Path $root 'tools\cmake\QuicBatchingProbe') -B (Join-Path $builds 'quic-batching-probe') -GNinja `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL `
    "-DSTREAMING_CURL_SOURCE=$(Join-Path $sources 'curl')" "-DSTREAMING_CURL_BUILD=$(Join-Path $builds 'curl')" `
    "-DSTREAMING_INSTALLED_PC=$(Join-Path $prefix 'lib\pkgconfig\libcurl.pc')"
if ($LASTEXITCODE -ne 0) { throw 'Auditing compiled QUIC batching capabilities failed.' }
# Audit metadata using native Python and the explicit MSYS shell; no target
# binary or implicit system package can satisfy a wrong-version closure.
$curlPkgFlags = & $env:PKG_CONFIG --cflags --libs --static libcurl
if ($LASTEXITCODE -ne 0) { throw 'Static pkg-config resolution for libcurl failed.' }
Write-Host "libcurl static pkg-config: $curlPkgFlags"
& python (Join-Path $root 'tools\check-streaming-dependencies.py') --prefix $prefix `
    --manifest (Join-Path $root 'tools\manifests\streaming-third-party.json') --bash $bash --require-native-ca
if ($LASTEXITCODE -ne 0) { throw 'The Windows streaming feature/version audit failed.' }
[IO.File]::WriteAllText($stamp, "$pin`n")
