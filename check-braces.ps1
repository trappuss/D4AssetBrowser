# Warn if any .cpp/.h under src/ has unbalanced braces (a truncation tell). Single-pass parser that
# skips // line comments, /* block */ comments, "strings" and 'char' literals in one go, so braces
# inside those don't produce false positives. Advisory only (the compiler is the real check); exits 1
# on imbalance so a caller can react.
param([string]$Root = "src")

$bad = @()
Get-ChildItem -Recurse -Include *.cpp,*.h $Root -ErrorAction SilentlyContinue | ForEach-Object {
    $t = Get-Content -Raw $_.FullName
    if (-not $t) { return }
    $depth = 0; $i = 0; $n = $t.Length
    $inLine = $false; $inBlock = $false; $inStr = $false; $inChr = $false
    while ($i -lt $n) {
        $c = $t[$i]
        $d = if ($i + 1 -lt $n) { $t[$i + 1] } else { [char]0 }
        if ($inLine)  { if ($c -eq "`n") { $inLine = $false }; $i++; continue }
        if ($inBlock) { if ($c -eq '*' -and $d -eq '/') { $inBlock = $false; $i += 2; continue }; $i++; continue }
        if ($inStr)   { if ($c -eq '\') { $i += 2; continue }; if ($c -eq '"')  { $inStr = $false }; $i++; continue }
        if ($inChr)   { if ($c -eq '\') { $i += 2; continue }; if ($c -eq "'")  { $inChr = $false }; $i++; continue }
        if ($c -eq '/' -and $d -eq '/') { $inLine  = $true; $i += 2; continue }
        if ($c -eq '/' -and $d -eq '*') { $inBlock = $true; $i += 2; continue }
        if ($c -eq '"')  { $inStr = $true; $i++; continue }
        if ($c -eq "'")  { $inChr = $true; $i++; continue }
        if     ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth-- }
        $i++
    }
    if ($depth -ne 0) { $bad += ('{0}: net brace depth {1} (nonzero = possible truncation)' -f $_.Name, $depth) }
}

if ($bad) {
    Write-Host "WARNING - brace imbalance:" -ForegroundColor Yellow
    $bad | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow }
    exit 1
}
Write-Host "Brace check OK (all source files balanced)."
exit 0
