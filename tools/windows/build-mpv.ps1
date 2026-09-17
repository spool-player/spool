param(
    [switch] $Clean,
    [switch] $SkipWrapUpdate
)

. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsMpvBuildEnvironment
$msysRoot = Get-Msys2Root

$root = Get-RepositoryRoot
$source = Join-Path $root 'mpv'
$dependencyRoot = Join-Path $root 'build\windows-deps'
$buildSource = Join-Path $dependencyRoot 'mpv-source'
$buildDirectory = Join-Path $dependencyRoot 'mpv-build'
$prefix = Join-Path $dependencyRoot 'mpv'
$packageCache = Join-Path $dependencyRoot 'meson-package-cache'

foreach ($path in @($buildSource, $buildDirectory, $prefix, $packageCache)) {
    $resolvedParent = [IO.Path]::GetFullPath((Split-Path $path -Parent))
    if (-not $resolvedParent.StartsWith([IO.Path]::GetFullPath($dependencyRoot), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the Windows dependency root: $path"
    }
}

if ($Clean) {
    foreach ($path in @($buildSource, $buildDirectory, $prefix)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
}

# Work in a disposable mirror so Meson wraps never dirty the mpv submodule.
# The mirror is a copy, so it goes stale the moment the submodule moves; re-seed
# it then rather than building the previous fork again. Ordinary retries, where
# the revision is unchanged, still retain the downloaded wraps.
$sourceRevision = Get-MpvSourceRevision
if ($sourceRevision -and (Test-Path -LiteralPath $buildSource)) {
    $mirroredRevision = Read-MpvRevisionStamp -Directory $buildSource
    if ($mirroredRevision -ne $sourceRevision) {
        Write-Host "mpv source mirror is at '$(if ($mirroredRevision) { $mirroredRevision } else { 'an unrecorded revision' })'; re-seeding from $sourceRevision"
        Remove-Item -LiteralPath $buildSource -Recurse -Force
        if (Test-Path -LiteralPath $buildDirectory) {
            Remove-Item -LiteralPath $buildDirectory -Recurse -Force
        }
    }
}
if (-not (Test-Path -LiteralPath $buildSource)) {
    New-Item -ItemType Directory -Force $buildSource | Out-Null
    Get-ChildItem -LiteralPath $source -Force |
        Where-Object { $_.Name -notin @('.git', 'build') } |
        Copy-Item -Destination $buildSource -Recurse -Force
    Write-MpvRevisionStamp -Directory $buildSource -Revision $sourceRevision
}
New-Item -ItemType Directory -Force $packageCache | Out-Null

$env:MESON_PACKAGE_CACHE_DIR = $packageCache
$subprojects = Join-Path $buildSource 'subprojects'
New-Item -ItemType Directory -Force $subprojects | Out-Null

Push-Location $buildSource
try {
    if (-not $SkipWrapUpdate) {
        meson wrap update-db
        if ($LASTEXITCODE -ne 0) { throw 'Failed to update the Meson WrapDB catalog.' }
    }

    foreach ($wrap in @('curl', 'expat', 'freetype2', 'fribidi', 'harfbuzz', 'libpng', 'luajit', 'zlib', 'xxhash')) {
        if (-not (Test-Path (Join-Path $subprojects "$wrap.wrap"))) {
            meson wrap install $wrap
            if ($LASTEXITCODE -ne 0) { throw "Failed to install the Meson wrap: $wrap" }
        }
    }

    meson subprojects download curl
    if ($LASTEXITCODE -ne 0) { throw 'Failed to download the pinned curl source.' }
    $curlRoot = Get-ChildItem -LiteralPath $subprojects -Directory -Filter 'curl-*' | Select-Object -First 1
    if (-not $curlRoot) { throw 'The downloaded curl subproject was not found.' }
    $curlProject = Join-Path $curlRoot.FullName 'meson.build'
    $curlLibrary = Join-Path $curlRoot.FullName 'lib\meson.build'
    $curlProjectText = (Get-Content -LiteralPath $curlProject -Raw).Replace(
        "if get_option('default_library') == 'static'",
        'if true')
    $curlLibraryText = (Get-Content -LiteralPath $curlLibrary -Raw).Replace(
        'curl_lib = library(',
        'curl_lib = static_library(').Replace(
        "  version: '4.8.0',`n",
        '')
    [IO.File]::WriteAllText($curlProject, $curlProjectText, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($curlLibrary, $curlLibraryText, [Text.UTF8Encoding]::new($false))

    # FFmpeg is built from the same upstream source pin as every desktop.
    & (Join-Path $PSScriptRoot 'build-ffmpeg.ps1') -Clean:$Clean
    $ffmpegPrefix = Join-Path $dependencyRoot 'ffmpeg'
    $env:PKG_CONFIG_PATH = (& (Join-Path $msysRoot 'usr\bin\cygpath.exe') -u "$ffmpegPrefix/lib/pkgconfig").Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Converting the FFmpeg pkg-config search path failed.' }

    # A GLSL compiler and SPIRV-Cross, which libplacebo needs for D3D11 and
    # Vulkan alike and which Windows has no other way to get.
    & (Join-Path $PSScriptRoot 'build-shader-tools.ps1') -Clean:$Clean
    Initialize-WindowsMpvBuildEnvironment
    $shaderTools = Join-Path $dependencyRoot 'shader-tools'
    $shaderPkgConfig = (& (Join-Path $msysRoot 'usr\bin\cygpath.exe') -u "$shaderTools/lib/pkgconfig").Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Converting the shader tool pkg-config search path failed.' }
    $env:PKG_CONFIG_PATH = "$($env:PKG_CONFIG_PATH):$shaderPkgConfig"
    # mpv's own sources include libplacebo/vulkan.h, so the Vulkan headers have
    # to be on mpv's include path too, not only on libplacebo's.
    $shaderInclude = Join-Path $shaderTools 'include'
    $env:CFLAGS = "$env:CFLAGS -I$($shaderInclude -replace '\\', '/')".Trim()
    $env:CXXFLAGS = "$env:CXXFLAGS -I$($shaderInclude -replace '\\', '/')".Trim()
    # libplacebo hands its vulkan-sdk path to find_library for SPIRV but not
    # for glslang, which is then looked for on the default search path alone --
    # and found nowhere, silently, because it asks for it as optional. LIB is
    # that default path for clang and lld-link, so the libraries end up where
    # both the detection and the link will look.
    $env:LIB = "$(Join-Path $shaderTools 'lib');$env:LIB"
    # Discard the old Meson-port wrap when reusing a dependency checkout.
    Remove-Item (Join-Path $subprojects 'ffmpeg.wrap') -ErrorAction SilentlyContinue

    @'
[wrap-git]
url = https://github.com/libass/libass
revision = f9fd3d20dff1cd84b7c74c8ae7f79711ad7736fa
depth = 1
'@ | Set-Content -LiteralPath (Join-Path $subprojects 'libass.wrap') -Encoding ascii

    @'
[wrap-git]
url = https://code.videolan.org/videolan/libplacebo.git
revision = a7a18af88ff0a17c04840dcb3246047bb6b46df3
depth = 1
clone-recursive = true
'@ | Set-Content -LiteralPath (Join-Path $subprojects 'libplacebo.wrap') -Encoding ascii

    # This pinned libplacebo revision uses project_source_root(), which points
    # at mpv when libplacebo is nested as a Meson subproject. Correct the path
    # in the disposable mirror so its bundled Python generators are found.
    meson subprojects download libplacebo
    if ($LASTEXITCODE -ne 0) { throw 'Failed to download the pinned libplacebo source.' }
    $libplaceboRoot = Join-Path $subprojects 'libplacebo'
    & git -C $libplaceboRoot submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw 'Failed to initialize libplacebo submodules.' }
    $libplaceboMeson = Join-Path $libplaceboRoot 'meson.build'
    $libplaceboText = Get-Content -LiteralPath $libplaceboMeson -Raw
    $pythonExecutable = if (Get-Command py.exe -ErrorAction SilentlyContinue) {
        (& py.exe -3 -c 'import sys; print(sys.executable)').Trim()
    } elseif (Get-Command python.exe -ErrorAction SilentlyContinue) {
        (Get-Command python.exe).Source
    } else {
        throw 'A system Python interpreter is required by libplacebo build-time generators.'
    }
    $pythonMesonPath = $pythonExecutable.Replace('\', '/')
    $libplaceboText = $libplaceboText.Replace(
        "thirdparty = meson.project_source_root()/'3rdparty'",
        "thirdparty = meson.current_source_dir()/'3rdparty'")
    $libplaceboText = $libplaceboText.Replace(
        "python = import('python').find_installation()",
        "python = import('python').find_installation('$pythonMesonPath')")
    [IO.File]::WriteAllText($libplaceboMeson, $libplaceboText, [Text.UTF8Encoding]::new($false))

    # libplacebo hands vulkan-sdk/lib to the SPIRV lookup but not to the
    # glslang one beside it. Meson resolves a static find_library from
    # `clang++ --print-search-dirs`, which lists LLVM's own directories and
    # never reads LIB, so glslang is looked for where it cannot be and is
    # missed silently -- the link then fails on glslang::InitializeProcess.
    # Give that call the same search path its neighbour already gets.
    $glslangMeson = Join-Path $libplaceboRoot 'src\glsl\meson.build'
    $glslangText = Get-Content -LiteralPath $glslangMeson -Raw
    $glslangText = $glslangText.Replace(
        "cxx.find_library('glslang', required: required, static: static)",
        "cxx.find_library('glslang', required: required, static: static, dirs: vulkan_lib_dirs)")
    [IO.File]::WriteAllText($glslangMeson, $glslangText, [Text.UTF8Encoding]::new($false))

    $setupArguments = @(
        'setup',
        $buildDirectory,
        $buildSource,
        '--prefix', $prefix,
        '--libdir', 'lib',
        '--buildtype', 'release',
        '--default-library', 'shared',
        '--force-fallback-for', 'curl,expat,freetype2,fribidi,harfbuzz,libpng,luajit,zlib,xxhash,libass,libplacebo',
        # libplacebo looks for glslang under <vulkan-sdk>/lib and takes the
        # headers from the same prefix, so the shader tools are handed to it
        # as though they were an SDK. They are not one: the Vulkan headers it
        # uses are its own, under 3rdparty.
        "-Dlibplacebo:vulkan-sdk=$($shaderTools -replace '\\', '/')"
    ) + @(Get-MpvFeatureArguments -Platform windows -IncludeSubprojects)

    if (Test-Path (Join-Path $buildDirectory 'build.ninja')) {
        $setupArguments = @('setup', '--reconfigure') + $setupArguments[1..($setupArguments.Count - 1)]
    } elseif (Test-Path -LiteralPath $buildDirectory) {
        Remove-Item -LiteralPath $buildDirectory -Recurse -Force
    }
    # Meson detects its compiler from scratch, and picks MSVC when CC is unset
    # -- then looks for link.exe and finds Git's, which is not a linker. Say
    # what it is about to be given, so a failure here names its own cause.
    Write-Host "mpv toolchain: CC=$env:CC CXX=$env:CXX CC_LD=$env:CC_LD"
    foreach ($tool in @('clang', 'lld-link', 'link')) {
        $found = (Get-Command $tool -ErrorAction SilentlyContinue)
        Write-Host "  $tool -> $(if ($found) { $found.Source } else { '<missing>' })"
    }
    & meson @setupArguments
    if ($LASTEXITCODE -ne 0) { throw 'Configuring the Windows libmpv build failed.' }

    # Meson 1.9 records a fallback's per-subproject core option during the
    # initial setup but may still instantiate that first fallback as shared.
    # Reapply these after the fallbacks exist so libmpv has no extra project
    # DLLs and installation does not expect import libraries for their tools.
    meson configure $buildDirectory '-Dcurl:default_library=static' '-Dluajit:default_library=static'
    if ($LASTEXITCODE -ne 0) { throw 'Configuring static Windows libmpv dependencies failed.' }

    # Ninja's default process fan-out can exhaust Windows process creation
    # resources while compiling the large static FFmpeg closure.
    meson compile -C $buildDirectory --jobs 4
    if ($LASTEXITCODE -ne 0) { throw 'Building Windows libmpv failed.' }

    if (Test-Path -LiteralPath $prefix) {
        Remove-Item -LiteralPath $prefix -Recurse -Force
    }
    meson install -C $buildDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Installing Windows libmpv failed.' }
    Copy-Item (Join-Path $ffmpegPrefix 'bin\*.dll') (Join-Path $prefix 'bin')
    # libplacebo reaches SPIRV-Cross through its shared library, so that DLL is
    # part of libmpv's runtime closure and has to sit beside it for staging.
    Copy-Item (Join-Path $shaderTools 'bin\*.dll') (Join-Path $prefix 'bin')
    # mpv's own render_vk.h includes vulkan.h, and libplacebo keeps its copy of
    # the Vulkan headers under 3rdparty without handing them to whoever links
    # against it. The prefix is what the application compiles against, so the
    # headers travel with it -- that, and nothing else, is what decides whether
    # the application can compile its Vulkan path on Windows. No loader is
    # among them: this is not an SDK, and nothing here calls a Vulkan entry
    # point that the driver's own vulkan-1.dll does not supply at run time.
    foreach ($headers in @('vulkan', 'vk_video')) {
        $source = Join-Path $shaderTools "include\$headers"
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $prefix 'include') -Recurse -Force
        }
    }
    # Name the revision this prefix was built from, so build.ps1 can tell a
    # current libmpv from one the submodule has moved past.
    Write-MpvRevisionStamp -Directory $prefix -Revision $sourceRevision
} finally {
    Pop-Location
}

$mpvDll = Get-ChildItem (Join-Path $prefix 'bin') -Filter '*mpv*.dll' -File | Select-Object -First 1
$mpvLibrary = Get-ChildItem (Join-Path $prefix 'lib') -Filter 'mpv.lib' -File | Select-Object -First 1
if (-not $mpvDll -or -not $mpvLibrary) {
    throw "The source build did not produce the expected libmpv DLL and MSVC import library below $prefix."
}

Write-Host "Built Windows libmpv from $source"
Write-Host "DLL: $($mpvDll.FullName) ($([math]::Round($mpvDll.Length / 1MB, 2)) MiB)"
Write-Host "Import library: $($mpvLibrary.FullName)"
