# Ghost Client — release.ps1
# Usage: .\scripts\release.ps1 -Version v1.2.2
#
# 1. Tags and pushes the version
# 2. Waits for CI build to complete
# 3. Updates GitHub release notes (with logo embed)
# 4. Shows Discord announcement preview, asks for approval, then posts

param(
    [Parameter(Mandatory)][string]$Version,
    [string]$EnvFile = "$PSScriptRoot\..\discord.env",
    [switch]$Confirm  # when set, skip Discord prompt and post automatically
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$REPO        = "GetGhostClient/GhostClient"
$LOGO_URL    = "https://raw.githubusercontent.com/$REPO/master/assets/logo.png"
$RELEASE_URL = "https://github.com/$REPO/releases/tag/$Version"

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
Write-Host "`nTagging $Version and pushing..." -ForegroundColor Green
git tag $Version
git push origin master
git push origin $Version

# ── Wait for CI ───────────────────────────────────────────────────────────────
Write-Host "`nWaiting for GitHub Actions build..." -ForegroundColor Green
Start-Sleep -Seconds 10
$runId = gh run list --limit 1 --json databaseId --jq ".[0].databaseId"
Write-Host "Run ID: $runId"
gh run watch $runId --exit-status
if ($LASTEXITCODE -ne 0) { Die "CI build failed for run $runId" }
Write-Host "Build succeeded!" -ForegroundColor Green

# ── Collect changelog ─────────────────────────────────────────────────────────
$prevTag = git tag --sort=-version:refname | Select-Object -Skip 1 -First 1
$rawCommits = git log "$prevTag..$Version" --pretty=format:"- %s"
Write-Host "`nCommits since $prevTag`:`n$rawCommits"

# Strip chore/ci/docs commits for the public-facing list
$publicCommits = $rawCommits -split "`n" |
    Where-Object { $_ -notmatch '^\s*-\s*(chore|ci|docs|style|refactor):' } |
    Select-Object -First 10

$bulletsMd   = $publicCommits -join "`n"              # for GitHub (markdown)
$bulletsPlain = ($publicCommits | ForEach-Object {     # for Discord (no leading -)
    $_ -replace '^\s*-\s*feat:\s*', '' `
       -replace '^\s*-\s*fix:\s*', 'Fix: ' `
       -replace '^\s*-\s*', ''
}) -join "`n"

# ── Update GitHub release notes ───────────────────────────────────────────────
$ghNotes = @"
<p align="center">
  <img src="$LOGO_URL" width="120" alt="Ghost Client logo"/>
</p>

## Ghost Client $Version

> Runtime FFlag editor for Roblox — no injection, no detection.

### What's new
$bulletsMd

---
**Download** ghostclient.exe below and run as Administrator.  
Offsets fetched automatically from [imtheo.lol/Offsets](https://imtheo.lol/Offsets).

*Built on Windows · DirectX 11 · ImGui · Wanted Sans*
"@

$ghNotes | Out-File "$env:TEMP\relnotes.txt" -Encoding utf8
gh release edit $Version --title "Ghost Client $Version" --notes-file "$env:TEMP\relnotes.txt"
Write-Host "GitHub release notes updated." -ForegroundColor Green

# ── Build Discord embed (rich embed with thumbnail) ───────────────────────────
# Discord webhook supports embeds via JSON
$embedFields = @()
if ($bulletsPlain) {
    $embedFields += @{
        name   = "What's new"
        value  = $bulletsPlain
        inline = $false
    }
}

$embed = @{
    title       = "Ghost Client $Version"
    description = "Runtime FFlag editor for Roblox."
    url         = $RELEASE_URL
    color       = 0x2b2d31   # dark grey matching app theme
    thumbnail   = @{ url = $LOGO_URL }
    fields      = $embedFields
    footer      = @{ text = "Offsets: imtheo.lol/Offsets" }
}

$payload = @{
    username   = "Ghost Client"
    avatar_url = $LOGO_URL
    content    = ""
    embeds     = @($embed)
}

# ── Preview ───────────────────────────────────────────────────────────────────
Write-Host "`n============ DISCORD EMBED PREVIEW ============" -ForegroundColor Yellow
Write-Host "Title   : Ghost Client $Version"
Write-Host "URL     : $RELEASE_URL"
Write-Host "Thumb   : $LOGO_URL"
Write-Host "Fields  :"
Write-Host $bulletsPlain
Write-Host "===============================================" -ForegroundColor Yellow

if (-not $Confirm -and -not (Confirm-Step "Post this to Discord? [y/N]")) {
    Write-Host "Discord post skipped." -ForegroundColor DarkGray
    exit 0
}

# ── Post ──────────────────────────────────────────────────────────────────────
$json = $payload | ConvertTo-Json -Depth 6 -Compress
Invoke-RestMethod -Uri $webhookUrl -Method Post `
    -ContentType "application/json" -Body $json | Out-Null
Write-Host "Discord announcement posted!" -ForegroundColor Green
