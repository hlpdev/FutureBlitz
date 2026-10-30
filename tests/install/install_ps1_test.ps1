# install.ps1 on releases made of this build's fb, into a FUTUREBLITZ_HOME with a space, leaving
# PATH alone, and downloads from a folder of fake GitHub Releases, which a function in place of
# Invoke-WebRequest serves: install_ps1_test.ps1 <install.ps1> <output folder> <fb.exe>.
param([string]$Script, [string]$Out, [string]$Fb)

$ErrorActionPreference = 'Stop'
$shell = (Get-Process -Id $PID).Path
# ctest gives paths with forward slashes, which the scripts' messages don't keep.
$Out = [IO.Path]::GetFullPath($Out)
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
New-Item -ItemType Directory -Path "$Out\archives" | Out-Null

function Fail([string]$message) {
    Write-Host "FAILED: $message"
    exit 1
}

# A release for `target` named `name`, whose fb is this build's, or a text file when it's broken.
function New-Release([string]$name, [string]$target, [switch]$Broken) {
    $folder = "$Out\archives\$name\futureblitz-0.1.0-$target"
    New-Item -ItemType Directory -Path "$folder\bin", "$folder\std" | Out-Null
    if ($Broken) {
        Set-Content -LiteralPath "$folder\bin\fb.exe" -Value 'not a program'
    } else {
        Copy-Item -LiteralPath $Fb -Destination "$folder\bin"
        Get-ChildItem -LiteralPath (Split-Path $Fb) -Filter *.dll |
            Copy-Item -Destination "$folder\bin"
    }
    Set-Content -LiteralPath "$folder\std\which.txt" -Value $name
    Compress-Archive -Path $folder -DestinationPath "$Out\archives\$name.zip"
}
New-Release good windows-x64
New-Release newer windows-x64
New-Release other linux-x64
New-Release broken windows-x64 -Broken

$env:FUTUREBLITZ_HOME = "$Out\home ps\fb"
$which = "$env:FUTUREBLITZ_HOME\std\which.txt"

# Runs install.ps1 in a shell of its own, and gives its output and exit code.
function Invoke-Install([string]$archive) {
    $output = & $shell -NoProfile -ExecutionPolicy Bypass -File $Script -Archive $archive `
        -NoModifyPath 2>&1 | Out-String
    return @{ Output = $output; Code = $LASTEXITCODE }
}

# -NoModifyPath says how to put fb on PATH, not that a new terminal has it.
$first = Invoke-Install "$Out\archives\good.zip"
if ($first.Code -ne 0) { Fail "install.ps1 failed: $($first.Output)" }
if ((Get-Content -LiteralPath $which) -ne 'good') { Fail "the release isn't installed" }
if (-not $first.Output.Contains("Add $env:FUTUREBLITZ_HOME\bin to PATH to use fb.") -or
    $first.Output.Contains('Open a new terminal')) {
    Fail "-NoModifyPath said: $($first.Output)"
}

# Running it again updates.
$update = Invoke-Install "$Out\archives\newer.zip"
if ($update.Code -ne 0 -or (Get-Content -LiteralPath $which) -ne 'newer') {
    Fail "the update failed: $($update.Output)"
}

# Another machine's release, or one whose fb doesn't run, leaves the one that's there.
foreach ($bad in @(
        @{ Name = 'other'; Says = 'is a release for another machine, not windows-x64' },
        @{ Name = 'broken'; Says = "fb doesn't run here" })) {
    $refused = Invoke-Install "$Out\archives\$($bad.Name).zip"
    if ($refused.Code -eq 0 -or -not $refused.Output.Contains($bad.Says)) {
        Fail "the $($bad.Name) archive gave: $($refused.Output)"
    }
    if ((Get-Content -LiteralPath $which) -ne 'newer') { Fail "the $($bad.Name) archive replaced it" }
}

# Windows PowerShell's irm reads a script served with no charset as Latin-1, so it's ASCII.
$bytes = [IO.File]::ReadAllBytes($Script)
if (@($bytes | Where-Object { $_ -gt 127 }).Count -ne 0) { Fail "install.ps1 isn't ASCII" }

# Run in the caller's session, as `irm | iex` runs it, dot-sourced and by iex with arguments, it
# leaves the session as it was, its own variables named like the script's parameters too.
$probe = @"
`$ErrorActionPreference = 'Continue'
`$ProgressPreference = 'Continue'
`$Version = 'mine'
`$Archive = 'kept'
`$NoModifyPath = 'too'
`$before = [Net.ServicePointManager]::SecurityProtocol
. '$Script' -Archive '$Out\archives\good.zip' -NoModifyPath | Out-Null
`$dotted = Get-Content -LiteralPath '$which'
Invoke-Expression "& { `$(Get-Content -Raw -LiteralPath '$Script') } -Archive '$Out\archives\newer.zip' -NoModifyPath" | Out-Null
`$piped = Get-Content -LiteralPath '$which'
`$left = @(Get-Variable root, repository, work, bin -ErrorAction SilentlyContinue).Count
`$left += @(Get-Command Install-FutureBlitz -ErrorAction SilentlyContinue).Count
"`$ErrorActionPreference `$ProgressPreference `$([Net.ServicePointManager]::SecurityProtocol -eq `$before) `$left `$Version `$Archive `$NoModifyPath `$dotted `$piped"
"@
Set-Content -LiteralPath "$Out\probe.ps1" -Value $probe
$session = & $shell -NoProfile -ExecutionPolicy Bypass -File "$Out\probe.ps1" | Select-Object -Last 1
if ($session -ne 'Continue Continue True 0 mine kept too good newer') {
    Fail "the session was left with: $session"
}

# m9-ops-6: the latest release is the one releases/latest/download/'s SHA256SUMS is of, with no
# call to GitHub's API, and m9-ops-5: its archive is installed once it's that file's hash. The
# script runs from one whose Invoke-WebRequest, a function, comes before the cmdlet, and gives
# FAKE_RELEASES' file for a URL, as GitHub would, or fails as a 404 does.
$releases = "$Out\releases"
function New-Download([string]$version, [string]$archive, [string]$hashOf, [switch]$NoArchive) {
    $folder = "$releases\v$version"
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $name = "futureblitz-$version-windows-x64.zip"
    $lines = @()
    if (-not $NoArchive) {
        Copy-Item -LiteralPath "$Out\archives\$archive.zip" -Destination "$folder\$name"
        $hash = (Get-FileHash -LiteralPath "$Out\archives\$hashOf.zip" -Algorithm SHA256).Hash
        $lines += "$($hash.ToLowerInvariant())  $name"
    }
    $other = (Get-FileHash -LiteralPath $Script -Algorithm SHA256).Hash.ToLowerInvariant()
    $lines += "$other  install.ps1"
    Set-Content -LiteralPath "$folder\SHA256SUMS" -Value $lines -Encoding ascii
}
New-Download 0.1.0 good good
New-Item -ItemType Directory -Path "$releases\latest" | Out-Null
Copy-Item -LiteralPath "$releases\v0.1.0\SHA256SUMS" -Destination "$releases\latest"
# 0.1.1's archive isn't the one its SHA256SUMS names, and 0.1.2 has none for windows-x64.
New-Download 0.1.1 newer good
New-Download 0.1.2 -NoArchive

$downloads = "$Out\downloads.log"
$wrapper = @"
function Invoke-WebRequest {
    param([Parameter(Position = 0)][string]`$Uri, [string]`$OutFile, [switch]`$UseBasicParsing)
    Add-Content -LiteralPath '$downloads' -Value `$Uri
    `$base = 'https://github.com/hlpdev/FutureBlitz/releases/'
    `$file = `$null
    if (`$Uri.StartsWith(`$base + 'latest/download/')) {
        `$file = 'latest\' + `$Uri.Split('/')[-1]
    } elseif (`$Uri.StartsWith(`$base + 'download/')) {
        `$file = `$Uri.Substring((`$base + 'download/').Length).Replace('/', '\')
    }
    if (-not `$file -or -not (Test-Path -LiteralPath "$releases\`$file")) {
        throw "The remote server returned an error: (404) Not Found."
    }
    Copy-Item -LiteralPath "$releases\`$file" -Destination `$OutFile
}
& '$Script' @args
# A script that exits ends only itself, so its code is the wrapper's too.
exit `$LASTEXITCODE
"@
Set-Content -LiteralPath "$Out\download.ps1" -Value $wrapper
$env:FUTUREBLITZ_HOME = "$Out\home dl\fb"
$which = "$env:FUTUREBLITZ_HOME\std\which.txt"
function Invoke-Download([string[]]$arguments) {
    $output = & $shell -NoProfile -ExecutionPolicy Bypass -File "$Out\download.ps1" @arguments `
        -NoModifyPath 2>&1 | Out-String
    return @{ Output = $output; Code = $LASTEXITCODE }
}

$latest = Invoke-Download @()
if ($latest.Code -ne 0 -or (Get-Content -LiteralPath $which) -ne 'good' -or
    -not $latest.Output.Contains('Downloading FutureBlitz 0.1.0 for windows-x64')) {
    Fail "the latest release wasn't installed: $($latest.Output)"
}
$asked = Get-Content -LiteralPath $downloads
if (@($asked | Where-Object { $_.Contains('api.github.com') }).Count -ne 0 -or
    $asked[0] -ne 'https://github.com/hlpdev/FutureBlitz/releases/latest/download/SHA256SUMS' -or
    $asked[1] -ne ('https://github.com/hlpdev/FutureBlitz/releases/download/v0.1.0/' +
        'futureblitz-0.1.0-windows-x64.zip')) {
    Fail "the downloads were: $($asked -join ', ')"
}

# An archive whose hash isn't its SHA256SUMS', or a release without one for this machine, isn't
# installed, and leaves the one that's there.
$newer = (Get-FileHash -LiteralPath "$Out\archives\newer.zip" -Algorithm SHA256).Hash
$good = (Get-FileHash -LiteralPath "$Out\archives\good.zip" -Algorithm SHA256).Hash
foreach ($bad in @(
        @{ Version = 'v0.1.1'; Says = ("futureblitz-0.1.1-windows-x64.zip's SHA-256 is " +
                "$($newer.ToLowerInvariant()), not the $($good.ToLowerInvariant())") },
        @{ Version = '0.1.2'; Says = "the release's SHA256SUMS has no archive for windows-x64" })) {
    $refused = Invoke-Download @('-Version', $bad.Version)
    if ($refused.Code -eq 0 -or -not $refused.Output.Contains($bad.Says)) {
        Fail "$($bad.Version) gave: $($refused.Output)"
    }
    if ((Get-Content -LiteralPath $which) -ne 'good') { Fail "$($bad.Version) replaced it" }
}
Write-Host 'install.ps1 passed'
