param(
    [switch]$Clean,
    [string[]]$CMakeArguments = @()
)

. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsBuildEnvironment
$root = Get-RepositoryRoot
$buildDir = Join-Path $root 'build\windows-release\app'
if (-not (Test-MpvBuildCurrent -Prefix $env:SPOOL_MPV_ROOT)) {
    # libmpv is built by clang and lld with a PATH, CC and CXX of its own; the
    # application is built by MSVC. Run it in a child process so that
    # environment cannot outlive it: in-process it did, and the resource
    # compiler the app links with went missing behind LLVM's own tools
    # (LNK1158: cannot run 'rc.exe'). The runner never saw this because there
    # the two are separate steps, which is what this reproduces locally.
    & (Get-Process -Id $PID).Path -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot 'build-mpv.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Building the Windows libmpv dependency failed.' }
}

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

Push-Location $root
try {
    cmake --preset windows-release @CMakeArguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    $ninjaCommands = Get-Content -LiteralPath (Join-Path $buildDir 'build.ninja') -Raw
    foreach ($requiredFlag in @('/GL', '/LTCG', '/OPT:REF', '/OPT:ICF')) {
        if (-not $ninjaCommands.Contains($requiredFlag, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Generated Windows Release commands are missing $requiredFlag"
        }
    }
    # Build every target, tests included, the way the Linux and macOS scripts
    # do. The application's whole-program-optimized link is a long serial step;
    # letting Ninja compile the tests alongside it keeps the runner's cores busy
    # instead of paying for the tests in a second pass.
    cmake --build --preset windows-release
    if ($LASTEXITCODE -ne 0) { throw 'Windows release build failed.' }
} finally {
    Pop-Location
}
