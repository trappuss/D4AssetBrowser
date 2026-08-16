<#
    Sets the release version in every file that carries one.

    Driven by "Set Version.bat", which exists only to launch this. The logic lives
    here rather than inside the bat because the first attempt embedded it in a
    powershell -Command string and the two patterns containing a quote character
    silently failed to match while the one without it worked. Nothing about a
    version bump is hard enough to justify fighting cmd's escaping rules, and a
    regex that quietly matches nothing is the worst possible failure for a script
    whose whole job is to report what it found.

    WHERE THE VERSION LIVES  (all three, or they drift)
      src\main.cpp     setApplicationVersion("X.Y.Z")   <- the one that MATTERS
      CMakeLists.txt   project(... VERSION X.Y.Z ...)
      vcpkg.json       "version": "X.Y.Z"

    main.cpp is authoritative at runtime: package-release.bat and Release Smoke
    Test.bat both read the zip filename out of it, and it is what Help > About
    shows. The other two are metadata, which is exactly why they rot unnoticed -
    when this was written main.cpp said 2.2.0 while both others still said 2.1.0.

    All three are printed BEFORE anything is written, so pre-existing disagreement
    is visible rather than quietly flattened, and read back from disk afterwards so
    the edit is verified rather than assumed.

    No backup files: git already holds the old value, and a .bak beside a source
    file is one more thing to forget to delete.
#>

[CmdletBinding()]
param(
    # X.Y.Z. Omitted, the script prompts. Blank at the prompt cancels.
    [string] $Version
)

$ErrorActionPreference = 'Stop'

# Paths are resolved against $PSScriptRoot and used absolutely throughout. Set-Location
# would NOT be enough: [System.IO.File] is .NET and resolves relative paths against the
# PROCESS working directory, which PowerShell's Set-Location does not change. Test-Path
# would agree with one and ReadAllText with the other - a mismatch that only appears when
# the script is launched from some other directory, and then reads or writes the wrong file.

# Group 2 is the version itself; groups 1 and 3 are the surrounding text that has
# to survive the rewrite untouched. Every pattern is anchored on enough context
# that it cannot match anything else in the file - verified: each matches exactly
# once, and rewriting changes one line with no change in line count.
$targets = @(
    @{ Path = 'src\main.cpp'
       Label = 'src\main.cpp  '
       Pattern = '(setApplicationVersion\(")([^"]*)("\))' }

    @{ Path = 'CMakeLists.txt'
       Label = 'CMakeLists.txt'
       # cmake_minimum_required(VERSION 3.21) on line 1 is NOT matched: VERSION has
       # to start the line there, and it does not.
       Pattern = '(?m)^(\s*VERSION\s+)(\d+\.\d+\.\d+)()' }

    @{ Path = 'vcpkg.json'
       Label = 'vcpkg.json    '
       Pattern = '(?m)^(\s*"version"\s*:\s*")([^"]*)(")' }

    # res\app.rc carries the version TWICE, in two spellings, and both drive what Explorer shows
    # under Properties > Details. The numeric one is comma-separated with a trailing build field,
    # because that is what VERSIONINFO requires — hence Format, rather than a special case in the
    # rewrite logic. Comparison always happens in dotted form, so all targets stay comparable.
    # The trailing ",0" (the unused build field) MUST live in group 3, not in the pattern's
    # untracked text. It was written as a bare ",0" once: the match consumed it, the replacement
    # rebuilt only groups 1-3, and it silently vanished — leaving "#define VER_NUM 2,2,1", a
    # three-field FILEVERSION where the resource compiler wants four. Anything the replacement has
    # to preserve belongs in a capture group.
    @{ Path = 'res\app.rc'
       Label = 'res\app.rc VER_NUM'
       Pattern = '(#define VER_NUM\s+)(\d+,\d+,\d+)(,\d+)'
       Format = 'commas' }

    @{ Path = 'res\app.rc'
       Label = 'res\app.rc VER_STR'
       Pattern = '(#define VER_STR\s+")([^"]*)(")' }
)

# A target's on-disk spelling <-> the canonical X.Y.Z used for comparison and for the prompt.
function ConvertTo-TargetFormat {
    param([string] $Version, [string] $Format)
    if ($Format -eq 'commas') { return ($Version -replace '\.', ',') }
    return $Version
}
function ConvertFrom-TargetFormat {
    param([string] $Raw, [string] $Format)
    if ($Format -eq 'commas') { return ($Raw -replace ',', '.') }
    return $Raw
}

# UTF-8 with NO BOM, matching how these files are stored today. main.cpp has 13
# lines of non-ASCII in its comments, so the encoding is not incidental, and adding
# a BOM would surface as a whole-file diff.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Show-Versions {
    param([string] $WriteVersion)

    $found = @()
    $failed = $false

    foreach ($t in $targets) {
        $full = Join-Path $PSScriptRoot $t.Path

        if (-not (Test-Path -LiteralPath $full)) {
            Write-Host ("  [X] {0}  NOT FOUND" -f $t.Label) -ForegroundColor Red
            $failed = $true
            continue
        }

        $re   = [regex] $t.Pattern
        $text = [System.IO.File]::ReadAllText($full)
        $m    = $re.Match($text)

        if (-not $m.Success) {
            Write-Host ("  [X] {0}  no version found - the file format changed, edit it by hand" -f $t.Label) -ForegroundColor Red
            $failed = $true
            continue
        }

        if ($WriteVersion) {
            # Count 1: rewrite the first match only. Belt and braces - each pattern
            # already matches exactly once - but a future edit to one of these files
            # must not turn this into a silent multi-replace.
            $written = ConvertTo-TargetFormat -Version $WriteVersion -Format $t.Format
            $text = $re.Replace($text, ('${1}' + $written + '${3}'), 1)
            [System.IO.File]::WriteAllText($full, $text, $utf8NoBom)

            # Read back from DISK rather than trusting the in-memory string, so what
            # is printed is what the next build will actually compile.
            $text = [System.IO.File]::ReadAllText($full)
            $m = $re.Match($text)
        }

        # Compared and displayed in canonical X.Y.Z however the file spells it, so the
        # "do they all agree" check stays meaningful across mixed formats.
        $canon = ConvertFrom-TargetFormat -Raw $m.Groups[2].Value -Format $t.Format
        $found += $canon
        Write-Host ("      {0}  {1}" -f $t.Label, $canon)
    }

    if ($failed) { return $false }

    $unique = @($found | Sort-Object -Unique)
    Write-Host ''
    if ($unique.Count -gt 1) {
        Write-Host ("  [!] These do not agree: {0}" -f ($unique -join ', ')) -ForegroundColor Yellow
    } else {
        Write-Host ("  [OK] all three agree on {0}" -f $unique[0]) -ForegroundColor Green
    }
    return $true
}

Write-Host ''
Write-Host ' ============================================================'
Write-Host '  CURRENT'
Write-Host ' ============================================================'
if (-not (Show-Versions)) {
    Write-Host ''
    Write-Host '  [X] Could not read one of the files - nothing changed.' -ForegroundColor Red
    exit 1
}

Write-Host ''
if (-not $Version) {
    $Version = (Read-Host '  New version (X.Y.Z), or blank to cancel').Trim()
}
if (-not $Version) {
    Write-Host ''
    Write-Host '  Cancelled - nothing changed.'
    exit 0
}

# Validate BEFORE touching anything. A typo here propagates into the zip filename,
# the git tag and Help > About, and is first noticed by whoever downloads it.
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Host ''
    Write-Host ("  [X] '{0}' is not X.Y.Z (digits only, e.g. 2.3.0)." -f $Version) -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host ' ============================================================'
Write-Host ("  WRITING {0}" -f $Version)
Write-Host ' ============================================================'
if (-not (Show-Versions -WriteVersion $Version)) {
    Write-Host ''
    Write-Host '  [X] Write failed - see above.' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host ' ============================================================'
Write-Host '  NEXT'
Write-Host ' ============================================================'
Write-Host '    "Release Smoke Test.bat"   builds, packages and verifies the zip'
Write-Host ("    dist\D4AssetBrowser_v{0}.zip is what you upload" -f $Version)
Write-Host ''
exit 0
