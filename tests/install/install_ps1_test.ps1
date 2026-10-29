# install.ps1 on releases made of this build's fb, into a FUTUREBLITZ_HOME with a space, leaving
# PATH alone: install_ps1_test.ps1 <install.ps1> <output folder> <fb.exe>.
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

# Run in the caller's session, as `irm | iex` runs it, it leaves the session as it was.
$probe = @"
`$ErrorActionPreference = 'Continue'
`$ProgressPreference = 'Continue'
`$before = [Net.ServicePointManager]::SecurityProtocol
. '$Script' -Archive '$Out\archives\newer.zip' -NoModifyPath | Out-Null
`$left = @(Get-Variable root, repository, work, bin -ErrorAction SilentlyContinue).Count
`$left += @(Get-Command Install-FutureBlitz -ErrorAction SilentlyContinue).Count
"`$ErrorActionPreference `$ProgressPreference `$([Net.ServicePointManager]::SecurityProtocol -eq `$before) `$left"
"@
Set-Content -LiteralPath "$Out\probe.ps1" -Value $probe
$session = & $shell -NoProfile -ExecutionPolicy Bypass -File "$Out\probe.ps1" | Select-Object -Last 1
if ($session -ne 'Continue Continue True 0') { Fail "the session was left with: $session" }

Write-Host 'install.ps1 passed'
