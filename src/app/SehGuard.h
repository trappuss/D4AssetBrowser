// SehGuard.h — turn Windows structured exceptions (access violations, etc.)
// into catchable C++ exceptions, and provide a small RAII/wrapper so the model
// load + GPU render paths can survive a hardware fault (CPU out-of-bounds *or*
// a GPU-driver crash, both of which surface as an access violation) instead of
// terminating the whole application.
//
// Requires the translation unit that *catches* to be compiled with /EHa
// (enabled for this target in CMakeLists.txt). The translator is per-thread, so
// installSehTranslator() must be called on every thread that wants protection
// (the GUI thread and each background load/parse thread).
#pragma once

#include <QString>
#include <functional>

namespace seh {

// A C++ exception carrying the SEH code (e.g. 0xC0000005 = access violation)
// and a short human-readable stage label for logging.
struct HardwareFault {
    unsigned long code = 0;
    QString what;
};

// Install the structured-exception translator on the *current* thread. Safe to
// call more than once per thread. No-op on non-MSVC builds.
void installSehTranslator();

// Run `fn` with hardware-fault protection. Returns true if it completed, false
// if a structured exception (or C++ exception) was caught. On failure, if
// `outFault` is non-null it receives the fault details. `stage` is a label used
// in the fault's `what` for logging (e.g. "parse", "setGeometry", "thumbnail").
//
// The caller's thread must have called installSehTranslator() first (for SEH
// faults to be catchable); this function also installs it defensively.
bool runGuarded(const char* stage, const std::function<void()>& fn,
                HardwareFault* outFault = nullptr);

} // namespace seh
