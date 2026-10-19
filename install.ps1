# installs odin on windows. paste this into powershell:
#   irm https://raw.githubusercontent.com/slizsy/Odin/main/install.ps1 | iex
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # the progress bar makes downloads crazy slow on old powershell

$repo  = 'slizsy/Odin'
$base  = "https://github.com/$repo/releases/latest/download"
$asset = 'odin-windows-x86_64.exe'
$dir   = Join-Path $env:LOCALAPPDATA 'Programs\odin'
$exe   = Join-Path $dir 'odin.exe'

if (-not [Environment]::Is64BitOperatingSystem) { throw 'odin needs 64-bit windows' }
# windows powershell 5.1 doesn't do tls 1.2 by default
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

New-Item -ItemType Directory -Force -Path $dir | Out-Null
Write-Host "downloading $asset..."
$tmp = "$exe.download"
Invoke-WebRequest "$base/$asset" -OutFile $tmp -UseBasicParsing

# make sure we got exactly what was published
$raw = (Invoke-WebRequest "$base/SHA256SUMS" -UseBasicParsing).Content
if ($raw -is [byte[]]) { $raw = [Text.Encoding]::UTF8.GetString($raw) }
$line = ($raw -split "`n") | Where-Object { $_ -match "\s$([regex]::Escape($asset))\s*$" } | Select-Object -First 1
$want = if ($line) { ($line -split '\s+')[0].ToLower() } else { '' }
$got  = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
if (-not $want -or $want -ne $got) {
    Remove-Item $tmp -Force
    throw "checksum didn't match, download got mangled. try again"
}
Move-Item $tmp $exe -Force

# put it on PATH for this user so 'odin' works from any terminal
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not $userPath) { $userPath = '' }
if (($userPath -split ';') -notcontains $dir) {
    [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ";$dir").TrimStart(';'), 'User')
}
if (($env:Path -split ';') -notcontains $dir) { $env:Path += ";$dir" }

Write-Host "installed to $exe"
& $exe version
Write-Host "run 'odin' to get started (new terminals pick it up automatically)"
