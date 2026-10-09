param(
    [string]$UwpDeps = 'C:\bsuwp\_deps\uwpdep-src',
    [int]$Jobs = 12
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'upstream'
if (-not (Test-Path -LiteralPath (Join-Path $source 'configure.py'))) {
    throw 'Run tools\setup_upstream.ps1 first.'
}
if (-not (Test-Path -LiteralPath $UwpDeps)) {
    throw "UWP dependency directory was not found: $UwpDeps"
}

$gallium = Join-Path $UwpDeps 'x64\bin\libgallium_wgl.dll'
if (Test-Path -LiteralPath $gallium) {
    $bytes = [IO.File]::ReadAllBytes($gallium)
    if ($bytes.Length -gt 0x005badd6 -and $bytes[0x005badd5] -eq 0x73 -and $bytes[0x005badd6] -eq 0x50) {
        $bytes[0x005badd5] = 0xeb
        [IO.File]::WriteAllBytes($gallium, $bytes)
        Write-Output "Patched $gallium for Xbox One Shader Model 6.0 support."
    }
}

Push-Location $source
try {
    & py -3 configure.py --release --pgo=off
    if ($LASTEXITCODE) { throw 'Guest configuration failed.' }
    & ninja xbox_guest
    if ($LASTEXITCODE) { throw 'Guest build failed.' }
} finally {
    Pop-Location
}

$msvc = Join-Path $PSScriptRoot 'with-msvc.ps1'
& $msvc cmake --fresh -S (Join-Path $source 'port\xbox') `
    -B (Join-Path $source 'build\xbox-uwp') -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl `
    -DCMAKE_CXX_COMPILER=clang-cl "-DUWP_DEPS=$UwpDeps"
if ($LASTEXITCODE) { throw 'Host configuration failed.' }
& $msvc cmake --build (Join-Path $source 'build\xbox-uwp') -j $Jobs
if ($LASTEXITCODE) { throw 'Host build failed.' }
