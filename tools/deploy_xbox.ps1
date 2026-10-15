param(
    [string]$ConsoleEndpoint = 'https://192.168.1.67:11443',
    [string]$PackagePath,
    [string]$Username,
    [string]$Password,
    [string]$Repository = 'nickarcade/OpenCE-UWP-XB1',
    [string]$Token = $env:GH_TOKEN,
    [string]$RunId
)

$ErrorActionPreference = 'Stop'

Write-Host "=== Xbox Dev Mode Deployment ===" -ForegroundColor Cyan
Write-Host "Target Endpoint: $ConsoleEndpoint"

# Check if target is reachable
Write-Host "Testing connectivity to Xbox Device Portal..."
$handler = [System.Net.Http.HttpClientHandler]::new()
$handler.ServerCertificateCustomValidationCallback = { $true }
if ($Username -and $Password) {
    $credentials = [System.Net.NetworkCredential]::new($Username, $Password)
    $handler.Credentials = $credentials
}
$client = [System.Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(30)

try {
    $osInfoUrl = "$($ConsoleEndpoint.TrimEnd('/'))/api/os/info"
    $infoResponse = $client.GetAsync($osInfoUrl).GetAwaiter().GetResult()
    if ($infoResponse.IsSuccessStatusCode) {
        $infoJson = $infoResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        Write-Host "Connected to Xbox Device Portal successfully: $infoJson" -ForegroundColor Green
    } else {
        Write-Warning "Console responded with HTTP $($infoResponse.StatusCode) on /api/os/info."
    }
} catch {
    Write-Warning "Could not query /api/os/info: $($_.Exception.Message)"
}

# Resolve package if not provided
$tempDir = $null
if (-not $PackagePath -or -not (Test-Path -LiteralPath $PackagePath)) {
    Write-Host "PackagePath not specified or not found. Fetching latest artifact from GitHub..."
    $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("xbox-deploy-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

    $downloaded = $false
    # Try using gh CLI
    if (Get-Command gh -ErrorAction SilentlyContinue) {
        Write-Host "Using GitHub CLI (gh) to fetch latest workflow artifact..."
        try {
            $runArg = if ($RunId) { $RunId } else {
                $latestRun = gh run list --repo $Repository --workflow uwp-auto.yml --status success --limit 1 --json databaseId | ConvertFrom-Json
                if ($latestRun -and $latestRun.databaseId) { $latestRun.databaseId } else { $null }
            }
            if ($runArg) {
                Write-Host "Downloading artifact from workflow run $runArg..."
                gh run download $runArg --repo $Repository --dir $tempDir
                $downloaded = $true
            }
        } catch {
            Write-Warning "gh run download failed: $($_.Exception.Message)"
        }
    }

    if (-not $downloaded -and $Token) {
        Write-Host "Using GitHub REST API to fetch latest artifact..."
        $headers = @{
            'Accept' = 'application/vnd.github+json'
            'Authorization' = "Bearer $Token"
            'X-GitHub-Api-Version' = '2022-11-28'
        }
        $artifactsUrl = "https://api.github.com/repos/$Repository/actions/artifacts?per_page=5"
        $resp = Invoke-RestMethod -Uri $artifactsUrl -Headers $headers -SkipCertificateCheck
        if ($resp.artifacts -and $resp.artifacts.Count -gt 0) {
            $artifact = $resp.artifacts[0]
            $zipPath = Join-Path $tempDir "$($artifact.name).zip"
            Write-Host "Downloading $($artifact.name) ($($artifact.size_in_bytes) bytes)..."
            Invoke-WebRequest -Uri $artifact.archive_download_url -Headers $headers -OutFile $zipPath -SkipCertificateCheck
            Expand-Archive -Path $zipPath -DestinationPath $tempDir -Force
            $downloaded = $true
        }
    }

    if (-not $downloaded) {
        throw "Could not retrieve artifact. Please specify -PackagePath directly or ensure gh CLI/GH_TOKEN is configured."
    }
    $PackagePath = $tempDir
}

# Locate primary .appx and dependencies
$appxFile = $null
$dependencyFiles = @()

if ((Test-Path -LiteralPath $PackagePath) -and (Get-Item -LiteralPath $PackagePath) -is [System.IO.FileInfo] -and $PackagePath.EndsWith('.zip')) {
    $zipExtract = Join-Path ([System.IO.Path]::GetTempPath()) ("xbox-zip-" + [Guid]::NewGuid().ToString('N'))
    Expand-Archive -Path $PackagePath -DestinationPath $zipExtract -Force
    $PackagePath = $zipExtract
}

if ((Test-Path -LiteralPath $PackagePath) -and (Get-Item -LiteralPath $PackagePath) -is [System.IO.FileInfo] -and $PackagePath.EndsWith('.appx')) {
    $appxFile = Get-Item -LiteralPath $PackagePath
    $parentDir = $appxFile.DirectoryName
    $depDir = Join-Path $parentDir 'Dependencies'
    if (Test-Path -LiteralPath $depDir) {
        $dependencyFiles = Get-ChildItem -LiteralPath $depDir -Recurse -Filter '*.appx'
    }
} else {
    $appxCandidates = Get-ChildItem -LiteralPath $PackagePath -Recurse -Filter '*.appx' | Where-Object { $_.FullName -notmatch '[\\/]Dependencies[\\/]' }
    if ($appxCandidates.Count -gt 0) {
        $appxFile = $appxCandidates[0]
    }
    $dependencyFiles = Get-ChildItem -LiteralPath $PackagePath -Recurse -Filter '*.appx' | Where-Object { $_.FullName -match '[\\/]Dependencies[\\/]' }
}

if (-not $appxFile) {
    throw "No .appx package found in $PackagePath."
}

Write-Host "Primary AppX: $($appxFile.FullName)" -ForegroundColor Green
if ($dependencyFiles.Count -gt 0) {
    Write-Host "Dependencies found ($($dependencyFiles.Count)):"
    foreach ($dep in $dependencyFiles) {
        Write-Host "  - $($dep.Name)"
    }
}

# Construct multipart upload request
$uploadUrl = "$($ConsoleEndpoint.TrimEnd('/'))/api/app/packagemanager/package?package=$([System.Uri]::EscapeDataString($appxFile.Name))"
Write-Host "Deploying to Xbox Device Portal: $uploadUrl..." -ForegroundColor Cyan

$content = [System.Net.Http.MultipartFormDataContent]::new()

# Add primary package
$packageStream = [System.IO.File]::OpenRead($appxFile.FullName)
$packageContent = [System.Net.Http.StreamContent]::new($packageStream)
$packageContent.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse('application/octet-stream')
$content.Add($packageContent, "file", $appxFile.Name)

# Add dependencies
$openedDepStreams = @()
foreach ($dep in $dependencyFiles) {
    $depStream = [System.IO.File]::OpenRead($dep.FullName)
    $openedDepStreams += $depStream
    $depContent = [System.Net.Http.StreamContent]::new($depStream)
    $depContent.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse('application/octet-stream')
    $content.Add($depContent, "file", $dep.Name)
}

try {
    $uploadClient = [System.Net.Http.HttpClient]::new($handler)
    $uploadClient.Timeout = [TimeSpan]::FromMinutes(10)
    $response = $uploadClient.PostAsync($uploadUrl, $content).GetAwaiter().GetResult()
    
    if (-not $response.IsSuccessStatusCode) {
        $errorBody = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        throw "Xbox installation rejected: HTTP $($response.StatusCode) - $errorBody"
    }
    
    Write-Host "Deployment request accepted (HTTP $($response.StatusCode)). Waiting for installation to complete..." -ForegroundColor Green
    
    # Poll status
    $stateUrl = "$($ConsoleEndpoint.TrimEnd('/'))/api/app/packagemanager/state"
    $maxWait = 60
    while ($maxWait -gt 0) {
        Start-Sleep -Seconds 2
        try {
            $stateResponse = $uploadClient.GetAsync($stateUrl).GetAwaiter().GetResult()
            if ($stateResponse.IsSuccessStatusCode) {
                $stateText = $stateResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
                Write-Host "State: $stateText"
                if ($stateText -match '"Status":\s*0' -or $stateText -match '"Completed":\s*true' -or $stateText -match '"Success":\s*true') {
                    Write-Host "Installation completed successfully on Xbox One!" -ForegroundColor Green
                    break
                }
            }
        } catch {
            # Continue polling
        }
        $maxWait--
    }
    Write-Host "Deployment finished successfully." -ForegroundColor Green
} finally {
    $packageStream.Dispose()
    foreach ($s in $openedDepStreams) { $s.Dispose() }
    $content.Dispose()
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
