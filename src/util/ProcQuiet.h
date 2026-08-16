#pragma once
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>   // CREATE_NO_WINDOW. NOMINMAX is set project-wide (see CMakeLists).
#endif

// ── Run a console program without flashing a console window ─────────────────────────────────────
//
// On Windows, starting a CONSOLE application (git, curl) from a GUI application allocates a console
// for it. Qt does not pass CREATE_NO_WINDOW by default, so every such spawn pops a black window for
// as long as the child lives — which for the startup update check is a visible flash a second or
// two after launch, and reads as "something crashed".
//
// Call this on the QProcess BEFORE start(). No-op on other platforms.
//
// Apply it to every QProcess that runs an external tool. The ones that exist today are the update
// check (git rev-parse / git ls-remote / curl), the diablo4.dad refresh (curl) and the d4data
// downloader (git) — all of which are invisible background work as far as the user is concerned.
inline void quietProcess(QProcess& p)
{
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        if (a) a->flags |= CREATE_NO_WINDOW;
    });
#else
    Q_UNUSED(p);
#endif
}
