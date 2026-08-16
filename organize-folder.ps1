<#
  Tidy the project root: park generated files, file working notes under docs\, and group the
  occasional .bat scripts behind category prefixes.

  DRY RUN BY DEFAULT.  Nothing is touched until you pass -Apply.

      powershell -ExecutionPolicy Bypass -File organize-folder.ps1            # show the plan
      powershell -ExecutionPolicy Bypass -File organize-folder.ps1 -Apply     # do it

  PREFIXES, NOT SUFFIXES.  Explorer sorts alphabetically, so a leading tag is what actually
  pulls related scripts together in the listing; a trailing one leaves them scattered and only
  helps if you already know to type it in the search box. Same keystrokes, better result.

  WHAT IS DELIBERATELY LEFT ALONE.  build.bat, rebuild.bat, run.bat, clean-rebuild.bat,
  backup-src.bat, package-release.bat and Diagnostics.bat keep their names. Between them they
  are named by ~15 other files, by CMakeLists.txt, by CLAUDE.md and by the README — renaming
  them means editing all of that and relearning the two commands you type every day, to tidy
  the seven entries you already know by heart. The churn is not worth it.

  Nothing is deleted. Generated files move to _to_delete\ so you can glance at them and remove
  the folder yourself.
#>
[CmdletBinding()]
param([switch]$Apply)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$mode = if ($Apply) { 'APPLY' } else { 'DRY RUN' }
Write-Host ""
Write-Host "  Project root : $root"
Write-Host "  Mode         : $mode" -ForegroundColor $(if ($Apply) { 'Yellow' } else { 'Cyan' })
Write-Host ""

$plan = 0
function Step($msg, $colour = 'Gray') { Write-Host "    $msg" -ForegroundColor $colour }

# ── 1. Generated files → _to_delete\ ────────────────────────────────────────────────────────────
# Every one of these is in .gitignore and is rewritten by the next build, smoke test or run, so
# nothing here is a loss. Parked rather than deleted purely so you can eyeball it first.
Write-Host "  [1] Generated files (all gitignored, all regenerate)" -ForegroundColor White
$generated = @(
    'build_log.txt', 'build_errors.txt',
    'smoke_test.txt', 'smoke_test_app.log',
    'rebuild_out.txt', 'cloth_console.txt', 'cloth_overlay_report.txt',
    'd4cloth_build_log.txt', 'd4cloth_extract_log.txt'
)
$hits = @()
foreach ($g in $generated) { if (Test-Path -LiteralPath $g -PathType Leaf) { $hits += (Get-Item -LiteralPath $g) } }
$hits += @(Get-ChildItem -File -Filter 'd4browser_log_*.txt' -ErrorAction SilentlyContinue)

if ($hits.Count -eq 0) { Step 'nothing to park - root is already clean' 'DarkGray' }
else {
    if ($Apply -and -not (Test-Path '_to_delete')) { New-Item -ItemType Directory -Path '_to_delete' | Out-Null }
    foreach ($f in $hits) {
        $kb = [math]::Round($f.Length / 1KB, 1)
        Step ("-> _to_delete\{0}  ({1} KB)" -f $f.Name, $kb)
        $plan++
        if ($Apply) { Move-Item -LiteralPath $f.FullName -Destination (Join-Path '_to_delete' $f.Name) -Force }
    }
}
Write-Host ""

# ── 2. Working notes → docs\notes\ ──────────────────────────────────────────────────────────────
# Session research and handoff documents. Real history worth keeping, but they are not the
# project's front door and five of them in the root buries README.md and CLAUDE.md.
#
# CLAUDE.md and README.md STAY in the root: the first is only read there, the second is what
# GitHub renders. RELEASE_README.txt stays too - package-release.bat copies it into the zip.
Write-Host "  [2] Working notes -> docs\notes\" -ForegroundColor White
$notes = @(
    'BUNDLES-TAB-RESEARCH.md',
    'ENCRYPTED-CONTENT-HANDOFF.md',
    'PHYSICS_AUDIT.md',
    'PHYSICS_HARNESS_PROMPT.md',
    'STATUS.md'
)
$moved = 0
foreach ($n in $notes) {
    if (-not (Test-Path -LiteralPath $n -PathType Leaf)) { continue }
    Step ("-> docs\notes\{0}" -f $n)
    $plan++; $moved++
    if ($Apply) {
        if (-not (Test-Path 'docs\notes')) { New-Item -ItemType Directory -Path 'docs\notes' | Out-Null }
        # git mv when the file is tracked, so the history follows it rather than reading as
        # delete + add. Falls back to a plain move outside a repo or for untracked files.
        $tracked = $false
        try { git ls-files --error-unmatch -- $n *> $null; $tracked = ($LASTEXITCODE -eq 0) } catch { }
        if ($tracked) { git mv -- $n (Join-Path 'docs\notes' $n) *> $null }
        else { Move-Item -LiteralPath $n -Destination (Join-Path 'docs\notes' $n) -Force }
    }
}
if ($moved -eq 0) { Step 'nothing to file' 'DarkGray' }
Write-Host ""

# ── 3. Group the occasional scripts behind a category prefix ────────────────────────────────────
# Four groups, chosen so the prefix answers "why would I open this":
#   Audit   - reports on the shipped data, safe to run any time
#   Dump    - writes one probe file about one format question
#   Test    - verifies something still works, pass/fail
#   Dev     - only useful while working on cloth
Write-Host "  [3] Rename occasional scripts (prefix = category)" -ForegroundColor White
$renames = [ordered]@{
    'Audit Asset Health.bat'             = 'Audit - Asset Health.bat'
    'Cloth Audit.bat'                    = 'Audit - Cloth.bat'
    'Check Encrypted Render.bat'         = 'Audit - Encrypted Render.bat'
    'Dump Encrypted SNOs.bat'            = 'Dump - Encrypted SNOs.bat'
    'Dump Marking Model.bat'             = 'Dump - Marking Model.bat'
    'Dump Material SNO Table.bat'        = 'Dump - Material SNO Table.bat'
    'Dump StoreProduct Layout.bat'       = 'Dump - StoreProduct Layout.bat'
    'Dump Texture Chain.bat'             = 'Dump - Texture Chain.bat'
    'Test Capsules Full.bat'             = 'Test - Capsules Full.bat'
    'Test Encrypted Chain.bat'           = 'Test - Encrypted Chain.bat'
    'Release Smoke Test.bat'             = 'Test - Release Smoke.bat'
    'Verify TACT Keys.bat'               = 'Test - Verify TACT Keys.bat'
    'Build d4cloth + Extract Corpus.bat' = 'Dev - Build d4cloth + Extract Corpus.bat'
    'Debug Cloth Overlay.bat'            = 'Dev - Debug Cloth Overlay.bat'
    'Set Version.bat'                    = 'Release - Set Version.bat'
    'make_portable.bat'                  = 'Release - Make Portable.bat'
}
$did = 0
foreach ($from in $renames.Keys) {
    $to = $renames[$from]
    if (-not (Test-Path -LiteralPath $from -PathType Leaf)) {
        if (Test-Path -LiteralPath $to -PathType Leaf) { Step ("already done: {0}" -f $to) 'DarkGray' }
        continue
    }
    Step ("{0}  ->  {1}" -f $from, $to)
    $plan++; $did++
    if ($Apply) {
        $tracked = $false
        try { git ls-files --error-unmatch -- $from *> $null; $tracked = ($LASTEXITCODE -eq 0) } catch { }
        if ($tracked) { git mv -- $from $to *> $null } else { Rename-Item -LiteralPath $from -NewName $to }
    }
}
if ($did -eq 0) { Step 'nothing to rename' 'DarkGray' }
Write-Host ""

# ── 4. Repoint every reference to a renamed script ──────────────────────────────────────────────
# Diagnostics.bat is a menu that launches seven of the scripts above BY NAME, and a few of them
# call each other. Renaming without this step leaves a menu whose entries all fail. Text files
# only, and only exact filename matches.
Write-Host "  [4] Repoint references in .bat / .ps1 / .md" -ForegroundColor White
$scan = @(Get-ChildItem -File -Include '*.bat', '*.ps1', '*.md' -ErrorAction SilentlyContinue)
$scan += @(Get-ChildItem -File -Path 'docs' -Include '*.md' -Recurse -ErrorAction SilentlyContinue)
$patched = 0
foreach ($f in $scan) {
    $text = Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8
    $orig = $text
    foreach ($from in $renames.Keys) { $text = $text.Replace($from, $renames[$from]) }
    if ($text -ne $orig) {
        Step ("patched {0}" -f $f.Name) 'Green'
        $plan++; $patched++
        if ($Apply) { Set-Content -LiteralPath $f.FullName -Value $text -Encoding UTF8 -NoNewline }
    }
}
if ($patched -eq 0) { Step 'no references needed changing' 'DarkGray' }
Write-Host ""

# ── 5. What this script cannot fix for you ──────────────────────────────────────────────────────
Write-Host "  [5] Manual follow-ups" -ForegroundColor White
$lnks = @(Get-ChildItem -File -Path '.bat shortcuts' -Filter '*.lnk' -ErrorAction SilentlyContinue)
$broken = @($lnks | Where-Object {
        $target = ($_.BaseName -replace ' - Shortcut$', '')
        $renames.Keys -contains $target
    })
if ($broken.Count -gt 0) {
    Step ("{0} shortcut(s) in '.bat shortcuts\' point at a renamed script and will break:" -f $broken.Count) 'Yellow'
    foreach ($b in $broken) { Step ("  - {0}" -f $b.Name) 'Yellow' }
    Step '  delete and recreate them, or right-click each one and repoint it' 'Yellow'
}
else { Step 'no shortcuts affected' 'DarkGray' }
Step "README's 'Building from source' block names build.bat / rebuild.bat / Diagnostics.bat - all unchanged, nothing to edit" 'DarkGray'
Write-Host ""

if ($Apply) { Write-Host "  Done. $plan change(s) applied." -ForegroundColor Green }
else { Write-Host "  $plan change(s) planned. Re-run with -Apply to make them." -ForegroundColor Cyan }
Write-Host ""
