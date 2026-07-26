#pragma once
#include <QString>
#include <functional>

class GLModelWidget;

// Shared preview-capture helpers used by the Export menu (and the Wardrobe Camera panel).
// Each renders the given live 3D preview and writes a file; they read GIF/quality options
// from QSettings (export/gifFps, export/gifTurntableFrames). Phase 1: opaque background
// (transparent-background rendering is a Phase 2 follow-up).
namespace ExportCapture {

// Progress callback for the multi-frame captures: called with (framesDone, framesTotal) after
// each rendered frame; return false to cancel (the capture then aborts and returns false).
using ProgressFn = std::function<bool(int done, int total)>;

// Save the current preview frame. Format is inferred from the path extension (.png/.jpg).
bool saveImage(GLModelWidget* view, const QString& path);

// Render one full 360° turntable spin → animated, looping GIF.
bool turntableGif(GLModelWidget* view, const QString& path, const ProgressFn& progress = {});

// Record exactly the currently-playing animation's frames (0..N-1), one GIF frame each, so
// the GIF loops the animation. Returns false if no animation is loaded on the preview.
bool animLoopGif(GLModelWidget* view, const QString& path, const ProgressFn& progress = {});

}
