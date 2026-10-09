<#
.SYNOPSIS
  Builds and installs iframe-windows-host, and starts it at every login.

.DESCRIPTION
  - Builds WindowsHost (Visual Studio with "Desktop development with C++" is required).
  - Installs both executables in %LOCALAPPDATA%\Programs\iFrame.
  - Creates a PIN in %APPDATA%\iframe\windows-host-pin if there isn't one.
  - Allows the host through Windows Firewall on private and domain networks (asks for admin once).
  - Registers the "iFrame Windows host" task, which starts the windowless host at login and
    restarts it if it stops, then starts it now.

.PARAMETER Elevated
  Run the host as administrator, so it can also control elevated windows (Task Manager,
  installers, admin terminals). Run this script from an admin PowerShell to use it.

.PARAMETER Uninstall
  Stops the host and removes the task, the firewall rule and the installed program. The PIN and
  the log are kept.
#>
param([switch]$Elevated, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$TaskName = 'iFrame Windows host'
$RuleName = 'iFrame Windows host'
$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\iFrame'
$Host_ = Join-Path $InstallDir 'iframe-windows-host.exe'
$HostW = Join-Path $InstallDir 'iframe-windows-hostw.exe'
$PinFile = Join-Path $env:APPDATA 'iframe\windows-host-pin'
$LogFile = Join-Path $env:LOCALAPPDATA 'iframe\windows-host.log'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

# Runs firewall commands as administrator: directly if we are, else through one UAC prompt.
function Invoke-AsAdmin([string]$Command) {
    if ($admin) { Invoke-Expression $Command; return }
    $p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
        -ArgumentList '-NoProfile', '-Command', $Command
    if ($p.ExitCode) { Write-Warning "The firewall change didn't complete (exit $($p.ExitCode))." }
}

function Stop-Host {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Get-Process iframe-windows-hostw -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $HostW } |
        Stop-Process -Force
}

if ($Uninstall) {
    Stop-Host
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Invoke-AsAdmin "Remove-NetFirewallRule -DisplayName '$RuleName' -ErrorAction SilentlyContinue"
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Uninstalled. The PIN ($PinFile) and log ($LogFile) are still there."
    return
}
if ($Elevated -and -not $admin) { throw 'Run this from an admin PowerShell to install with -Elevated.' }

Write-Host 'Building...'
& cmd /c "`"$PSScriptRoot\build.cmd`""
if ($LASTEXITCODE) { throw 'The build failed.' }

Stop-Host
New-Item -ItemType Directory -Force $InstallDir | Out-Null
foreach ($exe in 'iframe-windows-host.exe', 'iframe-windows-hostw.exe') {
    Copy-Item (Join-Path $PSScriptRoot "build\$exe") $InstallDir -Force
}

if (-not (Test-Path $PinFile) -or -not (Get-Content $PinFile -ErrorAction SilentlyContinue)) {
    New-Item -ItemType Directory -Force (Split-Path $PinFile) | Out-Null
    $bytes = New-Object byte[] 4
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $pin = '{0:D6}' -f ([BitConverter]::ToUInt32($bytes, 0) % 1000000)
    Set-Content -Path $PinFile -Value $pin -Encoding ascii -NoNewline
}

Write-Host 'Allowing the host through Windows Firewall (private and domain networks)...'
Invoke-AsAdmin @"
Remove-NetFirewallRule -DisplayName '$RuleName' -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName '$RuleName' -Direction Inbound -Action Allow -Profile Private,Domain -Program '$HostW' | Out-Null
New-NetFirewallRule -DisplayName '$RuleName' -Direction Inbound -Action Allow -Profile Private,Domain -Program '$Host_' | Out-Null
"@

$action = New-ScheduledTaskAction -Execute $HostW
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -StartWhenAvailable
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive `
    -RunLevel $(if ($Elevated) { 'Highest' } else { 'Limited' })
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Description 'Streams this desktop to iFrame clients.' -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName

Write-Host ''
Write-Host "Installed $HostW; it starts at login$(if ($Elevated) { ' as administrator' })."
Write-Host "PIN: $(Get-Content $PinFile)   (in $PinFile)"
Write-Host "Log: $LogFile"
