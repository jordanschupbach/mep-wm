# Windows install helper for the justfile's [windows] recipes (the analogue
# of scripts/macos-service.sh). Runs under Windows PowerShell 5.1+ and needs
# no admin rights: everything lives under the user profile.
#
#   windows-install.ps1 pre-install <prefix>        stop a running mepwm so its exe can be replaced
#   windows-install.ps1 install <prefix>            PATH entry + Start-menu shortcut
#   windows-install.ps1 uninstall <prefix>          undo install (and service-install)
#   windows-install.ps1 service-install <prefix>    start at sign-in + start now
#   windows-install.ps1 service-uninstall <prefix>  stop starting at sign-in
#   windows-install.ps1 start|stop|status <prefix>

param(
  [Parameter(Mandatory = $true, Position = 0)]
  [ValidateSet('pre-install', 'install', 'uninstall', 'service-install', 'service-uninstall', 'start', 'stop', 'status')]
  [string]$Command,
  [Parameter(Mandatory = $true, Position = 1)]
  [string]$Prefix
)

$ErrorActionPreference = 'Stop'

$bin = Join-Path $Prefix 'bin'
$gui = Join-Path $bin 'MEP-wm.exe'
$cli = Join-Path $bin 'mepwm.exe'
$programs = [Environment]::GetFolderPath('Programs')  # Start menu\Programs
$startup = [Environment]::GetFolderPath('Startup')
$menuShortcut = Join-Path $programs 'MEP-wm.lnk'
$startupShortcut = Join-Path $startup 'MEP-wm.lnk'
# Set by pre-install when it stopped a running mepwm; install then restarts it.
$restartMarker = Join-Path $env:TEMP 'mepwm-restart-after-install'

function New-Shortcut([string]$Path, [string]$Target) {
  $shell = New-Object -ComObject WScript.Shell
  $link = $shell.CreateShortcut($Path)
  $link.TargetPath = $Target
  # mepwm resolves projects, TODO.org and git status from its working dir.
  $link.WorkingDirectory = $env:USERPROFILE
  $link.Description = 'MEP-wm tiling window manager'
  $link.Save()
}

function Get-UserPath {
  $value = [Environment]::GetEnvironmentVariable('Path', 'User')
  if ($null -eq $value) { return @() }
  return @($value -split ';' | Where-Object { $_ -ne '' })
}

function Test-SamePath([string]$Left, [string]$Right) {
  return $Left.TrimEnd('\') -ieq $Right.TrimEnd('\')
}

# Smart App Control (Windows 11) refuses to run unsigned programs that have
# no cloud reputation -- which is every locally compiled build, mepwm
# included. Say so up front instead of leaving an opaque launch failure.
function Show-SmartAppControlWarning {
  $state = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
  if ($state -ne 1) { return }
  Write-Host ''
  Write-Host 'WARNING: Smart App Control is ON, and it blocks unsigned, locally built programs.' -ForegroundColor Yellow
  Write-Host '         Windows will refuse to start mepwm ("An Application Control policy has blocked this file").' -ForegroundColor Yellow
  Write-Host '         To run it, either sign the binaries with a publicly trusted code-signing certificate,' -ForegroundColor Yellow
  Write-Host '         or turn Smart App Control off: Windows Security > App & browser control >' -ForegroundColor Yellow
  Write-Host '         Smart App Control settings > Off. (On some Windows builds it cannot be turned back on' -ForegroundColor Yellow
  Write-Host '         without reinstalling Windows.)' -ForegroundColor Yellow
}

function Get-MepwmProcesses {
  return @(Get-Process -Name 'mepwm', 'MEP-wm' -ErrorAction SilentlyContinue)
}

function Start-Mepwm {
  if ((Get-MepwmProcesses).Count -gt 0) {
    Write-Host 'mepwm is already running.'
    return
  }
  if (-not (Test-Path $gui)) { throw "$gui not found; run 'just install' first." }
  Start-Process -FilePath $gui -WorkingDirectory $env:USERPROFILE
  Write-Host 'Started MEP-wm.'
}

function Stop-Mepwm {
  $running = Get-MepwmProcesses
  if ($running.Count -eq 0) {
    Write-Host 'mepwm is not running.'
    return
  }
  # Ask it to quit like Mod+Shift+q does (showing windows hidden on other
  # workspaces again); kill it only if it doesn't exit. A killed mepwm
  # restores those windows on its next start.
  if (Test-Path $cli) { & $cli --quit | Out-Null }
  $deadline = (Get-Date).AddSeconds(5)
  while ((Get-MepwmProcesses).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
  $running = Get-MepwmProcesses
  if ($running.Count -gt 0) { $running | Stop-Process -Force }
  Write-Host 'Stopped mepwm.'
}

switch ($Command) {
  'pre-install' {
    # Windows locks a running .exe, so cmake --install cannot overwrite it
    # ("Permission denied"). Quit mepwm first and remember to restart it.
    if ((Get-MepwmProcesses).Count -gt 0) {
      Stop-Mepwm
      New-Item -ItemType File -Force -Path $restartMarker | Out-Null
    }
  }
  'install' {
    if (-not (Test-Path $cli)) { throw "$cli not found; the cmake install step did not run." }
    $path = Get-UserPath
    if (-not ($path | Where-Object { Test-SamePath $_ $bin })) {
      [Environment]::SetEnvironmentVariable('Path', (($path + $bin) -join ';'), 'User')
      Write-Host "Added $bin to your user PATH (open a new terminal to pick it up)."
    }
    New-Shortcut $menuShortcut $gui
    Write-Host "Installed mepwm to $bin"
    Write-Host "  Start menu: 'MEP-wm'"
    Write-Host "  Terminal:   mepwm        (Alt+Shift+q quits; Alt+Shift+/ shows the keybindings)"
    Write-Host "  At sign-in: just service-install"
    Show-SmartAppControlWarning
    if (Test-Path $restartMarker) {
      Remove-Item -Force $restartMarker
      Start-Mepwm
    }
  }
  'uninstall' {
    Stop-Mepwm
    foreach ($file in @($menuShortcut, $startupShortcut, $cli, $gui)) {
      if (Test-Path $file) { Remove-Item -Force $file }
    }
    $assets = Join-Path $Prefix 'share\mepwm'
    if (Test-Path $assets) { Remove-Item -Recurse -Force $assets }
    $path = Get-UserPath
    $kept = @($path | Where-Object { -not (Test-SamePath $_ $bin) })
    # Only drop the PATH entry when nothing else lives in that bin dir.
    $leftovers = @(Get-ChildItem -Path $bin -ErrorAction SilentlyContinue)
    if ($kept.Count -ne $path.Count -and $leftovers.Count -eq 0) {
      [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
      Write-Host "Removed $bin from your user PATH."
    }
    Write-Host 'Uninstalled mepwm.'
  }
  'service-install' {
    New-Shortcut $startupShortcut $gui
    Write-Host 'MEP-wm will start at sign-in.'
    Show-SmartAppControlWarning
    Start-Mepwm
  }
  'service-uninstall' {
    if (Test-Path $startupShortcut) { Remove-Item -Force $startupShortcut }
    Write-Host 'MEP-wm will no longer start at sign-in (still installed; it keeps running until you quit it).'
  }
  'start' { Start-Mepwm }
  'stop' { Stop-Mepwm }
  'status' {
    $running = Get-MepwmProcesses
    if ($running.Count -gt 0) {
      Write-Host "running (pid $(($running | ForEach-Object Id) -join ', '))"
    } else {
      Write-Host 'not running'
    }
    $atLogin = if (Test-Path $startupShortcut) { 'yes' } else { 'no' }
    Write-Host "starts at sign-in: $atLogin"
  }
}
