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
    $patched = $false

    # Patch 1: Fallback to SM 6.0 when CheckFeatureSupport fails in d3d12_screen_create (offset 0x0057c6b3: eb 1a -> 90 90)
    if ($bytes.Length -gt 0x0057c6b4 -and $bytes[0x0057c6b3] -eq 0xeb -and $bytes[0x0057c6b4] -eq 0x1a) {
        $bytes[0x0057c6b3] = 0x90
        $bytes[0x0057c6b4] = 0x90
        $patched = $true
        Write-Output "Applied Patch 1: SM query fallback to SM6.0 at 0x0057c6b3."
    }

    # Patch 2: Enforce SM 6.0 in nir_to_dxil options check (offset 0x005bfa48)
    if ($bytes.Length -gt (0x005bfa48 + 26) -and $bytes[0x005bfa48] -eq 0x41 -and $bytes[0x005bfa49] -eq 0x8b) {
        $bytes[0x005bfa48] = 0xb8
        $bytes[0x005bfa49] = 0x00
        $bytes[0x005bfa4a] = 0x00
        $bytes[0x005bfa4b] = 0x06
        $bytes[0x005bfa4c] = 0x00
        $bytes[0x005bfa4d] = 0x41
        $bytes[0x005bfa4e] = 0x89
        $bytes[0x005bfa4f] = 0x47
        $bytes[0x005bfa50] = 0x14
        for ($i = 0; $i -lt 17; $i++) {
            $bytes[0x005bfa51 + $i] = 0x90
        }
        $patched = $true
        Write-Output "Applied Patch 2: Enforce SM6.0 in nir_to_dxil at 0x005bfa48."
    }

    # Patch 3: Bypass SM 6.2 requirement for 16-bit types in emit_module (offset 0x005badd5: 73 50 -> eb 50)
    if ($bytes.Length -gt 0x005badd6 -and $bytes[0x005badd5] -eq 0x73 -and $bytes[0x005badd6] -eq 0x50) {
        $bytes[0x005badd5] = 0xeb
        $patched = $true
        Write-Output "Applied Patch 3: Bypass 16-bit SM6.2 check in emit_module at 0x005badd5."
    }

    if ($patched) {
        [IO.File]::WriteAllBytes($gallium, $bytes)
        Write-Output "Successfully wrote patched $gallium for Xbox One SM6.0."
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
