<#
.SYNOPSIS
  Builds and installs iframe-windows-host, and keeps it running.

.DESCRIPTION
  - Builds WindowsHost (Visual Studio with "Desktop development with C++" is required).
  - Creates a PIN in %APPDATA%\iframe\windows-host-pin if there isn't one.
  - Installs both executables in %ProgramFiles%\iFrame and registers the "iFrame Windows host"
    service. It starts at boot and runs the host as SYSTEM on the physical screen's session, so
    clients can also see and use the lock screen, the sign-in screen and UAC prompts.
  - Allows the host through Windows Firewall on private and domain networks.
  These need administrator rights, so there is one UAC prompt.

.PARAMETER LoginTask
  Instead of the service, install in %LOCALAPPDATA%\Programs\iFrame and start the host as you at
  every login (the UAC prompt is then only for the firewall). Clients can't see or use the lock
  screen, sign-in screen or UAC prompts this way.

.PARAMETER Elevated
  With -LoginTask: run the host as administrator, so it can also control elevated windows (Task
  Manager, installers, admin terminals). Run this script from an admin PowerShell to use it.

.PARAMETER Restart
  Restarts the installed host without rebuilding, e.g. to pick up a changed PIN.

.PARAMETER Uninstall
  Stops the host and removes the service or task, the firewall rule and the installed program.
  The PIN and the log are kept.
#>
param([switch]$LoginTask, [switch]$Elevated, [switch]$Restart, [switch]$Uninstall,
      # Internal: the steps that run as administrator.
      [string]$AdminStep, [string]$PinFile)
$ErrorActionPreference = 'Stop'

$TaskName = 'iFrame Windows host'
$ServiceName = 'iFrameWindowsHost'
$RuleName = 'iFrame Windows host'
$UserDir = Join-Path $env:LOCALAPPDATA 'Programs\iFrame'
$ServiceDir = Join-Path $env:ProgramFiles 'iFrame'
if (-not $PinFile) { $PinFile = Join-Path $env:APPDATA 'iframe\windows-host-pin' }
$UserLog = Join-Path $env:LOCALAPPDATA 'iframe\windows-host.log'
# SYSTEM writes (and rotates) the service's log, so it's somewhere only SYSTEM and admins can write.
$ServiceLog = Join-Path $env:ProgramData 'iFrame\windows-host.log'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

function Set-FirewallRule([string]$Dir) {
    Remove-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue
    if (-not $Dir) { return }
    foreach ($exe in 'iframe-windows-hostw.exe', 'iframe-windows-host.exe') {
        New-NetFirewallRule -DisplayName $RuleName -Direction Inbound -Action Allow -Profile Private, Domain `
            -Program (Join-Path $Dir $exe) | Out-Null
    }
}

function Remove-HostService {
    if (-not (Get-Service $ServiceName -ErrorAction SilentlyContinue)) { return }
    Stop-Service $ServiceName -ErrorAction SilentlyContinue
    & sc.exe delete $ServiceName | Out-Null
}

# MARK: - Steps that run as administrator

if ($AdminStep) {
    switch ($AdminStep) {
        'service' {
            Stop-Service $ServiceName -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force $ServiceDir | Out-Null
            foreach ($exe in 'iframe-windows-host.exe', 'iframe-windows-hostw.exe') {
                Copy-Item (Join-Path $PSScriptRoot "build\$exe") $ServiceDir -Force
            }
            $exe = Join-Path $ServiceDir 'iframe-windows-host.exe'
            $logDir = Split-Path $ServiceLog
            New-Item -ItemType Directory -Force $logDir | Out-Null
            & icacls.exe $logDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' `
                '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
            $bin = "`"$exe`" --service --pin-file `"$PinFile`" --log `"$ServiceLog`""
            if (Get-Service $ServiceName -ErrorAction SilentlyContinue) {
                & sc.exe config $ServiceName binPath= $bin start= auto | Out-Null
            } else {
                New-Service -Name $ServiceName -BinaryPathName $bin -DisplayName $TaskName -StartupType Automatic `
                    -Description 'Streams this desktop to iFrame clients, including the lock and sign-in screens.' | Out-Null
            }
            & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/60000 | Out-Null
            Set-FirewallRule $ServiceDir
            Start-Service $ServiceName
        }
        'logintask' { Remove-HostService; Remove-Item $ServiceDir -Recurse -Force -ErrorAction SilentlyContinue; Set-FirewallRule $UserDir }
        'restart' { Restart-Service $ServiceName }
        'uninstall' { Remove-HostService; Remove-Item $ServiceDir -Recurse -Force -ErrorAction SilentlyContinue; Set-FirewallRule '' }
    }
    return
}

# Runs one of the steps above as administrator: directly if we are, else through one UAC prompt.
function Invoke-AdminStep([string]$Step) {
    if ($admin) { & $PSCommandPath -AdminStep $Step -PinFile $PinFile; return }
    $p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile',
        '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-AdminStep', $Step,
        '-PinFile', "`"$PinFile`""
    if ($p.ExitCode) { throw "The administrator step ($Step) didn't complete (exit $($p.ExitCode))." }
}

# MARK: - Steps that run as you

function Stop-LoginTask {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    $hostw = Join-Path $UserDir 'iframe-windows-hostw.exe'
    Get-Process iframe-windows-hostw -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $hostw } |
        Stop-Process -Force
}

function Remove-LoginTask {
    Stop-LoginTask
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $UserDir -Recurse -Force -ErrorAction SilentlyContinue
}

$hasService = [bool](Get-Service $ServiceName -ErrorAction SilentlyContinue)

if ($Uninstall) {
    Remove-LoginTask
    Invoke-AdminStep 'uninstall'
    Write-Host "Uninstalled. The PIN ($PinFile) and logs ($ServiceLog, $UserLog) are still there."
    return
}
if ($Restart) {
    if ($hasService) { Invoke-AdminStep 'restart' } else { Stop-LoginTask; Start-ScheduledTask -TaskName $TaskName }
    Write-Host "Restarted. PIN: $(Get-Content $PinFile)"
    return
}
if ($Elevated -and -not $LoginTask) { throw '-Elevated is for -LoginTask; the service can already control every window.' }
if ($Elevated -and -not $admin) { throw 'Run this from an admin PowerShell to install with -Elevated.' }

Write-Host 'Building...'
& cmd /c "`"$PSScriptRoot\build.cmd`""
if ($LASTEXITCODE) { throw 'The build failed.' }

if (-not (Test-Path $PinFile) -or -not (Get-Content $PinFile -ErrorAction SilentlyContinue)) {
    New-Item -ItemType Directory -Force (Split-Path $PinFile) | Out-Null
    $bytes = New-Object byte[] 4
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $pin = '{0:D6}' -f ([BitConverter]::ToUInt32($bytes, 0) % 1000000)
    Set-Content -Path $PinFile -Value $pin -Encoding ascii -NoNewline
}
if (-not $LoginTask) {
    Remove-LoginTask
    Write-Host 'Installing the service and allowing it through Windows Firewall (private and domain networks)...'
    Invoke-AdminStep 'service'
    Write-Host ''
    Write-Host "Installed the '$TaskName' service in $ServiceDir; it starts at boot."
    Write-Host "Log: $ServiceLog"
} else {
    Stop-LoginTask
    $hostw = Join-Path $UserDir 'iframe-windows-hostw.exe'
    New-Item -ItemType Directory -Force $UserDir | Out-Null
    foreach ($exe in 'iframe-windows-host.exe', 'iframe-windows-hostw.exe') {
        Copy-Item (Join-Path $PSScriptRoot "build\$exe") $UserDir -Force
    }
    Write-Host 'Allowing the host through Windows Firewall (private and domain networks)...'
    if ($hasService) { Write-Host 'and removing the service...' }
    Invoke-AdminStep 'logintask'

    $action = New-ScheduledTaskAction -Execute $hostw
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
    Write-Host "Installed $hostw; it starts at login$(if ($Elevated) { ' as administrator' })."
    Write-Host "Log: $UserLog"
}
Write-Host "PIN: $(Get-Content $PinFile)   (in $PinFile)"
