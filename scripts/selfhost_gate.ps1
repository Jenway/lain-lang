# 自举闸门：拿 Meta 降级 bootstrap 的单元，并把产物装回 lainir 验证器。
#
# 口径：对每个单元先算出它的 import 闭包（相对导入方解析，与 lain0 的
# resolveModulePath 同语义），只注册闭包里的源码，登记名由 --root bootstrap 从
# 源文件路径算出。闭包不经过 meta.lain 的只有 5 个叶子单元（emit 1、regions 2、
# types 2、lex 3、registry 3）；其余 23 个的闭包都是全部 28 份源码（583217 字节），
# 单次会在推导出的 4e9 步燃料内跑不完、没有产物（std/parse 实测 6 分 44 秒）。
#
# 判据：host_status=0、输出里没有 trap 行、产物过 lainir 解析与校验
# （build/check_unit.exe 打印 PARSE+VERIFY OK）。三者缺一即红。check_unit 的
# 编译命令见 docs/development.md。
#
# 用法：
#   powershell -NoProfile -File scripts/selfhost_gate.ps1
#   powershell -NoProfile -File scripts/selfhost_gate.ps1 -Only std/regions,std/types
param(
  [string[]]$Only = @(),
  [string]$Units = 'build/units',
  [string]$OutDir = 'build/selfhost'
)

Set-Location (Split-Path -Parent $PSScriptRoot)

# powershell -File 传进来的 -Only a,b 是一整个字符串（逗号不切），这里按逗号拆开，
# 这样命令行写 -Only std/regions,std/types 与写 -Only std/regions 都成立。
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })

function Normalize-Rel([string]$p) {
  $parts = New-Object System.Collections.Generic.List[string]
  foreach ($seg in ($p -replace '\\', '/' -split '/')) {
    if ($seg -eq '' -or $seg -eq '.') { continue }
    if ($seg -eq '..') {
      if ($parts.Count -gt 0 -and $parts[$parts.Count - 1] -ne '..') {
        $parts.RemoveAt($parts.Count - 1)
        continue
      }
      $parts.Add('..')
      continue
    }
    $parts.Add($seg)
  }
  return ($parts -join '/')
}

function Dir-Of([string]$p) {
  $i = $p.LastIndexOf('/')
  if ($i -lt 0) { return '.' }
  return $p.Substring(0, $i)
}

# 源文件里 import("…") 的说明符（跳过注释行）。
function Specs-Of([string]$file) {
  $out = @()
  foreach ($line in [IO.File]::ReadAllLines($file)) {
    $t = $line.Trim()
    if ($t.StartsWith('//')) { continue }
    foreach ($m in [regex]::Matches($t, 'import\("([^"]+)"\)')) {
      $out += $m.Groups[1].Value
    }
  }
  return $out
}

# 说明符 → 源根相对路径（带 .lain）。规则与 lain0 的 resolveModulePath 一致：
# `a::b` 取最后一段 mod，先试 <dir>/<mod>.lain 再试 <dir>/<base>/<mod>.lain；
# 目录是导入方所在目录；以 std/ 开头且能在源根直接找到时按源根解析。
function Resolve-Spec([string]$importer, [string]$spec) {
  $dir = Dir-Of $importer
  $mod = $spec
  $base = ''
  $pos = $spec.LastIndexOf('::')
  if ($pos -ge 0) {
    $base = $spec.Substring(0, $pos)
    $mod = $spec.Substring($pos + 2)
  }
  $cands = @()
  $cands += (Normalize-Rel ($dir + '/' + $mod + '.lain'))
  if ($base -ne '') {
    $cands += (Normalize-Rel ($dir + '/' + $base + '/' + $mod + '.lain'))
  }
  $rootCand = Normalize-Rel ($mod + '.lain')
  $cands += $rootCand
  foreach ($c in $cands) {
    if (Test-Path ('bootstrap/' + $c)) { return $c }
  }
  return $cands[0]
}

# 闭包：被依赖的在前（深度优先后序，环用 visited 打断）。
function Closure-Of([string]$entry) {
  $seen = New-Object 'System.Collections.Generic.HashSet[string]'
  $order = New-Object System.Collections.Generic.List[string]
  $stack = New-Object System.Collections.Generic.List[object]
  $stack.Add(@($entry, $false))
  while ($stack.Count -gt 0) {
    $top = $stack[$stack.Count - 1]
    $path = $top[0]
    $expanded = $top[1]
    if ($expanded) {
      $stack.RemoveAt($stack.Count - 1)
      continue
    }
    if ($seen.Contains($path)) { $stack.RemoveAt($stack.Count - 1); continue }
    $stack[$stack.Count - 1] = @($path, $true)
    $deps = @()
    $src = 'bootstrap/' + $path
    if (Test-Path $src) {
      foreach ($sp in (Specs-Of $src)) {
        $dep = Resolve-Spec $path $sp
        if ($dep -ne $path) { $deps += $dep }
      }
    }
    for ($k = $deps.Count - 1; $k -ge 0; $k--) {
      if (-not $seen.Contains($deps[$k])) { $stack.Add(@($deps[$k], $false)) }
    }
    if (-not $seen.Contains($path)) {
      $seen.Add($path) | Out-Null
      $order.Add($path)
    }
  }
  return $order
}

$order = @(
  'std/regions', 'std/types', 'std/lex', 'std/registry', 'std/parse',
  'std/recognize', 'std/syntax', 'std/scalars', 'std/sums', 'std/records',
  'std/handlers/enum', 'std/handlers/func', 'std/handlers/if',
  'std/handlers/let', 'std/handlers/statements', 'std/handlers/struct',
  'std/macros/my_block', 'std/macros/my_if', 'std/macros/my_require',
  'std/lainir', 'std/emit', 'std/eval', 'std/funcs', 'std/declarations',
  'std/expand', 'std/modules', 'std/scope', 'meta'
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$pass = 0
$fail = @()
foreach ($unit in $order) {
  if ($Only.Count -gt 0 -and -not ($Only -contains $unit)) { continue }
  $outFile = Join-Path $OutDir (($unit -replace '/', '_') + '.out')
  $sources = @(Closure-Of ($unit + '.lain'))
  $lower = [array]::IndexOf($sources, ($unit + '.lain'))
  if ($lower -lt 0) {
    $fail += ($unit + ':闭包里没有自己')
    Write-Output ("FAIL  " + $unit + "  闭包里没有自己")
    continue
  }
  $argv = @('--root', 'bootstrap', '--units-dir', ($Units + '/'),
            '--lower', "$lower", '--out', $outFile) +
          @($sources | ForEach-Object { 'bootstrap/' + $_ })
  $log = (& ./build/lain-meta.exe @argv 2>&1 | Out-String)
  $st = [regex]::Match($log, 'host_status=([0-9]+)')
  $code = if ($st.Success) { $st.Groups[1].Value } else { 'NA' }
  $trap = $log -match 'trap kind'
  $reason = ''
  if ($code -ne '0') { $reason = 'status=' + $code }
  elseif ($trap) { $reason = 'trap' }
  elseif (-not (Test-Path $outFile)) { $reason = '无产物' }
  else {
    # 单份产物只含本单元自己的过程；跨模块调用指向的符号在依赖单元里。所以校验口径
    # 是「产物 + 依赖单元」一起装载（依赖用 build/units 下同键那份，符号集与它自举
    # 产出的完全一致），报 2015 unknown callee 才算不合格。
    $checkFile = Join-Path $OutDir (($unit -replace '/', '_') + '.check.out')
    $parts = @([IO.File]::ReadAllText((Resolve-Path $outFile)))
    foreach ($dep in $sources) {
      if ($dep -eq ($unit + '.lain')) { continue }
      $depName = ($dep -replace '\.lain$', '') -replace '/', '_'
      $depUnit = Join-Path $Units ($depName + '.l1')
      if (Test-Path $depUnit) { $parts += [IO.File]::ReadAllText((Resolve-Path $depUnit)) }
    }
    [IO.File]::WriteAllText((Join-Path $PWD $checkFile), ($parts -join "`n"))
    $chk = (& ./build/check_unit.exe $checkFile 2>&1 | Out-String)
    if ($chk -notmatch 'PARSE.*VERIFY OK') { $reason = '产物不合格: ' + $chk.Trim() }
  }
  if ($reason -eq '') {
    $pass++
    Write-Output ("PASS  " + $unit + "  闭包=" + $sources.Count)
  } else {
    $fail += ($unit + ':' + $reason)
    Write-Output ("FAIL  " + $unit + "  " + $reason + "  闭包=" + $sources.Count)
    if ($env:LAIN_GATE_VERBOSE) { Write-Output $log }
  }
}
Write-Output ("PASS=" + $pass + " FAIL=" + $fail.Count)
if ($fail.Count -gt 0) { Write-Output ($fail -join ' '); exit 1 }
exit 0
