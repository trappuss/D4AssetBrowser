#pragma once
#include <QString>

// The runtime log file. main.cpp owns the QFile and the Qt message handler; this is the small
// surface Settings needs to turn it on and off and to tell the user where it is.
//
// Why a live toggle rather than a "takes effect next launch" note: the reason to want the file is
// almost always that something is misbehaving RIGHT NOW. A switch that only arms itself after a
// restart would lose the very session you wanted captured.
namespace AppLog {

// Open (truncating) or close data\D4AssetBrowser.log. Persists to the log/autoFile key, so the
// choice survives a restart, and is safe to call from the GUI thread at any time — the message
// handler serialises every write behind the same mutex this takes.
void setFileLogging(bool on);
bool fileLogging();

// Absolute path of the log file, whether or not it is currently open. Shown in Settings so a bug
// report can point at it without the user hunting.
QString filePath();

}  // namespace AppLog
