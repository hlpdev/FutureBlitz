# Installs FutureBlitz on Windows from its GitHub Releases into %USERPROFILE%\.futureblitz, or
# $env:FUTUREBLITZ_HOME, and puts its bin on the user's PATH. Running it again updates it.
#
#   irm https://futureblitz.dev/install.ps1 | iex
#   iex "& { $(irm https://futureblitz.dev/install.ps1) } -Version 1.0.0"
#   .\install.ps1 -Archive futureblitz-1.0.0-windows-x64.zip
#
# Each release has it too: https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.ps1
#
# -NoModifyPath leaves PATH as it is. `irm | iex` runs this in the caller's session, so all of it
# is in a script block, whose parameters, variables, preferences, and function are its own, and
# the session's own variables of those names stay as they were. It's ASCII, as Windows
# PowerShell's irm reads a script served with no charset as Latin-1.
& {
    param(
        [string]$Version = $env:FUTUREBLITZ_VERSION,
        [string]$Archive = "",
        [switch]$NoModifyPath
    )

    function Install-FutureBlitz {
        param([string]$Version, [string]$Archive, [switch]$NoModifyPath)

        $ErrorActionPreference = 'Stop'
        $ProgressPreference = 'SilentlyContinue'
        $repository = 'hlpdev/FutureBlitz'
        # The x64 release runs on arm64 Windows too, emulated.
        $target = 'windows-x64'
        $root = if ($env:FUTUREBLITZ_HOME) { $env:FUTUREBLITZ_HOME } else {
            Join-Path $env:USERPROFILE '.futureblitz'
        }
        $bin = Join-Path $root 'bin'
        $work = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
        New-Item -ItemType Directory -Path $work | Out-Null
        # Windows PowerShell 5 asks for TLS 1.0 unless it's told otherwise, which GitHub refuses.
        $protocols = [Net.ServicePointManager]::SecurityProtocol

        try {
            [Net.ServicePointManager]::SecurityProtocol =
                $protocols -bor [Net.SecurityProtocolType]::Tls12
            if (-not $Archive) {
                if (-not $Version) {
                    try {
                        $api = "https://api.github.com/repos/$repository/releases/latest"
                        $latest = Invoke-RestMethod $api
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
                    $url = "https://github.com/$repository/releases/download/v$Version/$name"
                    Invoke-WebRequest $url -OutFile $Archive -UseBasicParsing
                } catch {
                    throw "couldn't download $name; is $Version a release? $_"
                }
            }

            if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
                throw "there's no archive $Archive"
            }

            # The archive holds one folder, futureblitz-<version>-<target>, which has to be this
            # machine's, and its fb has to run here, before anything there is replaced.
            $unpacked = Join-Path $work 'unpacked'
            try {
                Expand-Archive -LiteralPath $Archive -DestinationPath $unpacked
            } catch {
                throw "couldn't unpack $Archive"
            }
            $release = Get-ChildItem -LiteralPath $unpacked -Directory | Select-Object -First 1
            $fb = if ($release) { Join-Path $release.FullName 'bin\fb.exe' } else { '' }
            if (-not $fb -or -not (Test-Path -LiteralPath $fb)) {
                throw "$Archive isn't a FutureBlitz release"
            }
            if (-not $release.Name.StartsWith('futureblitz-') -or
                -not $release.Name.EndsWith("-$target")) {
                throw "$Archive is a release for another machine, not ${target}: $($release.Name)"
            }
            # A file that isn't a program would have Windows say so in a window of its own, which
            # waits for someone to close it, so only one that starts as a program's does is run.
            $stream = [IO.File]::OpenRead($fb)
            $magic = @($stream.ReadByte(), $stream.ReadByte())
            $stream.Dispose()
            $installed = $null
            if ($magic[0] -eq 0x4D -and $magic[1] -eq 0x5A) {
                try {
                    $installed = & $fb --version 2>$null
                } catch {
                    $installed = $null
                }
            }
            if ($LASTEXITCODE -ne 0 -or -not $installed) {
                throw "$Archive's fb doesn't run here"
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

            # bin on the user's PATH, once, as written there, %VAR%s and all, and on this session's.
            $onPath = $false
            if (-not $NoModifyPath) {
                $key = Get-Item -LiteralPath 'HKCU:\Environment'
                $path = $key.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
                $entries = @($path -split ';' | Where-Object { $_ })
                $expanded = $entries |
                    ForEach-Object { [Environment]::ExpandEnvironmentVariables($_) }
                if ($expanded -notcontains $bin) {
                    $kind = if ($path) { $key.GetValueKind('Path') } else { 'ExpandString' }
                    if ($kind -ne 'String') { $kind = 'ExpandString' }
                    Set-ItemProperty -LiteralPath 'HKCU:\Environment' -Name Path `
                        -Value ((@($entries) + $bin) -join ';') -Type $kind
                    # Windows tells running programs the environment changed when it's set this way.
                    [Environment]::SetEnvironmentVariable('FUTUREBLITZ_INSTALLING', '1', 'User')
                    [Environment]::SetEnvironmentVariable('FUTUREBLITZ_INSTALLING', $null, 'User')
                    Write-Host "Added $bin to your PATH"
                }
                if (($env:Path -split ';') -notcontains $bin) {
                    $env:Path = "$bin;$env:Path"
                }
                $onPath = $true
            }

            Write-Host "Installed $installed in $root"
            if ($onPath) {
                Write-Host "Open a new terminal to use fb anywhere."
            } else {
                Write-Host "Add $bin to PATH to use fb."
            }
        } finally {
            [Net.ServicePointManager]::SecurityProtocol = $protocols
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    try {
        Install-FutureBlitz -Version $Version -Archive $Archive -NoModifyPath:$NoModifyPath
    } catch {
        Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
        # A script run as a file fails; one run with iex leaves its session open.
        if ($PSCommandPath) { exit 1 }
    }
} @args
