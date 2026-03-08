# Ghost Client — release.ps1
# Usage: .\scripts\release.ps1 -Version v1.2.1
#
# 1. Tags and pushes the version
# 2. Waits for CI build
# 3. Edits release notes on GitHub
# 4. Previews Discord announcement and asks for approval before posting

param(
    [Parameter(Mandatory)][string]$Version,
    [string]$EnvFile = "$PSScriptRoot\..\discord.env"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Helpers ───────────────────────────────────────────────────────────────────
function Die($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

function Confirm-Step($prompt) {
    Write-Host "`n$prompt" -ForegroundColor Cyan
    $r = Read-Host "[y/N]"
    return ($r -match '^[Yy]')
}

# ── Load webhook URL ──────────────────────────────────────────────────────────
$webhookUrl = $null
if (Test-Path $EnvFile) {
    Get-Content $EnvFile | ForEach-Object {
        if ($_ -match '^DISCORD_WEBHOOK=(.+)$') { $webhookUrl = $Matches[1].Trim() }
    }
}
if (-not $webhookUrl) { Die "DISCORD_WEBHOOK not found in $EnvFile" }

# ── Verify clean working tree ─────────────────────────────────────────────────
$dirty = git status --porcelain 2>&1
if ($dirty) { Die "Working tree is dirty. Commit or stash changes first." }

# ── Tag and push ──────────────────────────────────────────────────────────────
Write-Host "`nTagging $Version..." -ForegroundColor Green
git tag $Version
git push origin master
git push origin $Version

# ── Wait for CI ───────────────────────────────────────────────────────────────
Write-Host "`nWaiting for GitHub Actions build..." -ForegroundColor Green
Start-Sleep -Seconds 8
$runId = (gh run list --limit 1 --json databaseId --jq ".[0].databaseId") 2>&1
Write-Host "Run ID: $runId"
gh run watch $runId --exit-status
if ($LASTEXITCODE -ne 0) { Die "CI build failed for run $runId" }
Write-Host "Build succeeded!" -ForegroundColor Green

# ── Collect changelog ─────────────────────────────────────────────────────────
# Auto-generate from commits since previous tag
$prevTag = (git tag --sort=-version:refname | Select-Object -Skip 1 -First 1)
$commits = git log "$prevTag..$Version" --pretty=format:"- %s" 2>&1
Write-Host "`nCommits since $prevTag`:`n$commits"

# ── Build release notes ───────────────────────────────────────────────────────
$notes = @"
## Ghost Client $Version

### Changes
$commits

Download: https://github.com/GetGhostClient/GhostClient/releases/tag/$Version
"@

# ── Update GitHub release ─────────────────────────────────────────────────────
$notes | Out-File -FilePath "$env:TEMP\relnotes.txt" -Encoding utf8
gh release edit $Version --title "Ghost Client $Version" --notes-file "$env:TEMP\relnotes.txt"
Write-Host "GitHub release updated." -ForegroundColor Green

# ── Build Discord embed ───────────────────────────────────────────────────────
$bulletLines = ($commits -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 8) -join "`n"

$discordMsg = @"
**Ghost Client $Version** is out!

**What's new:**
$bulletLines

**Download:** https://github.com/GetGhostClient/GhostClient/releases/tag/$Version
"@

# ── Preview and confirm ───────────────────────────────────────────────────────
Write-Host "`n============ DISCORD PREVIEW ============" -ForegroundColor Yellow
Write-Host $discordMsg
Write-Host "=========================================" -ForegroundColor Yellow

if (-not (Confirm-Step "Post this to Discord? (y to send, N to skip)")) {
    Write-Host "Discord post skipped." -ForegroundColor DarkGray
    exit 0
}

# ── Post to Discord webhook ───────────────────────────────────────────────────
$payload = @{
    content  = $discordMsg
    username = "Ghost Client"
} | ConvertTo-Json -Compress

$resp = Invoke-RestMethod -Uri $webhookUrl -Method Post `
    -ContentType "application/json" -Body $payload
Write-Host "Discord announcement posted!" -ForegroundColor Green
