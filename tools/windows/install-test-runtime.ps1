. (Join-Path $PSScriptRoot 'common.ps1')
$root = Get-RepositoryRoot
$pin = Get-Content -LiteralPath (Join-Path $root 'tools\manifests\windows-test-runtime.json') -Raw | ConvertFrom-Json
$runtime = Join-Path $root 'build\windows-test-runtime'
New-Item -ItemType Directory -Force -Path $runtime | Out-Null
$archive = Join-Path $runtime "mesa-$($pin.mesa.version).7z"
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri $pin.mesa.url -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.mesa.sha256) {
    throw 'The pinned Mesa test driver archive failed SHA-256 verification.'
}
$mesa = Join-Path $runtime "mesa-$($pin.mesa.version)"
$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path -LiteralPath $sevenZip)) {
    $sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
}
& $sevenZip x -y "-o$mesa" $archive | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Extracting the pinned Mesa test driver failed.' }
foreach ($file in @('opengl32.dll', 'libgallium_wgl.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $mesa "x64\$file"))) {
        throw "The pinned Mesa x64 test runtime is missing $file."
    }
}
# Chocolatey packages contain the upstream installer/archive digest. Pin the
# package version as well; never use the runner's changing latest versions.
# FFmpeg's version comes from the common toolchain pin, not another copy here.
foreach ($package in @(
    @{ Name = $pin.tesseract.package; Version = $pin.tesseract.version },
    @{ Name = 'ffmpeg'; Version = (Get-ToolchainManifest).ffmpeg.version }
)) {
    & choco install $package.Name "--version=$($package.Version)" --allow-downgrade -y --no-progress
    if ($LASTEXITCODE -ne 0) { throw "Installing pinned $($package.Name) test dependency failed." }
}
Write-Host "Installed pinned Mesa $($pin.mesa.version) llvmpipe for real Qt/libmpv OpenGL tests."
