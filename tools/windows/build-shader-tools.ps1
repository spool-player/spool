param([switch] $Clean)

# libplacebo needs a GLSL compiler for both its Vulkan and its D3D11 backend,
# and SPIRV-Cross on top of that to reach HLSL for D3D11. Linux takes both from
# nixpkgs. Windows has neither, and Meson's WrapDB carries no glslang and no
# shaderc, so there is nothing to install the way curl or harfbuzz are
# installed -- they are built from source here, as FFmpeg already is.
#
# Vulkan headers are deliberately not among them: libplacebo carries its own
# under 3rdparty and uses those when they are present, so nothing here needs
# the Vulkan SDK.
. (Join-Path $PSScriptRoot 'common.ps1')
# The same clang/MSVC toolchain mpv is built with, so these libraries are
# compiled by the compiler that will link against them.
Initialize-WindowsMpvBuildEnvironment
$root = Get-RepositoryRoot
$deps = Join-Path $root 'build\windows-deps'
$pin = (Get-ToolchainManifest).shaderTools
$prefix = Join-Path $deps 'shader-tools'
New-Item -ItemType Directory -Force $deps | Out-Null

$stamp = Join-Path $prefix '.spool-shader-inputs'
$identity = (@(
    $PSCommandPath,
    (Join-Path $PSScriptRoot 'common.ps1'),
    (Join-Path $PSScriptRoot 'extract-archive.py'),
    (Join-Path $root 'tools\manifests\toolchain.json'),
    (Get-Command clang.exe -ErrorAction Stop).Source
) | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }) -join "`n"
$current = (Test-Path -LiteralPath $stamp) -and
    ((Get-Content -LiteralPath $stamp -Raw) -eq $identity)
if (($Clean -or -not $current) -and (Test-Path $prefix)) {
    Remove-Item -LiteralPath $prefix -Recurse -Force
}

function Get-Pinned {
    param([string] $Name, [object] $Source)

    $archive = Join-Path $deps "$Name-$($Source.sha256).tar.gz"
    $extracted = Join-Path $deps "$Name-$($Source.sha256)"
    if (-not (Test-Path $archive)) { Invoke-WebRequest $Source.url -OutFile $archive }
    if ((Get-FileHash $archive -Algorithm SHA256).Hash -ine $Source.sha256) {
        throw "$Name source checksum mismatch: $archive"
    }
    if (-not (Test-Path $extracted)) {
        New-Item -ItemType Directory -Force $extracted | Out-Null
        # One directory deep in the tarball, named for the tag.
        Expand-WindowsSourceArchive -Archive $archive -Destination $extracted -StripComponents 1
    }
    return $extracted
}

# glslang, static: libplacebo finds it with find_library plus a check for
# glslang/build_info.h, and looks under <vulkan-sdk>/lib for the libraries, so
# the prefix is handed to it as though it were an SDK.
$glslangMarker = Join-Path $prefix 'include\glslang\build_info.h'
if (-not (Test-Path $glslangMarker)) {
    $source = Get-Pinned 'glslang' $pin.glslang
    $build = Join-Path $deps 'glslang-build'
    if (Test-Path $build) { Remove-Item -LiteralPath $build -Recurse -Force }
    # mpv and its subprojects use the static runtime, and a static library
    # linked into them has to agree: lld-link refuses the mismatch outright.
    cmake -S $source -B $build -GNinja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_INSTALL_PREFIX="$prefix" `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
        -DUSE_MSVC_RUNTIME_LIBRARY_DLL=OFF `
        -DENABLE_OPT=OFF `
        -DENABLE_GLSLANG_BINARIES=OFF `
        -DGLSLANG_TESTS=OFF `
        -DBUILD_SHARED_LIBS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Configuring glslang failed.' }
    cmake --build $build --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Building glslang failed.' }
    cmake --install $build
    if ($LASTEXITCODE -ne 0) { throw 'Installing glslang failed.' }
}

# The Vulkan headers. libplacebo carries its own under 3rdparty and builds
# against those, but does not put them in the dependency it hands to whoever
# links it -- and mpv's own sources include libplacebo/vulkan.h, which includes
# vulkan/vulkan.h. No loader comes with them: nothing calls a Vulkan entry
# point here, the handles are only passed through to libplacebo.
$vulkanMarker = Join-Path $prefix 'include\vulkan\vulkan.h'
if (-not (Test-Path $vulkanMarker)) {
    $source = Get-Pinned 'vulkan-headers' $pin.vulkanHeaders
    $build = Join-Path $deps 'vulkan-headers-build'
    if (Test-Path $build) { Remove-Item -LiteralPath $build -Recurse -Force }
    cmake -S $source -B $build -GNinja -DCMAKE_INSTALL_PREFIX="$prefix"
    if ($LASTEXITCODE -ne 0) { throw 'Configuring the Vulkan headers failed.' }
    cmake --install $build
    if ($LASTEXITCODE -ne 0) { throw 'Installing the Vulkan headers failed.' }
}

# SPIRV-Cross, shared: libplacebo asks pkg-config for spirv-cross-c-shared,
# which only the shared build installs a .pc for.
$spirvMarker = Join-Path $prefix 'lib\pkgconfig\spirv-cross-c-shared.pc'
if (-not (Test-Path $spirvMarker)) {
    $source = Get-Pinned 'spirv-cross' $pin.spirvCross
    $build = Join-Path $deps 'spirv-cross-build'
    if (Test-Path $build) { Remove-Item -LiteralPath $build -Recurse -Force }
    cmake -S $source -B $build -GNinja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_INSTALL_PREFIX="$prefix" `
        -DSPIRV_CROSS_SHARED=ON `
        -DSPIRV_CROSS_STATIC=ON `
        -DSPIRV_CROSS_CLI=OFF `
        -DSPIRV_CROSS_ENABLE_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Configuring SPIRV-Cross failed.' }
    cmake --build $build --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Building SPIRV-Cross failed.' }
    cmake --install $build
    if ($LASTEXITCODE -ne 0) { throw 'Installing SPIRV-Cross failed.' }
}

if (-not (Test-Path $spirvMarker)) {
    throw "SPIRV-Cross did not install a pkg-config file at $spirvMarker"
}
if (-not (Test-Path $glslangMarker)) {
    throw "glslang did not install its headers at $glslangMarker"
}
if (-not (Test-Path $vulkanMarker)) {
    throw "the Vulkan headers did not install at $vulkanMarker"
}
# CMake leaves the environment as it found it, but the compiler probes above
# can put a different toolchain first. Meson is configured after this and
# detects its compiler from scratch, so hand it back the one it expects --
# the same reason build-ffmpeg.ps1 ends this way.
Initialize-WindowsMpvBuildEnvironment
[IO.File]::WriteAllText($stamp, $identity, [Text.UTF8Encoding]::new($false))
Write-Host "shader tools: $prefix"
