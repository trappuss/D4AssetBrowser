#pragma once
// parallelMap — moved verbatim from tabs/ModelsTab.cpp (2026-09-23) so the anim/entity index builds
// in index/ModelAnimIndex.cpp can use it too. ModelsTab includes this and is unchanged.
#include "app/SehGuard.h"

#include <QStringList>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

// ── Parallel file-scan helper (first-run indexing speed) ──────────────────────────────────────────
// Parse a big list of metadata files across all CPU cores. `parse(path)` runs on worker threads and
// returns one record R per file (no shared state → no locking); the caller then aggregates the returned
// records serially (keeping the existing, correct merge logic). Results preserve input order.
// `report(done,total)` is polled from the coordinating thread for live progress. Blocks until done —
// call it from a background thread (the scans already run on one). Cuts first-run parsing ~N-fold.
// `threadMul` oversubscribes the pool: for I/O-bound loose-file scans (opening tens of thousands of
// tiny JSON files, where threads spend most of their time blocked on disk), running ~2× cores hides
// that latency and raises throughput. Leave it 1 for CPU-bound work (e.g. the texture BC-decode).
template <typename R, typename ParseFn, typename ReportFn>
std::vector<R> parallelMap(const QStringList& files, ParseFn parse, ReportFn report,
                           bool installSeh = false, int threadMul = 1)
{
    const int total = files.size();
    const size_t nSlots = size_t(total);   // NB: a plain variable (NOT 'slots' — that's a Qt macro!)
    std::vector<R> out(nSlots);   // one slot per file; workers write distinct indices (no COW, no lock)
                                  // NB: 'nSlots' as a plain variable also avoids a most-vexing-parse.
    if (total == 0) return out;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2) hw = 2;
    if (threadMul < 1) threadMul = 1;
    const int nThreads = std::min<int>(int(hw) * threadMul, total);
    std::atomic<int> done{0};
    std::vector<std::thread> pool;
    pool.reserve(size_t(nThreads));
    for (int t = 0; t < nThreads; ++t) {
        pool.emplace_back([&, t]() {
            if (installSeh) seh::installSehTranslator();
            for (int i = t; i < total; i += nThreads) {   // strided → balanced load
                // A parse that throws (or an SEH fault translated to a C++ exception when
                // installSeh is set) must not std::terminate the whole app from a worker —
                // leave that slot default-constructed so one bad file is skipped, not fatal.
                try { out[i] = parse(files.at(i)); }
                catch (...) { }
                done.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (done.load(std::memory_order_relaxed) < total) {
        report(done.load(std::memory_order_relaxed), total);
        QThread::msleep(60);
    }
    for (std::thread& th : pool) th.join();
    report(total, total);
    return out;
}
