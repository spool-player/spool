param([switch] $Clean)

. (Join-Path $PSScriptRoot 'common.ps1')
Import-MsvcEnvironment
$root = Get-RepositoryRoot
$deps = Join-Path $root 'build\windows-deps'
$pin = (Get-ToolchainManifest).ffmpeg
$source = Join-Path $deps "ffmpeg-$($pin.version)"
$archive = Join-Path $deps "ffmpeg-$($pin.version).tar.xz"
$headers = Join-Path $deps 'nv-codec-headers'
$prefix = Join-Path $deps 'ffmpeg'
$build = Join-Path $deps 'ffmpeg-build'
$msysRoot = Get-Msys2Root
$bash = Join-Path $msysRoot 'usr\bin\bash.exe'
$stamp = Join-Path $prefix '.spool-ffmpeg-inputs'
$identity = (@(
    $PSCommandPath, (Join-Path $PSScriptRoot 'build-ffmpeg.sh'),
    (Join-Path $PSScriptRoot 'common.ps1'), (Join-Path $PSScriptRoot 'extract-archive.py'),
    (Join-Path $PSScriptRoot 'check-ffmpeg.py'),
    (Join-Path $root 'tools\manifests\toolchain.json'),
    (Join-Path $root 'tools\manifests\ffmpeg-capabilities.json'),
    (Join-Path $root 'tools\ffmpeg-capabilities.py'),
    (Get-Command cl.exe -ErrorAction Stop).Source,
    (Get-Command nasm.exe -ErrorAction Stop).Source
) | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }) -join "`n"
if (-not $Clean -and (Test-Path -LiteralPath $stamp) -and
    (Get-Content -LiteralPath $stamp -Raw) -eq $identity) {
    Write-Host "FFmpeg build inputs unchanged; reusing $prefix"
    Initialize-WindowsMpvBuildEnvironment
    $env:PKG_CONFIG = Join-Path $msysRoot 'usr\bin\pkgconf.exe'
    return
}
New-Item -ItemType Directory -Force $deps | Out-Null
if (-not (Test-Path $archive)) { Invoke-WebRequest $pin.url -OutFile $archive }
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ine $pin.sha256) {
    throw "FFmpeg source checksum mismatch: $archive"
}
if (-not (Test-Path $source)) {
    Expand-WindowsSourceArchive -Archive $archive -Destination $deps
}
if (-not (Test-Path $headers)) {
    git clone https://github.com/FFmpeg/nv-codec-headers.git $headers
    if ($LASTEXITCODE -ne 0) { throw 'Fetching NVIDIA decode headers failed.' }
}
git -C $headers checkout --detach e844e5b26f46bb77479f063029595293aa8f812d
if ($LASTEXITCODE -ne 0) { throw 'Checking out NVIDIA decode headers failed.' }
if ($Clean) {
    foreach ($path in @($build, $prefix)) {
        if (Test-Path $path) { Remove-Item $path -Recurse -Force }
    }
}
New-Item -ItemType Directory -Force $build | Out-Null
$flags = Join-Path $build 'configure-flags.txt'
& python (Join-Path $root 'tools\ffmpeg-capabilities.py') configure --platform windows |
    Set-Content $flags -Encoding utf8NoBOM
if ($LASTEXITCODE -ne 0) { throw 'Generating FFmpeg capabilities failed.' }
$env:SPOOL_MSVC_BIN = Split-Path (Get-Command cl.exe).Source
$env:SPOOL_NASM_BIN = Split-Path (Get-Command nasm.exe).Source
& $bash (Join-Path $PSScriptRoot 'build-ffmpeg.sh').Replace('\', '/') `
    $source $headers $prefix $build
if ($LASTEXITCODE -ne 0) { throw 'Building upstream Windows FFmpeg failed.' }
& python (Join-Path $root 'tools\ffmpeg-capabilities.py') audit-components --platform windows `
    (Join-Path $build 'config_components.h')
if ($LASTEXITCODE -ne 0) { throw 'Windows FFmpeg capabilities failed verification.' }
& python (Join-Path $PSScriptRoot 'check-ffmpeg.py') $prefix
if ($LASTEXITCODE -ne 0) { throw 'Windows FFmpeg runtime failed verification.' }
[IO.File]::WriteAllText($stamp, $identity, [Text.UTF8Encoding]::new($false))
# Ensure subsequent Meson builds use the existing clang/MSVC-compatible toolchain.
Initialize-WindowsMpvBuildEnvironment
$env:PKG_CONFIG = Join-Path $msysRoot 'usr\bin\pkgconf.exe'
