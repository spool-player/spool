param(
    [int] $Workers = [Math]::Min(32, [Environment]::ProcessorCount),
    [switch] $Resume,
    [switch] $RetryFailed
)
. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsBuildEnvironment
$root = Get-RepositoryRoot
$pin = Get-Content -LiteralPath (Join-Path $root 'tools\manifests\windows-test-runtime.json') -Raw | ConvertFrom-Json
$mesa = Join-Path $root "build\windows-test-runtime\mesa-$($pin.mesa.version)\x64"
foreach ($file in @('opengl32.dll', 'libgallium_wgl.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $mesa $file))) {
        throw 'Pinned Mesa test drivers are missing. Run tools\windows\install-test-runtime.ps1.'
    }
}
$tesseract = Join-Path $env:ProgramFiles 'Tesseract-OCR'
$env:PATH = "$mesa;$tesseract;$(Join-Path $env:SPOOL_MPV_ROOT 'bin');$(Join-Path $env:SPOOL_QT_ROOT 'bin');$env:PATH"
foreach ($tool in @('ffmpeg.exe', 'tesseract.exe', 'python.exe')) {
    Get-Command $tool -ErrorAction Stop | Out-Null
}
# Qt's dynamic OpenGL loader must load the pinned WGL driver, not the system's
# OpenGL 1.1 implementation. This is real OpenGL shared with libmpv, not Qt's
# software scenegraph backend, D3D WARP, or a graphics-availability skip.
$env:QT_OPENGL = 'software'
$env:QT_OPENGL_DLL = Join-Path $mesa 'opengl32.dll'
$env:GALLIUM_DRIVER = 'llvmpipe'
$env:LIBGL_ALWAYS_SOFTWARE = '1'
$env:QSG_RHI_BACKEND = 'opengl'
$env:SPOOL_RENDER_API = 'opengl'
$env:SPOOL_TEST_RENDER_BACKEND = 'opengl'
$env:QT_QPA_PLATFORM = 'windows'
$env:QSG_INFO = '1'
Remove-Item Env:QT_QUICK_BACKEND, Env:QSG_RHI_PREFER_SOFTWARE_RENDERER -ErrorAction SilentlyContinue
$arguments = @((Join-Path $root 'tools\run-tests.py'), '--build-dir',
    (Join-Path $root 'build\windows-release\app'), '--workers', "$Workers")
if ($Resume) { $arguments += '--resume' }
if ($RetryFailed) { $arguments += '--retry-failed' }
& python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Windows traditional and real OpenGL GUI e2e tests failed.' }
