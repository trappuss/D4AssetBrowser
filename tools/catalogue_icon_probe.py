#!/usr/bin/env python3
"""
Catalogue icon probe - why a Catalogue row is blank, answered from the app's OWN CACHES.

NO REBUILD, NO APP LAUNCH. It reads three files the app has already written into
build\\release\\data and answers in about a second:

    coretoc_v3.bin          every SNO: group -> [(sno, name)]      (148k group-44 textures)
    store_products_v4.json  every product: name, season, art[], cart[], kids[], enc
    icon_index_v4.json      frames: handle -> [atlasSno, ...]
    appearance_meta_v23.json  iconNames: handle -> appearance name   (route 3, optional)

WHY IT EXISTS. The same question - "why is this row blank, and what would fix it" - was being
answered by editing C++, rebuilding, launching the app and clicking File > Icon audit. Minutes
per iteration, for a question that is really "what do these three files say". Three wrong
policies were shipped into that loop before this existed. Now a policy is proposed, measured
and rejected in one run.

WHAT IT CANNOT DO, stated so the numbers are not over-read:
  * It cannot tell whether a texture payload DECODES, only that the SNO exists and is indexed.
    A BC-decode fault or a degenerate UV rect still blanks a row the probe calls renderable;
    IconIndex logs one warning per atlas when that happens, so the app log is the cross-check.
  * It cannot see TACT locking. The in-app audit measured that at 12 handles and 1 row, so the
    error this introduces is under a tenth of a percent - but it is an error, not zero.
  * The caches are a snapshot. After a game patch, launch the app once to refresh them, then
    re-run this.

THE TWO ROUTES A ROW CAN GET A PICTURE BY, which is the whole point:
  1. BY NAME   CatalogueTab::cardImage() then heroImage() derive a group-44 texture name from
               the product's SNO name. This is what the grid does today.
  2. BY HANDLE the product's authored art handles, accepting only a handle NO other product
               carries - a shared class or promo banner is declined rather than repeated.
  3. BY CONTENT the icon of the first thing the product contains. A bundle whose own record holds
               nothing but its class banner still sells a chest piece, and that piece has an icon.
  4. BY PORTRAIT the payload actor's own hPortraitImage. NOT MEASURED HERE: it needs the d4data
               actor files, which this probe deliberately does not read - it answers from the
               app's caches alone. The app has this route; the "no art handles at all" bucket
               below is therefore an UPPER bound on what stays blank, not the final figure. The
               in-app File > Icon audit is what measures it.

               Route 3 is measured here through appearance_meta's iconNames table (3,559 entries)
               while the app resolves it through appearancesFor() -> iconFor() over the full
               11,516-appearance graph. What this prints is a FLOOR, not the app's coverage.

Usage:  python catalogue_icon_probe.py [path-to-data-dir]   (default: build\\release\\data)
"""
import atexit, json, os, re, struct, sys, collections

# ── Load ────────────────────────────────────────────────────────────────────────────────────────

def read_coretoc(path):
    """Format taken from SnoIndex::loadFromCache, not guessed: magic, sig, then per group
    (id, count) and per entry (sno u32, nameLen u16, name utf8)."""
    b = open(path, "rb").read()
    magic, siglen = struct.unpack_from("<II", b, 0)
    if magic != 0x544F4331:
        raise SystemExit("coretoc: bad magic 0x%08X - cache format changed" % magic)
    off = 8 + siglen
    groups, = struct.unpack_from("<I", b, off); off += 4
    out = {}
    for _ in range(groups):
        gid, n = struct.unpack_from("<iI", b, off); off += 8
        d = {}
        for _ in range(n):
            sno, = struct.unpack_from("<i", b, off); off += 4
            nl,  = struct.unpack_from("<H", b, off); off += 2
            d[b[off:off+nl].decode("utf-8", "replace")] = sno
            off += nl
        out[gid] = d
    if off != len(b):
        raise SystemExit("coretoc: consumed %d of %d bytes - format drift" % (off, len(b)))
    return out

DATA = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "release", "data")

# ── The report goes to a FILE as well as the console ─────────────────────────────────────────────
# Every other diagnostic in this project writes beside the exe - preset_audit.txt, asset_health.txt,
# icon_audit.txt - and Help > Diagnostic output lists them there. This one printed to stdout only,
# so its answer lived in a console window until that window closed, and then had to be produced a
# second time. Written to both: the console for the person who ran it, the file for everything else.
REPORT = os.path.join(DATA, "catalogue_icon_probe.txt")


class _Tee:
    """Writes to the console and the report at once. Only write/flush are used by print()."""

    def __init__(self, *streams):
        self.streams = streams

    def write(self, s):
        for st in self.streams:
            st.write(s)

    def flush(self):
        for st in self.streams:
            try:
                st.flush()
            except Exception:
                pass


try:
    _fh = open(REPORT, "w", encoding="utf-8")
except OSError as e:               # read-only folder, or DATA does not exist yet
    _fh = None
    print("note: could not open %s (%s) - console only" % (REPORT, e))
if _fh:
    sys.stdout = _Tee(sys.__stdout__, _fh)


def _finish():
    # Registered with atexit so it also runs on the `raise SystemExit` paths above, where the
    # file holds the reason the probe stopped and is the more useful half of the output.
    if not _fh:
        return
    try:
        sys.stdout = sys.__stdout__
        _fh.close()
        print("\nreport written to %s" % os.path.abspath(REPORT))
    except Exception:
        pass


atexit.register(_finish)

# ── Finding the caches without pinning their version ────────────────────────────────────────────
# The app writes store_products_v4.json today and will write v5 the first time that record shape
# changes. A probe that names v4 stops working at exactly the moment it is most needed - the patch
# that changed something - and it fails looking like the DATA in it is missing rather than like the
# probe is stale. So each cache is found by its stem and the HIGHEST version present is used.
def newest(stem, ext):
    pat = re.compile(r"^" + re.escape(stem) + r"_v(\d+)\." + re.escape(ext) + r"$", re.I)
    best, bestv = None, -1
    try:
        names = os.listdir(DATA)
    except OSError as e:
        raise SystemExit("cannot read %s: %s" % (DATA, e))
    for n in names:
        m = pat.match(n)
        if m and int(m.group(1)) > bestv:
            best, bestv = os.path.join(DATA, n), int(m.group(1))
    return best

_toc = newest("coretoc", "bin")
_spp = newest("store_products", "json")
_ico = newest("icon_index", "json")
missing = [s for s, p in (("coretoc_v*.bin", _toc), ("store_products_v*.json", _spp),
                          ("icon_index_v*.json", _ico)) if not p]
if missing:
    raise SystemExit("missing %s in %s\nLaunch the app once so it writes its caches, then re-run."
                     % (", ".join(missing), os.path.abspath(DATA)))

toc    = read_coretoc(_toc)
sp     = json.load(open(_spp, encoding="utf-8"))
frames = json.load(open(_ico, encoding="utf-8"))["frames"]

# Route 3 is optional - absent, the probe still answers routes 1 and 2 and says so.
_amp = newest("appearance_meta", "json")
icon_by_name = {}
if _amp:
    icon_by_name = {v.lower(): int(k)
                    for k, v in json.load(open(_amp, encoding="utf-8")).get("iconNames", {}).items()}
print("caches: %s" % ", ".join(os.path.basename(p) for p in (_toc, _spp, _ico, _amp) if p))

# Group id by NAME is not available here (that map lives in SnoIndex), so 44 is used with a
# sanity check rather than blind faith: Texture is by far the largest group in the file.
# The CoreTOC cache carries no group NAMES, so the id cannot be looked up the way the app does it
# (SnoIndex::groupIdByName). Rather than trust the literal, it is verified against the data: the
# Texture group is the one whose members are overwhelmingly named 2DUI_/2DInventory_/*_Normal, and
# if group 44 ever stops looking like that the probe says so instead of reporting a false zero.
TEX = 44
def looks_like_textures(d):
    if len(d) < 10000:
        return False
    hits = sum(1 for n in list(d)[:4000]
               if n.lower().startswith(("2dui_", "2dinventory_")) or n.lower().endswith(
                   ("_normal", "_mask", "_alpha", "_diffuse")))
    return hits >= 100
if not looks_like_textures(toc.get(TEX, {})):
    alt = [g for g, d in toc.items() if looks_like_textures(d)]
    if len(alt) != 1:
        raise SystemExit("group %d no longer looks like Texture (%d entries) and %d candidates "
                         "matched - the SNO groups were renumbered; check SnoIndex::groupIdByName."
                         % (TEX, len(toc.get(TEX, {})), len(alt)))
    print("NOTE: Texture is group %d in this build, not %d - using the one the data supports."
          % (alt[0], TEX))
    TEX = alt[0]
tex_lower = {k.lower(): v for k, v in toc[TEX].items()}

prods   = {p["sno"]: p for p in sp["products"]}
bundles = set(sp["bundles"])
kids    = {k for p in sp["products"] for k in p.get("kids", [])}
locked  = {p["sno"] for p in sp["products"] if p.get("enc")}
loose   = {p["sno"] for p in sp["products"]
           if p["sno"] not in bundles and p["sno"] not in kids and p["sno"] not in locked}
# The rows the list actually shows. NOT children - a child is reachable only by opening a bundle.
rows = sorted(bundles | loose | locked)

def season_of(p):
    return p.get("sname") or (("season %d" % p["season"]) if p.get("season") else "(no season)")

# ── Route 1: by name ────────────────────────────────────────────────────────────────────────────
# cardImage()'s four candidates then heroImage()'s six, in the order the tab tries them.

def bare_of(name):
    return name[7:] if name.lower().startswith("bundle_") else name

def thumb_candidates(bare):
    c = ["2DUI_Bundle_" + bare, "2DInventory_Bundle_" + bare,
         "2DInventory_" + bare, "2DUI_" + bare]
    for sfx in ("_background", "_WebImage", "_details"):
        c.append("2DUI_Bundle_" + bare + sfx)
    for sfx in ("_background", "_WebImage", "_details"):
        c.append("2DUI_" + bare + sfx)
    return c

# uiArtCandidates() - what shopTextures() and the "Has icon" FILTER use. Wider than the
# thumbnail's list, which is why a row can pass the filter and still draw blank.
def filter_candidates(bare):
    sfx = ("", "_details", "_background", "_WebImage", "_icons")
    c = ["2DUI_Bundle_" + bare + s for s in sfx]
    c.append("2DInventory_Bundle_" + bare)
    c += ["2DUI_" + bare + s for s in sfx]
    c.append("2DInventory_" + bare)
    return c

def name_hit(cands):
    for c in cands:
        if c.lower() in tex_lower:
            return c
    return None


# ── The derived scan, as CatalogueTab::ensureDerivedArt does it ──────────────────────────────────
# The templates are a guess about filenames; the game has already outgrown them (2DUI_S12_,
# 2DUI_BP_S13_, 2DUI_RL_S08_ are real prefixes that were never in the list). The app therefore also
# finds art structurally, and so must this probe - otherwise it under-reports the app and a season
# whose art only the scan can reach looks like a regression.
#
#   * the product name appears as a whole TOKEN SPAN  ("stor001" never matches "stor0010")
#   * that span is PRECEDED by at least one token     (shop art is <prefix>_<product>;
#                                                      a material map is <product>_color)
#   * and the texture is one the icon index treats as an ATLAS - UI art, not a material or a
#     VFX gradient. This is IconIndex::isAtlas, and it needs no filename knowledge at all.
_atlas_snos = {f[0] for f in frames.values()}


def build_derived():
    stems = set()
    for s_ in rows:
        p = prods.get(s_)
        if not p:
            continue
        b = bare_of(p["name"]).lower()
        if "_" in b:                      # a one-token stem would match far too much
            stems.add(b)
    out = collections.defaultdict(list)
    for nm, sno in toc[TEX].items():
        if sno not in _atlas_snos:
            continue
        tk = nm.lower().split("_")
        for i in range(1, len(tk)):
            span = tk[i]
            for j in range(i + 1, len(tk) + 1):
                if span in stems:
                    out[span].append((sno, nm))
                if j == len(tk):
                    break
                span += "_" + tk[j]
    # Shortest name first, as the app orders them.
    for k in out:
        out[k].sort(key=lambda t: (len(t[1]), t[1].lower()))
    return out


derived_art = build_derived()


def derived_hit(product_name):
    b = bare_of(product_name).lower()
    got = derived_art.get(b)
    return got[0][1] if got else None

# ── Route 2: by handle ──────────────────────────────────────────────────────────────────────────
# A handle's GLOBAL usage across every product is what separates a product's own art from a
# shared class/promo banner. Measured, not assumed: Bundle_HArmor_bar_stor276 carries
# 3712417547 (on 204 products) and 3461930798 (on exactly one). The first is the Barbarian
# banner, the second is the bundle's own card - and "first non-zero" picks the banner.
usage = collections.Counter()
for p in sp["products"]:
    for h in (p.get("art") or []) + (p.get("cart") or []):
        if h and str(h) in frames:
            usage[h] += 1

def renderable(p):
    return [h for h in (p.get("cart") or []) + (p.get("art") or []) if h and str(h) in frames]

def pol_first(hs):   return hs[0] if hs else 0
def pol_least(hs):   return min(hs, key=lambda h: (usage[h], h)) if hs else 0
def pol_unique(hs):
    b = pol_least(hs)
    return b if b and usage[b] == 1 else 0

POLICIES = [
    ("first non-zero (today's strip)", pol_first),
    ("least-shared handle",            pol_least),
    ("least-shared, unique only",      pol_unique),
]

# ── Report ──────────────────────────────────────────────────────────────────────────────────────

print("CATALOGUE ICON PROBE")
print("=" * 68)
print("%d rows  (%d bundles, %d loose, %d locked)  -  %d textures  -  %d frames"
      % (len(rows), len(bundles), len(loose), len(locked), len(tex_lower), len(frames)))
print()

by_name, by_derived = {}, {}
for s in rows:
    p = prods.get(s)
    if p:
        by_name[s] = name_hit(thumb_candidates(bare_of(p["name"])))
        if not by_name[s]:
            by_derived[s] = derived_hit(p["name"])
            if by_derived[s]:
                by_name[s] = by_derived[s]      # the app takes it; so does every count below
renders = sum(1 for v in by_name.values() if v)
filt_only = [s for s in rows
             if prods.get(s) and not by_name.get(s)
             and name_hit(filter_candidates(bare_of(prods[s]["name"])))]
print("ROUTE 1 - BY NAME (templates, then the structural scan)")
print("  %d of %d rows resolve a texture name.  %d blank." % (renders, len(rows), len(rows) - renders))
print("    %d from the filename templates, %d only the structural scan can reach."
      % (renders - sum(1 for v in by_derived.values() if v),
         sum(1 for v in by_derived.values() if v)))
print("  %d more are found only by the FILTER's wider list: they pass \"Has icon\","
      % len(filt_only))
print("  take a row, and still draw blank.")
if filt_only:
    tmpl = collections.Counter()
    for s in filt_only:
        b = bare_of(prods[s]["name"])
        hit = name_hit(filter_candidates(b))
        tmpl[hit.replace(b, "*") if b else hit] += 1
    for t, c in tmpl.most_common():
        print("      %4d rows  %s" % (c, t))
print()

print("ROUTE 2 - BY HANDLE (the product's own art), with the evidence for the policy")
print("  DISTINCT/PICKS near 1.00 means each row shows its own picture. A low ratio means one")
print("  image stamped across many rows, which is worse than a blank - it looks like real data.")
print()
print("  %-32s %7s %9s %9s %7s  %s" % ("policy", "picks", "distinct", "per row", "worst", "verdict"))
best = None
for name, pick in POLICIES:
    use, ex = collections.Counter(), {}
    for s in rows:
        p = prods.get(s)
        if not p:
            continue
        h = pick(renderable(p))
        if h:
            use[h] += 1
            ex.setdefault(h, p["name"])
    n, d = sum(use.values()), len(use)
    w = use.most_common(1)[0][1] if use else 0
    r = d / n if n else 0.0
    verdict = "USABLE" if r > 0.99 else ("marginal" if r > 0.9 else "REPEATS - unusable")
    print("  %-32s %7d %9d %9.2f %7d  %s" % (name, n, d, r, w, verdict))
    if r > 0.99 and (best is None or n > best[1]):
        best = (name, n, pick)
    if w > 1:
        for h, c in use.most_common(3):
            if c > 1:
                print("        %5d rows share handle %-12d e.g. %s" % (c, h, ex[h]))
print()

if not best:
    print("No handle policy is safe. Do not wire the grid to handles.")
    sys.exit(0)

name, _, pick = best
gained = [s for s in rows if prods.get(s) and not by_name.get(s) and pick(renderable(prods[s]))]
print("ROUTE 2 COVERAGE - rows route 1 cannot name, filled by '%s'" % name)
print("  %d rows, every one with an image no other row uses." % len(gained))
print()
print("  %-22s %6s %8s %8s %8s" % ("season", "rows", "by name", "by handle", "still blank"))
bys = collections.defaultdict(lambda: [0, 0, 0])
for s in rows:
    p = prods.get(s)
    if not p:
        continue
    k = season_of(p)
    bys[k][0] += 1
    if by_name.get(s):
        bys[k][1] += 1
    elif pick(renderable(p)):
        bys[k][2] += 1
for k in sorted(bys):
    t, r, g = bys[k]
    print("  %-22s %6d %8d %8d %8d" % (k, t, r, g, t - r - g))

# ── Route 3, over whatever routes 1 and 2 left blank ─────────────────────────────────────────────
print()
if not icon_by_name:
    print("ROUTE 3 - skipped: appearance_meta_v23.json not in %s" % DATA)
    raise SystemExit(0)

def content_icon(p):
    """First content's icon. contentSnos' rule: a product with no children IS its own content."""
    for k in (p.get("kids") or [None]):
        c = prods.get(k) if k is not None else p
        if not c:
            continue
        h = icon_by_name.get((c.get("pname") or "").lower())
        if h and str(h) in frames:
            return h
    return 0

still = [s for s in rows
         if prods.get(s) and not by_name.get(s) and not pick(renderable(prods[s]))]
use3, fixed3 = collections.Counter(), []
for s in still:
    h = content_icon(prods[s])
    if h:
        use3[h] += 1
        fixed3.append(s)
print("ROUTE 3 - BY CONTENT, over the %d rows routes 1 and 2 leave blank" % len(still))
print("  fills %d rows, %d distinct images, worst cluster %d   (a FLOOR - see the header)"
      % (len(fixed3), len(use3), use3.most_common(1)[0][1] if use3 else 0))
b3 = collections.Counter(season_of(prods[s]) for s in fixed3)
for k in sorted(b3):
    print("      %-22s %5d" % (k, b3[k]))

done = set(fixed3)
rest = [s for s in still if s not in done]
cat = collections.Counter()
for s in rest:
    p = prods[s]
    if p.get("enc"):
        cat["locked - TACT-encrypted record, genuinely unknowable"] += 1
    elif not p.get("art") and not p.get("cart"):
        cat["no art handles at all - the app's portrait route may still reach these"] += 1
    else:
        cat["has art, but every handle is shared"] += 1
print()
print("  still blank after the three routes this probe can measure: %d" % len(rest))
for k, v in cat.most_common():
    print("      %-58s %5d" % (k, v))
print()
print("  The app has a fourth route (the payload actor's portrait) that needs the d4data actor")
print("  files, so it is not counted here - run File > Icon audit in the app for the real total.")
