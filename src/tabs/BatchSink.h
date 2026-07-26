#pragma once
// Optional live-reporting hooks for the batch export pipelines (ModelsTab::exportModels,
// TexturesTab::bulkExportTextures). When a sink is supplied the pipeline reports per-item
// progress and human-readable log lines here INSTEAD of popping its own modal progress
// dialog, and polls `canceled` between items — this is what feeds the Bulk Extract tab's
// live console. A null sink keeps the old dialog behaviour for the in-tab batch exports.

#include <QString>
#include <functional>

struct BatchSink {
    std::function<void(int done, int total)> progress;   // called before each item
    std::function<void(const QString& line)> log;        // one line per notable event
    std::function<bool()> canceled;                      // polled between items
};
