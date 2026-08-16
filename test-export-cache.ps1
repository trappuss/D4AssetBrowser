<#
    Companion to "Test - Export Cache.bat". Two jobs, chosen with -Mode:

      Prepare  pin the export settings in data\D4AssetBrowser.ini so the two passes
               are measuring the same work
      Report   parse the "export perf:" line out of each pass's stderr capture and
               print the comparison

    Kept out of the .bat because batch cannot parse that line reliably: it carries a
    timestamp prefix, an em dash, and parenthesised fields, so token-splitting on
    spaces gives a different answer depending on how many models were exported.

    Exit code from Report: 0 = the cache demonstrably worked, 1 = it did not, 2 = the
    run could not be measured (no perf line - usually the export was never started).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Check','Prepare','Report')][string]$Mode,
    [string]$Ini,
    [string]$Baseline,
    [string]$Cached,
    [string]$Exe,
    [string]$Src
)

$ErrorActionPreference = 'Stop'

# The options both passes must share. Chosen to exercise the decode path hard and to
# keep the run reproducible:
#   looseTexturesAll  every map every material binds, decoded per model - the case
#                     with the most repeats, and the reason the cache exists
#   looseTextures     also writes the four composited maps, so the PNG-encode memo
#                     in writeLooseTextures() is exercised too
#   includeTex        the four-map pass needs it (the all-maps pass does not)
#   includeAnim       OFF: exportModels() forces par=1 for animation runs, and the
#   includeBaseBody   parallel pool is the interesting path - a serial run would
#   includeBaseHead   measure something the Bulk Extract user never hits
#   bothGenders       OFF: keeps the model count equal to what the preset matched
#   bulk/onlyNew      OFF: pass 2 must re-export everything, not skip it all
$Pinned = [ordered]@{
    'export/includeTex'       = 'true'
    'export/looseTextures'    = 'true'
    'export/looseTexturesAll' = 'true'
    'export/includeAnim'      = 'false'
    'export/includeBaseBody'  = 'false'
    'export/includeBaseHead'  = 'false'
    'export/bothGenders'      = 'false'
    'bulk/onlyNew'            = 'false'
    'bulk/parallel'           = '-1'
}

function Set-IniValues {
    param([string]$Path, [System.Collections.Specialized.OrderedDictionary]$Values)

    # QSettings' IniFormat: [group] sections, key=value, groups may be absent.
    $lines = if (Test-Path -LiteralPath $Path) { @(Get-Content -LiteralPath $Path) } else { @() }

    foreach ($entry in $Values.GetEnumerator()) {
        $group, $key = $entry.Key -split '/', 2
        $val = $entry.Value

        $secStart = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match "^\s*\[$([regex]::Escape($group))\]\s*$") { $secStart = $i; break }
        }
        if ($secStart -lt 0) {
            # No such group yet - append it with the key.
            if ($lines.Count -and $lines[-1] -ne '') { $lines += '' }
            $lines += "[$group]"
            $lines += "$key=$val"
            continue
        }
        # Where does this section end (next [header] or EOF)?
        $secEnd = $lines.Count
        for ($i = $secStart + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^\s*\[.+\]\s*$') { $secEnd = $i; break }
        }
        # Back up over the blank line(s) that separate this section from the next, so an appended
        # key lands with its own group rather than after the gap. Cosmetic, but this file is the
        # user's real settings and the .bat restores it - it should come back looking untouched.
        while ($secEnd - 1 -gt $secStart -and $lines[$secEnd - 1] -match '^\s*$') { $secEnd-- }
        $found = $false
        for ($i = $secStart + 1; $i -lt $secEnd; $i++) {
            if ($lines[$i] -match "^\s*$([regex]::Escape($key))\s*=") {
                $lines[$i] = "$key=$val"; $found = $true; break
            }
        }
        if (-not $found) {
            $lines = @($lines[0..($secEnd - 1)]) + @("$key=$val") +
                     @(if ($secEnd -lt $lines.Count) { $lines[$secEnd..($lines.Count - 1)] })
        }
    }

    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Set-Content -LiteralPath $Path -Value $lines -Encoding UTF8
}

function Get-PerfLine {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    # Last one wins: an app session can contain more than one export, and the one the
    # tester meant is the one they did last before closing.
    # No -Encoding: the capture is whatever the app's toLocal8Bit() produced (a Windows code page),
    # so forcing UTF-8 would mangle the em dash. Every field parsed below is ASCII, and the regex
    # steps over the dash with .*?, so a substituted character cannot affect the result.
    $line = Select-String -LiteralPath $Path -Pattern 'export perf:' | Select-Object -Last 1
    if (-not $line) { return $null }
    $t = $line.Line
    $m = [regex]::Match($t,
        # The trailing "[cache ...]" is OPTIONAL on purpose: a binary built before the kill-switch
        # went in does not emit it. Requiring it turned "your exe is stale" into "could not measure",
        # which is the least useful thing this script could have said.
        'export perf:\s*(?<models>\d+) model\(s\) in (?<ms>\d+) ms.*?decodes (?<miss>\d+), served from cache (?<hit>\d+) \((?<reuse>\d+)% reuse, (?<mb>\d+) MB[^\r\n]*?(\[cache (?<state>[^\]]+)\])?\s*$')
    if (-not $m.Success) { return $null }
    [pscustomobject]@{
        Models = [int]$m.Groups['models'].Value
        Ms     = [int]$m.Groups['ms'].Value
        Miss   = [int]$m.Groups['miss'].Value
        Hit    = [int]$m.Groups['hit'].Value
        Reuse  = [int]$m.Groups['reuse'].Value
        MB     = [int]$m.Groups['mb'].Value
        # "OFF (D4_NO_TEXCACHE)" is useful in the raw log line but overflows the column here.
        # Absent entirely on a pre-kill-switch build - reported as "?" so the mismatch is visible.
        State  = $(if (-not $m.Groups['state'].Success) { '?' }
                   elseif ($m.Groups['state'].Value -like 'OFF*') { 'OFF' }
                   else { $m.Groups['state'].Value })
        Raw    = $t.Trim()
    }
}

switch ($Mode) {

'Check' {
    # Is the binary actually built from the source that is on disk? Every failure this script can
    # report has "you did not rebuild" as its most likely cause, and that is cheap to rule out here
    # instead of after twenty minutes of exporting.
    if (-not (Test-Path -LiteralPath $Exe)) { Write-Host '  No exe.' -ForegroundColor Red; exit 1 }
    $exeTime = (Get-Item -LiteralPath $Exe).LastWriteTime
    $newest  = Get-ChildItem -LiteralPath $Src -Recurse -File -Include *.cpp,*.h |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $newest) { exit 0 }
    if ($newest.LastWriteTime -gt $exeTime) {
        Write-Host ''
        Write-Host '  STOP - the exe is older than the source.' -ForegroundColor Red
        Write-Host ('    exe     {0:yyyy-MM-dd HH:mm:ss}' -f $exeTime)
        Write-Host ('    newest  {0:yyyy-MM-dd HH:mm:ss}  {1}' -f $newest.LastWriteTime, $newest.Name)
        Write-Host ''
        Write-Host '  Run rebuild.bat first. Without it the test measures the previous build:'
        Write-Host '  the D4_NO_TEXCACHE kill-switch will not exist, so the "baseline" pass runs'
        Write-Host '  WITH the cache and both halves come out identical.'
        exit 1
    }
    Write-Host ('  Binary is current (built {0:HH:mm:ss}, newest source {1:HH:mm:ss}).' -f $exeTime, $newest.LastWriteTime) -ForegroundColor DarkGray
    exit 0
}

'Prepare' {
    if (-not $Ini) { Write-Host '  -Ini is required for Prepare.' -ForegroundColor Red; exit 1 }
    Set-IniValues -Path $Ini -Values $Pinned
    Write-Host '  Export settings pinned for the test:' -ForegroundColor Cyan
    foreach ($e in $Pinned.GetEnumerator()) { Write-Host ('    {0,-26} {1}' -f $e.Key, $e.Value) }
    exit 0
}

'Report' {
    $a = Get-PerfLine $Baseline
    $b = Get-PerfLine $Cached

    Write-Host ''
    Write-Host '  ===========================================================' -ForegroundColor Cyan
    Write-Host '   RESULT' -ForegroundColor Cyan
    Write-Host '  ===========================================================' -ForegroundColor Cyan

    if (-not $a -or -not $b) {
        Write-Host ''
        Write-Host '   Could not measure.' -ForegroundColor Yellow
        if (-not $a) { Write-Host '     No "export perf:" line in the BASELINE capture.' }
        if (-not $b) { Write-Host '     No "export perf:" line in the CACHED capture.' }
        Write-Host ''
        Write-Host '   Most likely the export was never started, or it was started from a'
        Write-Host '   tab that does not go through exportModels(). Only MODEL exports are'
        Write-Host '   instrumented - a Textures-mode bulk run will not print this line.'
        exit 2
    }

    # A mismatched model count means the two passes did different work, and every
    # number below would be a comparison of two different things.
    if ($a.Models -ne $b.Models) {
        Write-Host ''
        Write-Host ("   The two passes exported different model counts ({0} vs {1})." -f $a.Models, $b.Models) -ForegroundColor Yellow
        Write-Host '   Nothing below is a fair comparison. Re-run and use the same preset,'
        Write-Host '   the same selection and the same output folder in both passes.'
    }

    $fmt = '   {0,-22}{1,14}{2,14}'
    $fair = ($a.Models -eq $b.Models)
    Write-Host ''
    Write-Host ($fmt -f '', 'BASELINE', 'CACHED')
    Write-Host ($fmt -f 'cache state', $a.State, $b.State)
    Write-Host ($fmt -f 'models exported', $a.Models, $b.Models)
    Write-Host ($fmt -f 'wall clock (ms)', $a.Ms, $b.Ms)
    Write-Host ($fmt -f 'textures decoded', $a.Miss, $b.Miss)
    Write-Host ($fmt -f 'served from cache', $a.Hit, $b.Hit)
    Write-Host ($fmt -f 'reuse', "$($a.Reuse)%", "$($b.Reuse)%")
    Write-Host ($fmt -f 'MB not re-decoded', $a.MB, $b.MB)

    Write-Host ''
    # An unequal-work comparison must not be able to report a pass, however good the numbers look.
    $ok = $fair

    # THE most important check here. If the baseline pass served anything from the cache then
    # D4_NO_TEXCACHE never reached the binary, both halves ran cached, and the comparison below is
    # two identical runs - which will look like "the cache does nothing" and is completely wrong.
    if ($a.Hit -gt 0 -or $a.State -eq 'on') {
        Write-Host '   INVALID  The BASELINE pass used the cache too.' -ForegroundColor Red
        Write-Host ('            It served {0} texture(s) from cache with D4_NO_TEXCACHE set.' -f $a.Hit)
        Write-Host '            The kill-switch is not in this binary - rebuild.bat, then re-run.'
        Write-Host '            Nothing below is an A/B comparison; both passes are the cached one.'
        Write-Host ''
        $ok = $false
    }
    if ($a.State -eq '?' -or $b.State -eq '?') {
        Write-Host '   NOTE     This build does not report its cache state, so it predates the' -ForegroundColor Yellow
        Write-Host '            kill-switch. Rebuild before trusting any of these numbers.'
        Write-Host ''
    }

    # Past this point every statement is an INTERPRETATION of the two runs. If the comparison is
    # not valid they are two samples of the same thing, and "no wall-clock win" would read as a
    # finding about the cache when it is a finding about the harness. Say nothing instead.
    if (-not $ok) {
        Write-Host '   No conclusion drawn - fix the above and re-run.' -ForegroundColor Yellow
        Write-Host ''
        Write-Host '   Raw lines:' -ForegroundColor DarkGray
        Write-Host ('     ' + $a.Raw) -ForegroundColor DarkGray
        Write-Host ('     ' + $b.Raw) -ForegroundColor DarkGray
        exit 1
    }

    if ($b.Hit -le 0) {
        Write-Host '   FAIL  The cached pass served nothing from the cache.' -ForegroundColor Red
        Write-Host '         Either the assets share no textures, or the pool workers are not'
        Write-Host '         joining the scope - that exact bug shipped once already.'
        $ok = $false
    } else {
        $avoided = $a.Miss - $b.Miss
        Write-Host ('   Decodes avoided: {0} of {1} ({2}%).' -f $avoided, $a.Miss,
                    [math]::Round(100.0 * $avoided / [math]::Max(1, $a.Miss), 1)) -ForegroundColor Green
    }

    if ($a.Ms -gt 0 -and $b.Ms -gt 0) {
        $saved = $a.Ms - $b.Ms
        $pct   = [math]::Round(100.0 * $saved / $a.Ms, 1)
        if ($saved -gt 0) {
            Write-Host ('   {0} ms faster ({1}% of the baseline run).' -f $saved, $pct) -ForegroundColor Green
        } else {
            Write-Host ('   No wall-clock win ({0} ms slower).' -f (-$saved)) -ForegroundColor Yellow
            Write-Host '   The decodes were avoided but the run did not get faster, so decoding'
            Write-Host '   is not what this export spends its time on - look at CASC reads, PNG'
            Write-Host '   encoding or disk before optimising here any further.'
            $ok = $false
        }
    }

    Write-Host ''
    Write-Host '   Raw lines:' -ForegroundColor DarkGray
    Write-Host ('     ' + $a.Raw) -ForegroundColor DarkGray
    Write-Host ('     ' + $b.Raw) -ForegroundColor DarkGray
    if ($ok) { exit 0 } else { exit 1 }
}

}
