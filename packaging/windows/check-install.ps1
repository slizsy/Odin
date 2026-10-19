# ci check: install.ps1 has to parse on both powershell 7 and the old 5.1 that ships with windows
$errs = $null
[void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '..\..\install.ps1'), [ref]$null, [ref]$errs)
if ($errs) { $errs | ForEach-Object { Write-Host $_ }; exit 1 }
Write-Host "install.ps1 parses fine on powershell $($PSVersionTable.PSVersion)"
