<#
.SYNOPSIS
Create an optional desktop or per-user login shortcut for Node-Sight.
.PARAMETER Startup
Use the current user's Startup folder instead of the desktop. No admin rights required.
.PARAMETER Remove
Remove only a shortcut that still points to this project's executable.
#>
param([switch]$Startup, [switch]$Remove)
$ErrorActionPreference = 'Stop'
$executable = Join-Path $PSScriptRoot 'dist\NodeSight.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Run .\build.ps1 first.' }
# Resolve special folders rather than hardcoding user names or desktop locations.
$folder = if ($Startup) { [Environment]::GetFolderPath('Startup') } else { [Environment]::GetFolderPath('Desktop') }
$shortcutPath = Join-Path $folder 'Node-Sight.lnk'
$shell = New-Object -ComObject WScript.Shell
if ($Remove) {
    # Refuse to remove a shortcut that has been repurposed by another app/user.
    if (Test-Path -LiteralPath $shortcutPath) {
        $existing = $shell.CreateShortcut($shortcutPath)
        if ($existing.TargetPath -ne $executable) { throw 'Shortcut target differs; nothing was removed.' }
        Remove-Item -LiteralPath $shortcutPath
    }
    return
}
# Preferences/credentials are loaded from per-user storage, not from this shortcut.
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $executable
$shortcut.WorkingDirectory = Split-Path -Parent $executable
$shortcut.Description = 'Node-Sight native desktop resource monitor'
$shortcut.IconLocation = "$executable,0"
$shortcut.Save()
Write-Output $shortcutPath
