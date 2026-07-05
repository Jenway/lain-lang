param(
  [string]$Out = "src/compiler/lainc.exe"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $Root
try {
  $sources = @(
    "src/compiler/native_runtime.c",
    "src/compiler/vm_gauche.c",
    "src/compiler/lainir_exec.c",
    "src/compiler/native_compiler.c",
    "src/compiler/builder_ffi.c",
    "src/lainir/lainir_core.c",
    "src/lainast/lain_ast.c",
    "src/lainast/lain_ast_parser.c",
    "src/lainir/emitter.c",
    "src/lainir/emit_text.c",
    "src/lainir/interpreter.c"
  )

  $inc = (& gauche-config -I).Trim()
  $libPath = (& gauche-config -L).Trim()
  $archDir = ((& gauche-config --archdirs) -split ';')[0].Trim()
  $gaucheRoot = $archDir -replace '\\lib\\gauche-0\.98\\.*$', ''
  $gaucheBin = Join-Path $gaucheRoot "bin"
  $gaucheDll = Join-Path $gaucheBin "libgauche-0.98.dll"
  $pthreadDll = Join-Path $gaucheBin "libwinpthread-1.dll"
  $libs = (& gauche-config -l) -split '\s+' |
    Where-Object { $_ -ne "" -and $_ -ne "-lgauche-0.98" }
  $common = @(
    "-Isrc",
    "-DLAIN_SCHEME_BACKEND_GAUCHE",
    "-DLAIN_DISABLE_NATIVE_BUILD",
    "-std=gnu11",
    $inc
  )

  $zig = Get-Command zig.exe -ErrorAction SilentlyContinue
  $clang = Get-Command clang.exe -ErrorAction SilentlyContinue
  $gcc = Get-Command gcc.exe -ErrorAction SilentlyContinue

  New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
  $env:ZIG_LOCAL_CACHE_DIR = Join-Path $Root "target/zig-cache/local"
  $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Root "target/zig-cache/global"
  New-Item -ItemType Directory -Force $env:ZIG_LOCAL_CACHE_DIR | Out-Null
  New-Item -ItemType Directory -Force $env:ZIG_GLOBAL_CACHE_DIR | Out-Null

  $failures = @()
  foreach ($cc in @($zig, $clang, $gcc)) {
    if (-not $cc) { continue }
    try {
      $global:LASTEXITCODE = -1
      if ($cc.Name -eq "zig.exe") {
        & $cc.Source "cc" @common @sources $libPath $gaucheDll @libs "-o" $Out
      } else {
        & $cc.Source @common @sources $libPath $gaucheDll @libs "-o" $Out
      }
      if ($LASTEXITCODE -eq 0 -and (Test-Path $Out)) {
        $outDir = Split-Path $Out
        Copy-Item -Force $gaucheDll $outDir
        Copy-Item -Force $pthreadDll $outDir
        exit 0
      }
      $failures += "$($cc.Name) failed with exit code $LASTEXITCODE"
    } catch {
      $failures += "$($cc.Name) failed: $($_.Exception.Message)"
    }
  }

  if ($failures.Count -eq 0) {
    Write-Error "No C compiler found. Install zig, clang, or gcc."
  }
  Write-Error ($failures -join "; ")
} finally {
  Pop-Location
}
