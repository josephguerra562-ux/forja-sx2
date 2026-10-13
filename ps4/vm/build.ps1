# (AI-assisted) Compile the PS4SX2 application and validate its package.
param(
  [string]$Toolchain = 'C:\OpenOrbis\PS4Toolchain',
  [string]$Distro = 'Ubuntu-24.04',
  [ValidateRange(1, 16)][int]$Jobs = 4,
  [ValidateSet('OFF', 'ON')][string]$Jit = 'ON',
  [ValidateSet('OFF', 'ON')][string]$Fastmem = 'ON',
  [ValidateSet('OFF', 'ON')][string]$MTVU = 'ON'
)
$ErrorActionPreference = 'Stop'
Write-Output "(AI-assisted) VM build: JIT=$Jit Fastmem=$Fastmem MTVU=$MTVU"
$scriptPath = (& wsl.exe -d $Distro -- wslpath -a ($PSScriptRoot.Replace('\', '/') + '/build-wsl.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve WSL script path.' }
& wsl.exe -d $Distro -- env "PCSX2_VM_JIT=$Jit" "PCSX2_VM_FASTMEM=$Fastmem" "PCSX2_VM_MTVU=$MTVU" bash $scriptPath $Jobs
if ($LASTEXITCODE -ne 0) { throw 'VM build failed; no package published.' }
$elfPath = Join-Path $PSScriptRoot 'build/vm_boot'
$stamp = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($elfPath))
if (-not $stamp.Contains('PS4SX2-EMU-1.00-CHD-SMC')) {
  throw 'VM build stamp missing (expected PS4SX2-EMU-1.00-CHD-SMC).'
}
& (Join-Path $PSScriptRoot 'build-package.ps1') -Toolchain $Toolchain
