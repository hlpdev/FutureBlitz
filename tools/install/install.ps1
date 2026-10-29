# Installs FutureBlitz on Windows from its GitHub Releases into %USERPROFILE%\.futureblitz, or
# $env:FUTUREBLITZ_HOME, and puts its bin on the user's PATH. Running it again updates it.
#
#   irm https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.ps1 | iex
#   .\install.ps1 -Version 0.1.0
#   .\install.ps1 -Archive futureblitz-0.1.0-windows-x64.zip
#
# -NoModifyPath leaves PATH as it is.
param(
    [string]$Version = $env:FUTUREBLITZ_VERSION,
    [string]$Archive = "",
    [switch]$NoModifyPath
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# Windows PowerShell 5 asks for TLS 1.0 unless it's told otherwise, which GitHub refuses.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repository = 'hlpdev/FutureBlitz'
# The x64 release runs on arm64 Windows too, emulated.
$target = 'windows-x64'
$root = if ($env:FUTUREBLITZ_HOME) { $env:FUTUREBLITZ_HOME } else {
    Join-Path $env:USERPROFILE '.futureblitz'
}
$work = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work | Out-Null

try {
    if (-not $Archive) {
        if (-not $Version) {
            try {
                $latest = Invoke-RestMethod "https://api.github.com/repos/$repository/releases/latest"
            } catch {
                throw "couldn't find FutureBlitz's latest release: $_"
            }
            $Version = $latest.tag_name
        }

        $Version = $Version.TrimStart('v')
        $name = "futureblitz-$Version-$target.zip"
        Write-Host "Downloading FutureBlitz $Version for $target"
        $Archive = Join-Path $work $name
        try {
            Invoke-WebRequest "https://github.com/$repository/releases/download/v$Version/$name" `
                -OutFile $Archive -UseBasicParsing
        } catch {
            throw "couldn't download $name; is $Version a release? $_"
        }
    }

    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
        throw "there's no archive $Archive"
    }

    # The archive holds one folder, futureblitz-<version>-<target>.
    $unpacked = Join-Path $work 'unpacked'
    Expand-Archive -LiteralPath $Archive -DestinationPath $unpacked
    $release = Get-ChildItem -LiteralPath $unpacked -Directory | Select-Object -First 1
    if (-not $release -or -not (Test-Path -LiteralPath (Join-Path $release.FullName 'bin\fb.exe'))) {
        throw "$Archive isn't a FutureBlitz release"
    }

    # The release's folders replace those there, and anything else in the folder stays.
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    foreach ($folder in Get-ChildItem -LiteralPath $release.FullName) {
        $there = Join-Path $root $folder.Name
        if (Test-Path -LiteralPath $there) {
            try {
                Remove-Item -LiteralPath $there -Recurse -Force
            } catch {
                throw "couldn't replace $there; is fb running? $_"
            }
        }
        Move-Item -LiteralPath $folder.FullName -Destination $root
    }

    $bin = Join-Path $root 'bin'
    $installed = & (Join-Path $bin 'fb.exe') --version
    if ($LASTEXITCODE -ne 0) {
        throw "the installed fb doesn't run: $bin\fb.exe"
    }

    # bin on the user's PATH, once, and on this session's.
    if (-not $NoModifyPath) {
        $path = [Environment]::GetEnvironmentVariable('Path', 'User')
        $entries = if ($path) { $path -split ';' | Where-Object { $_ } } else { @() }
        if ($entries -notcontains $bin) {
            [Environment]::SetEnvironmentVariable('Path', (@($entries) + $bin) -join ';', 'User')
            Write-Host "Added $bin to your PATH"
        }
        if (($env:Path -split ';') -notcontains $bin) {
            $env:Path = "$bin;$env:Path"
        }
    }

    Write-Host "Installed $installed in $root"
    Write-Host "Open a new terminal to use fb anywhere."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
