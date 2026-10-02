#!/usr/bin/env python3
"""Series / collection probe -- what d4data ITSELF can tell us about collections.

Purpose: settle, from the game's own export and nothing else, whether the tool can
derive a collection tree without leaning on diablo4.dad. Three questions, three passes:

  A  LABEL VOCABULARY. Every szLabel used across every StringList, with a per-prefix
     breakdown. This is the one that can surprise us: if the data carries a grouping
     field richer than "Series" (a Set, a Category, a Source), this is what finds it.
     We are NOT looking for "Series" here -- we are looking for what else is there.

  B  SERIES HARVEST. Every StringList carrying a Series row, dumped raw so the
     normalisation rules are written against real strings rather than assumed ones.

  C  SHOP / SEASON / ACQUISITION LINKAGE. Walk StoreProduct and record, per payload
     sno: the product, its season, its eType, its release branch, and whether it has an
     arRequiresOwning edge -- which StoreProductIndex.cpp already calls the only source
     for "this was never sold, it came with the Season 3 premium pass". Then score each
     series. eType is the one field whose labels are NOT in the data; this dumps its
     distribution so a Promotional rule can be written against measured values or
     abandoned on them, rather than assumed either way.

Reads only. Writes three files into "Claude outputs". Does not build, does not launch
the app, does not touch the game install.
"""

import concurrent.futures as cf
import json
import os
import re
import sys
from collections import Counter, defaultdict

# The 13 payload pointers a StoreProduct leaf can carry, taken from StoreProductIndex.cpp
# rather than retyped from memory -- a missing one would silently understate shop coverage.
PAYLOAD_FIELDS = (
    "snoItemTransmog", "snoMount", "snoEmote", "snoMarkingShape", "snoJewelry",
    "snoEmblem", "snoHeadstone", "snoTownPortal", "snoHairStyle", "snoFacialHair",
    "snoCompanion", "snoPower", "snoDyeArmor",
)

# "<Name>" <Class> Equipment -- the per-class child rows. Class list is discovered from
# the data in pass B and only used to SPLIT; an unknown class simply stays a parent row.
CLASS_SUFFIX = re.compile(r'^(?P<base>.*?)\s+(?P<cls>[A-Z][a-z]+)\s+Equipment$')


def find_dirs(root):
    """Locate the two folders we need by STEM, not by version or depth."""
    sl = prd = None
    for dirpath, dirnames, _ in os.walk(root):
        base = os.path.basename(dirpath)
        if base == "StringList" and sl is None:
            sl = dirpath
        elif base == "StoreProduct" and prd is None:
            prd = dirpath
        if sl and prd:
            break
        # Do not descend into the export trees; they are large and hold neither.
        dirnames[:] = [d for d in dirnames if d not in ("exports", "cache", "thumbs")]
    return sl, prd


def read_json(path):
    try:
        with open(path, "rb") as f:
            return json.loads(f.read().decode("utf-8-sig"))
    except Exception:
        return None


def prefix_of(stem):
    """Item_Axe_stor004 -> Item. The prefix is the SNO group name the file describes."""
    return stem.split("_", 1)[0] if "_" in stem else stem


def normalise(series):
    """Strip the quote wrapping the data ships with, then split the class child row.

    Returns (parent, class_or_None). No other cleanup: anything else we 'fix' here is a
    guess, and the raw column in series_rows.tsv is there so the rules stay checkable.
    """
    s = series.replace('"', "").strip()
    m = CLASS_SUFFIX.match(s)
    if m:
        return m.group("base").strip(), m.group("cls")
    return s, None


def scan_stringlists(sldir):
    files = [e.name for e in os.scandir(sldir) if e.is_file() and e.name.endswith(".stl.json")]
    total = len(files)
    print(f"  {total} StringList files", flush=True)

    label_count = Counter()                       # szLabel -> files carrying it
    label_prefix = defaultdict(Counter)           # szLabel -> prefix -> count
    prefix_count = Counter()
    rows = []                                     # (prefix, stem, raw series, name)
    done = 0

    def one(name):
        stem = name[:-len(".stl.json")]
        o = read_json(os.path.join(sldir, name))
        if not isinstance(o, dict):
            return None
        labels, series, disp = set(), None, ""
        for e in o.get("arStrings") or ():
            if not isinstance(e, dict):
                continue
            lab = e.get("szLabel") or ""
            labels.add(lab)
            if lab == "Series":
                series = e.get("szText") or ""
            elif lab == "Name":
                disp = e.get("szText") or ""
        return stem, labels, series, disp

    with cf.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(one, files, chunksize=256):
            done += 1
            if done % 8000 == 0:
                print(f"    ...{done}/{total}", flush=True)
            if not r:
                continue
            stem, labels, series, disp = r
            pfx = prefix_of(stem)
            prefix_count[pfx] += 1
            for lab in labels:
                label_count[lab] += 1
                label_prefix[lab][pfx] += 1
            if series:
                rows.append((pfx, stem, series, disp))
    return label_count, label_prefix, prefix_count, rows


def scan_storeproducts(prddir):
    """payload sno-name (lowercased) -> dict of everything the shop knows about it."""
    if not prddir or not os.path.isdir(prddir):
        return {}, 0
    files = [e.name for e in os.scandir(prddir) if e.is_file() and e.name.endswith(".prd.json")]
    print(f"  {len(files)} StoreProduct files", flush=True)
    sold = {}

    def one(name):
        o = read_json(os.path.join(prddir, name))
        if not isinstance(o, dict):
            return None
        # __raw__ / name: the ref-object shape StoreProductIndex.cpp reads, not a guess.
        seas = o.get("snoAssociatedSeason")
        seas = seas if isinstance(seas, dict) else {}
        info = {
            "product": name[:-len(".prd.json")],
            "season_sno": int(seas.get("__raw__") or 0),
            "season": seas.get("name") or "",
            "etype": o.get("eType", -1),
            "branch": o.get("szProductReleaseBranch") or "",
            "requires": bool(o.get("arRequiresOwning")),
        }
        out = []
        for fld in PAYLOAD_FIELDS:
            ref = o.get(fld)
            if isinstance(ref, dict):
                nm = ref.get("name") or ""
                if nm:
                    out.append((nm.lower(), dict(info, kind=fld)))
        return out

    with cf.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(one, files, chunksize=128):
            for nm, info in (r or ()):
                sold.setdefault(nm, info)
    return sold, len(files)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "build/release/data"
    outdir = sys.argv[2] if len(sys.argv) > 2 else "Claude outputs"
    if not os.path.isdir(root):
        print(f"Data folder not found: {root}")
        return 2
    os.makedirs(outdir, exist_ok=True)

    print("Locating d4data folders...", flush=True)
    sldir, prddir = find_dirs(root)
    if not sldir:
        print(f"No StringList folder under {root} -- is d4data downloaded?")
        return 2
    print(f"  StringList  : {sldir}")
    print(f"  StoreProduct: {prddir or '(not found)'}")

    print("Pass A/B -- StringLists...", flush=True)
    label_count, label_prefix, prefix_count, rows = scan_stringlists(sldir)
    print("Pass C -- StoreProducts...", flush=True)
    sold, n_prd = scan_storeproducts(prddir)

    # ── series aggregation ──────────────────────────────────────────────────────────
    series = defaultdict(lambda: {"members": [], "classes": set(), "prefixes": Counter(),
                                  "shop": 0, "season": 0, "seasons": Counter(),
                                  "requires": 0, "etypes": Counter(), "branches": Counter()})
    etype_all, branch_all = Counter(), Counter()
    # A StoreProduct_*.stl.json carries the Series label of the PRODUCT, not of a thing
    # you can own -- it is the row AppearanceMeta reads today. Counting it as a member
    # would double-count every shop bundle, so it is tallied on its own line instead.
    product_rows = Counter()
    for pfx, stem, raw, disp in rows:
        parent, cls = normalise(raw)
        d = series[parent]
        if pfx == "StoreProduct":
            product_rows[parent] += 1
            if cls:
                d["classes"].add(cls)
            continue
        d["members"].append((pfx, stem, raw, disp))
        d["prefixes"][pfx] += 1
        if cls:
            d["classes"].add(cls)
        # Item_Axe_stor004 -> Axe_stor004: the StoreProduct ref names the payload sno, not
        # the StringList file, so the group prefix has to come off before the lookup.
        hit = sold.get(stem.split("_", 1)[1].lower() if "_" in stem else stem.lower())
        if hit:
            d["shop"] += 1
            d["etypes"][hit["etype"]] += 1
            etype_all[hit["etype"]] += 1
            if hit["branch"]:
                d["branches"][hit["branch"]] += 1
                branch_all[hit["branch"]] += 1
            if hit["requires"]:
                d["requires"] += 1
            if hit["season"]:
                d["season"] += 1
                d["seasons"][hit["season"]] += 1

    # ── write ───────────────────────────────────────────────────────────────────────
    p_rows = os.path.join(outdir, "series_rows.tsv")
    with open(p_rows, "w", encoding="utf-8", newline="\n") as f:
        f.write("prefix\tstem\tseries_raw\tseries_parent\tclass\tname\n")
        for pfx, stem, raw, disp in sorted(rows):
            parent, cls = normalise(raw)
            f.write(f"{pfx}\t{stem}\t{raw}\t{parent}\t{cls or ''}\t{disp}\n")

    p_series = os.path.join(outdir, "series_summary.tsv")
    with open(p_series, "w", encoding="utf-8", newline="\n") as f:
        f.write("series\tmembers\tclass_rows\tshop_linked\tseason_linked\tpass_locked\t"
                "season_names\tetypes\tbranches\tprefixes\n")
        for name in sorted(series):
            d = series[name]
            f.write("\t".join([
                name, str(len(d["members"])), ",".join(sorted(d["classes"])),
                str(d["shop"]), str(d["season"]), str(d["requires"]),
                ";".join(sorted(n for n in d["seasons"] if n)),
                ",".join(f"{k}:{v}" for k, v in d["etypes"].most_common()),
                ",".join(f"{k}:{v}" for k, v in d["branches"].most_common()),
                ",".join(f"{k}:{v}" for k, v in d["prefixes"].most_common()),
            ]) + "\n")

    p_txt = os.path.join(outdir, "series_probe.txt")
    with open(p_txt, "w", encoding="utf-8", newline="\n") as f:
        def w(s=""):
            print(s, flush=True)
            f.write(s + "\n")

        w("=== Series / collection probe ===")
        w(f"StringList folder : {sldir}")
        w(f"StringList files  : {sum(prefix_count.values())}")
        w(f"StoreProduct files: {n_prd}")
        w()
        w("-- PASS A: every szLabel in the StringLists, most common first --")
        w("   (this is the discovery pass: a grouping field we do not know about shows up here)")
        w(f"   {'label':<28} {'files':>7}   top prefixes")
        for lab, c in label_count.most_common(60):
            top = ", ".join(f"{k}:{v}" for k, v in label_prefix[lab].most_common(4))
            w(f"   {lab:<28} {c:>7}   {top}")
        if len(label_count) > 60:
            w(f"   ... and {len(label_count) - 60} more labels (full set is implicit in the TSVs)")
        w()
        w("-- PASS B: Series coverage --")
        w(f"   files carrying a Series row : {len(rows)}")
        w(f"   ... of which StoreProduct_* : {sum(product_rows.values())}  (the product's own")
        w("       label -- the ONLY route AppearanceMeta.cpp reads today)")
        w(f"   ... owned things (members)  : {sum(len(d['members']) for d in series.values())}")
        w(f"   distinct raw Series strings : {len(set(r[2] for r in rows))}")
        w(f"   distinct after normalising  : {len(series)}")
        w()
        w("   by SNO-group prefix:")
        by_pfx = Counter(r[0] for r in rows)
        for k, v in by_pfx.most_common():
            w(f"     {k:<24} {v:>6}  (of {prefix_count[k]} files with that prefix)")
        w()
        w("-- PASS C: how far the shop and the seasons reach --")
        tot = sum(len(d["members"]) for d in series.values())
        shop = sum(d["shop"] for d in series.values())
        seas = sum(d["season"] for d in series.values())
        req = sum(d["requires"] for d in series.values())
        w(f"   members reachable from a StoreProduct : {shop} / {tot}")
        w(f"   ... of those, carrying a season       : {seas}")
        w(f"   ... of those, gated by arRequiresOwning (pass-locked, never sold): {req}")
        w(f"   members NO StoreProduct reaches       : {tot - shop} / {tot}")
        w("   ^ that last number is the size of the gap the Catalogue tab cannot see today,")
        w("     because CatalogueTab is a view over StoreProductIndex and nothing else.")
        w()
        w("   series with NO shop member at all (candidates for a non-shop category):")
        orphan = [(n, len(d["members"])) for n, d in series.items() if d["shop"] == 0]
        orphan.sort(key=lambda t: -t[1])
        w(f"     {len(orphan)} series, {sum(c for _, c in orphan)} members; largest 25:")
        for n, c in orphan[:25]:
            w(f"       {c:>5}  {n}")
        w()
        w("   eType distribution over shop-linked members (labels are NOT in the data --")
        w("   this is the only way to find out whether eType separates promo from ordinary):")
        for k, v in etype_all.most_common(20):
            w(f"     eType {k:<6} {v:>6}")
        w()
        w("   szProductReleaseBranch distribution (the patch a product shipped in):")
        for k, v in branch_all.most_common(25):
            w(f"     {k:<12} {v:>6}")
        w()
        w("   series whose members are split across several SNO groups (real bundles):")
        multi = [(n, d) for n, d in series.items() if len(d["prefixes"]) > 1]
        w(f"     {len(multi)} of {len(series)}")
        w()
        w(f"Wrote: {p_rows}")
        w(f"Wrote: {p_series}")
        w(f"Wrote: {p_txt}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
