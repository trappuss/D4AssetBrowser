"""
tact_scan.py  (v4 - fast: private-heap only, minimal needle set, calibrated)

Scan a running Diablo IV process for TACT key name->value pairs; write them
in wowdev format (KEYNAME_HEX KEYVALUE_HEX).

    python tact_scan.py <PID> [output_file] [--all] [--allmem]

Speed notes:
  * Scans only MEM_PRIVATE heap by default (skips the game's multi-GB
    texture/asset and DLL regions, where runtime keys never live).
    Use --allmem to scan every readable region (slow fallback).
  * By default searches only the 7 known calibration keys + the target
    collab key, so it needs ~8 fast substring scans per chunk instead of
    ~190. Use --all to extract EVERY known key (much slower).
  * Self-calibrates the in-memory layout against the known keys (the
    layout shifts between game patches).
  * Keeps only a 64-byte window per hit; heartbeat every ~3 s.
"""

import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ           = 0x0010
MEM_COMMIT                = 0x1000
MEM_PRIVATE               = 0x20000
PAGE_NOACCESS             = 0x001
PAGE_GUARD                = 0x100

k32 = ctypes.windll.kernel32

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress",       ctypes.c_size_t),
        ("AllocationBase",    ctypes.c_size_t),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize",        ctypes.c_size_t),
        ("State",             wt.DWORD),
        ("Protect",           wt.DWORD),
        ("Type",              wt.DWORD),
    ]

def open_process(pid):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise ctypes.WinError()
    return h

def enum_readable_regions(handle, private_only=True):
    addr = 0
    mbi  = MEMORY_BASIC_INFORMATION()
    sz   = ctypes.sizeof(mbi)
    while True:
        if k32.VirtualQueryEx(handle, ctypes.c_size_t(addr), ctypes.byref(mbi), sz) == 0:
            break
        ok = (mbi.State == MEM_COMMIT
              and not (mbi.Protect & PAGE_NOACCESS)
              and not (mbi.Protect & PAGE_GUARD))
        if ok and private_only and mbi.Type != MEM_PRIVATE:
            ok = False
        if ok:
            yield mbi.BaseAddress, mbi.RegionSize
        addr += mbi.RegionSize
        if addr > 0x7FFFFFFFFFFF:
            break

def read_mem(handle, addr, size):
    buf  = (ctypes.c_char * size)()
    read = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(handle, ctypes.c_size_t(addr), buf, size, ctypes.byref(read)) or read.value == 0:
        return None
    return bytes(buf[:read.value])

KEY_IDS_HEX = [
    "00b9b2a6cb806a3d","02819b3a245a767c","0296d1f07ca1df56","02d64e000d128ac5",
    "04af7358e24ff07b","093b6416e2835509","09f0c09900c83a1c","0a5015374c0f9fc7",
    "0ac43975994b4106","0c22c1f954a336f0","0c2931d39ff685ad","0c585874e53841a4",
    "0cea68beb6ec803e","0dc8f43051340411","0e45c5b3f080c6fb","0e9b08fa35ddc6d8",
    "0f73ba902a27279e","117fa68bf73b4f46","11d2784edc1c3769","19ae70d4f8ce4833",
    "1c6299566314a81d","1e15e556129dd6a5","207ce929ca10d688","20b887ab86b29eff",
    "23994777a02901f1","23f52090bd6414e0","25bb457c64653cd4","268ecbb3cd39c706",
    "271f26c4d4a64875","27c4a114536506d8","298935e8d2985166","2ae07585052a28de",
    "2babd3fba391b64a","2eba52c766dd5133","30481734ca1beb09","3159e78ad558d81e",
    "33d3455983ca5be5","35fe77f664722b93","363463c7ad46e5ba","39f4bf6898b8afb7",
    "3b5646cd2a44ed23","3bc501404c5ea17a","3c37e237f2ea13f8","3ea0f19a64f57cfd",
    "3fe9ca51458d7074","41c2c06e9734ae99","44549befd43628e1","49c1973b9187cee2",
    "4a2698166bd0eef8","4a80a460e28f06dd","4cd01eb4ba52b1d3","4d3cadbaeb53462b",
    "4e63648c952aad61","4e6dde2436600f88","52651493ad4fde83","53301688d6060182",
    "5357e6fd5313bb80","53a9eed970275d93","56c11d5703b1cd62","570c993d29e0f053",
    "5a4ed91f5fe14479","5e00ef0ad3a6c7fa","60eb8622b471cabf","637c0b945226609a",
    "63a1f6b61adadfa1","64547c3be707c47b","647062defcac4560","64b7b2fc65ec01d4",
    "65a09c95c5c03a10","65d1e3457b9473af","65fcf70aea2bcd39","6612264f51ca5ba2",
    "66e3f6a1c8ce6bfe","6bbcbaeb32c26d98","6f065be757bd4723","6f3132d3eeaf2a1f",
    "6f34731e2bcb1945","6f5010d43388052f","6ffb7de0b9b8c50d","7071e0478c2f0622",
    "75876ef74f32014e","75dc4391e8b03f92","76bf8ce5f9f6eb49","76f517c7b6896375",
    "77602f74e704b849","7763703d3588429d","796bbc238b12ea3c","7a52b345239dabe8",
    "7a989ac15a9a3c26","7d3ef238d7d524be","7da29f856ac29ab7","7e4c9506710a54b5",
    "7efaf89332a0c06e","7f5e97a90853c568","80a99a0d2e48c1a0","8833e2af6105637d",
    "899a59b6c756ddb7","8a134765c03ee270","8a223842ed92fa53","8ccee4def3880104",
    "8efadde626f7dc66","8feb63c7d24d20e9","911a1b309ad5cdc9","92a40362cc12955a",
    "92d22b8b23c99f93","93399ccf91519776","9588a3afcc74db0f","9588b155114b5b42",
    "969796b620592968","99f77a097ed90eb8","9afa34c766a9fcc5","9bed681f0dc6c164",
    "9dc9af77a803eb97","9dcf3c25bf3b6afb","9f762d72da44beed","9fcd1b6af0b4cf8b",
    "a21546af07fc0820","a365324771a9b9b7","a486d013627eb6a4","a5b1a3b4a325d636",
    "a5fe26a62054a2ce","a6927e313d2e1231","a6a382c4771ffc7c","a77ed1d8068fd2ab",
    "aa89ee6a2722de91","acacdd907969ab03","aeba2a3a58de7854","aebe3fb3330c35fe",
    "b1af1db9f17fa6d7","b1aaab0ef7f159f1","b23b0175d97aca6d","b2f3a7f548cbd50a",
    "b4da1b8ad9141078","b76560768ae38b8a","b86956d6d91d443b","bc95bd45e558f14a",
    "bd4b832dfb32530e","c00f1fd390a4aa96","c03f22e1b898322b","c04b6ae2d0e3fbf6",
    "c0a20b37e510d2b1","c14dc08060f9511d","c25cf2becc3b013e","c2b90a22cc6e486d",
    "c6269e2b13409ed7","c6674c942e38e929","c8273ec77b455370","c9a2f1dd0070603d",
    "ca9a3eb527f86eff","cac06c7b567326b7","cc29d089cbad6059","ce19c6558cefdb2b",
    "ce74442b4c1fb82b","d573abc49816a246","d9547915664390a4","d9aebc355721a591",
    "dda7ef59c165e490","ddaaa6375b245536","e0861787e6650df0","e1a5e0d54038e62e",
    "e2cd1b98025480ca","e31f0ca32b5a48be","e5326863682422ad","e8090a2bd5e6e7b8",
    "e96f69e932031e83","ea2a1a8784018d99","eb1eff0b64416e57","eb4d907876a15042",
    "ed843ad87633f47c","eda9b641b34aff74","edcfa4e92857c6ed","eed9a8c332e13e38",
    "f221dc10f30817bf","f22db741197babd4","f37c618f8d8e780a","f53e3a480013b59f",
    "f61dc328a49e2b27","f796f9e481aabec9","f7d8eb38d48cdfd9","f81378acae8096b8",
    "f850e87bf4bb815f","f9c3ce895d6e413a","fb45e6b23dc6ca40","fcdf1a78a63e10a6",
    "fdd1c709131c45b7","ef133e99f633fb1f",
]

ANCHORS_TEXT = """
0E5332FB2D834BBD 3ED1F79569C1E7B89941F8D358C31140
1C3AC80099C0F009 9D77FE7CDE388BB382E7FABDAF0EEFE9
53FA92ED4238228A 55C6C0A89CD8B608E8FCA6F68E3D261B
5960ADCB89D029CC F382EB7DB08F21000600000081938EC1
665198D2E8358929 DF8968D3D37D7DBA3763E4184D8F1933
A1DFDA1AB6F6A163 F382EB7DB08F21000600000081938EC1
F159F1F70EABAAB1 E00294A2AF86C537E088A086BABE3D82
"""

TARGET = "B7B9A971473265A3"

# Full name set (blte hex) and its memory search patterns.
ALL_PATTERNS = {}
for h in KEY_IDS_HEX:
    pat  = struct.pack("<Q", int(h, 16))
    blte = struct.unpack("<Q", bytes.fromhex(h))[0]
    ALL_PATTERNS[pat] = format(blte, "016X")

ANCHORS = {}
for line in ANCHORS_TEXT.strip().splitlines():
    nm, vv = line.split()
    ANCHORS[bytes.fromhex(nm)] = bytes.fromhex(vv)

# Fast set = anchors + target only.
FAST_PATTERNS = {}
for nm in [l.split()[0] for l in ANCHORS_TEXT.strip().splitlines()] + [TARGET]:
    FAST_PATTERNS[bytes.fromhex(nm)] = nm.upper()

ALL_KEY_PATS = set(ALL_PATTERNS.keys())

LAYOUTS = [
    ("inline+8", 8, False), ("inline+16", 16, False), ("inline+24", 24, False),
    ("inline+12", 12, False),
    ("ptr+8", 8, True), ("ptr+16", 16, True), ("ptr+24", 24, True), ("ptr+32", 32, True),
]

CHUNK = 4 * 1024 * 1024

def is_heap_ptr(v):
    if v == 0:
        return True
    return (v >> 48) in (0, 1, 2, 3, 4, 5, 6, 7)

def looks_like_key(b):
    if not b or len(b) < 16:            return False
    if all(x == 0 for x in b):          return False
    if len(set(b)) < 6:                 return False
    if all(0x20 <= x < 0x7f for x in b): return False
    if is_heap_ptr(struct.unpack_from("<Q", b, 0)[0]): return False
    if is_heap_ptr(struct.unpack_from("<Q", b, 8)[0]): return False
    if b[8:16] in ALL_KEY_PATS:         return False
    return True

def extract_value(handle, win, off, is_ptr):
    if is_ptr:
        if off + 8 > len(win):
            return None
        ptr = struct.unpack_from("<Q", win, off)[0]
        if not (0x10000 < ptr < 0x7FFFFFFFFFFF):
            return None
        return read_mem(handle, ptr, 16)
    if off + 16 > len(win):
        return None
    return win[off:off + 16]

def scan(pid, patterns, private_only=True):
    handle  = open_process(pid)
    regions = list(enum_readable_regions(handle, private_only))
    total_regions = len(regions)
    print("Scanning PID %d - %d %s regions"
          % (pid, total_regions, "private-heap" if private_only else "readable"),
          file=sys.stderr, flush=True)

    WIN  = 64
    hits = {}
    want = list(set(patterns.keys()) | set(ANCHORS.keys()))
    want_set = set(want)
    total_bytes = 0
    total_hits  = 0
    t0 = time.time()
    last = t0
    for ri, (base, size) in enumerate(regions):
        offset = 0
        while offset < size:
            n = min(CHUNK, size - offset)
            data = read_mem(handle, base + offset, n)
            if data:
                total_bytes += len(data)
                for pat in want:
                    pos = 0
                    while True:
                        i = data.find(pat, pos)
                        if i < 0:
                            break
                        hits.setdefault(pat, []).append(bytes(data[i:i + WIN]))
                        total_hits += 1
                        pos = i + 1
            offset += n
            now = time.time()
            if now - last >= 3.0:
                last = now
                print("  ... region %d/%d  %.0f MiB  %d hit(s)  %ds"
                      % (ri + 1, total_regions, total_bytes / 1048576.0, total_hits, int(now - t0)),
                      file=sys.stderr, flush=True)
        # Early exit: stop the moment every requested key AND every calibration
        # anchor has been located (no need to scan the rest of the heap).
        if want_set.issubset(hits.keys()):
            print("  all requested keys located at region %d/%d - stopping early"
                  % (ri + 1, total_regions), file=sys.stderr, flush=True)
            break
    print("  scan done: %.0f MiB, %d hit(s), %ds. Calibrating..."
          % (total_bytes / 1048576.0, total_hits, int(time.time() - t0)),
          file=sys.stderr, flush=True)

    scores = {}
    for (label, off, is_ptr) in LAYOUTS:
        for rev in (False, True):
            good = 0
            for pat, want_val in ANCHORS.items():
                for win in hits.get(pat, []):
                    v = extract_value(handle, win, off, is_ptr)
                    if v and (v[::-1] if rev else v) == want_val:
                        good += 1
                        break
            scores[(label, off, is_ptr, rev)] = good

    best = max(scores, key=scores.get)
    if scores[best] == 0:
        print("\nWARNING: could not calibrate against any known key.", file=sys.stderr, flush=True)
        print("Open the in-game Shop/Collections so keys load, then retry.", file=sys.stderr, flush=True)
        print("If it still fails, re-run with --allmem (scans all regions).", file=sys.stderr, flush=True)
        k32.CloseHandle(handle)
        return {}
    print("Calibration matches:", file=sys.stderr, flush=True)
    for k in sorted(scores, key=lambda x: -scores[x]):
        if scores[k]:
            lbl, off, is_ptr, rev = k
            print("  %-9s %-9s -> %d" % (lbl, "reversed" if rev else "as-is", scores[k]),
                  file=sys.stderr, flush=True)

    lbl, off, is_ptr, rev = best
    print("\nUsing layout: %s (%s)\n" % (lbl, "reversed" if rev else "as-is"),
          file=sys.stderr, flush=True)

    found = {}
    for pat, blte_name in patterns.items():
        # D4 stores each record as [name][name][value]; the name pattern therefore
        # matches at two adjacent offsets, so we get a window per copy. Reject any
        # candidate whose first 8 bytes ARE the key name (that window read a second
        # name field, not the value), then majority-vote across the rest for safety.
        votes = {}
        for win in hits.get(pat, []):
            v = extract_value(handle, win, off, is_ptr)
            if not v:
                continue
            cand = v[::-1] if rev else v
            if cand[:8] == pat:
                continue
            if looks_like_key(cand):
                votes[cand] = votes.get(cand, 0) + 1
        if votes:
            best_val = max(votes, key=votes.get)
            found[blte_name.lower()] = best_val.hex().upper()
            print("  FOUND %s %s" % (blte_name, best_val.hex().upper()), file=sys.stderr, flush=True)

    k32.CloseHandle(handle)
    return found

if __name__ == "__main__":
    args = sys.argv[1:]
    full    = "--all" in args
    allmem  = "--allmem" in args
    pos     = [a for a in args if not a.startswith("--")]
    if not pos:
        print("Usage: python tact_scan.py <PID> [output_file] [--all] [--allmem]", file=sys.stderr)
        sys.exit(1)
    pid      = int(pos[0])
    out_file = pos[1] if len(pos) > 1 else "d4_tact_keys.txt"
    patterns = ALL_PATTERNS if full else FAST_PATTERNS

    found = scan(pid, patterns, private_only=not allmem)
    # Fallback: if fast/private scan found nothing, retry scanning all regions.
    if not found and not allmem:
        print("\nRetrying with --allmem (all regions)...", file=sys.stderr, flush=True)
        found = scan(pid, patterns, private_only=False)

    lines = ["%s %s" % (name.upper(), val) for name, val in sorted(found.items())]
    with open(out_file, "w") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))

    print("\n--- %d key(s) written to %s ---" % (len(found), out_file), file=sys.stderr, flush=True)
    if TARGET.lower() in found:
        print("*** Target collab key %s = %s ***" % (TARGET, found[TARGET.lower()]), file=sys.stderr, flush=True)
    else:
        print("(target collab key %s not found this run)" % TARGET, file=sys.stderr, flush=True)
    for line in lines:
        print(line)
