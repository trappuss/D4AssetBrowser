#pragma once
// d4cloth — LegacySolver: a faithful, line-for-line port of the app's current cloth path
// (GLModelWidget::buildClothSim capsule setup, buildSpringBones, springBoneStep — state as
// of 2026-07-25), WITH ITS BUGS. Its purpose is to reproduce the cape defect inside the
// harness so the root-cause account cites harness numbers and fixes are A/B diffs.
//
// Known deliberate divergences from the app (documented, believed irrelevant to the repro):
//   · part flags (m_partHair / m_partFx / m_partCloth) are all false — the harness has no
//     material classifier; hair-class overrides and FX exclusion never trigger. The barF
//     cape outfit carries no hair parts on the cloth bones.
//   · no user mouse: m_yaw is scripted per scenario (constant for `rest`).
//   · m_hasAnim = false (rest pose) until the anim scenario lands.

#include "Scene.h"

#include <QString>
#include <QVector>
#include <array>
#include <functional>

namespace d4cloth {

class LegacySolver {
public:
    using Mat4 = RigMath::Mat4;

    explicit LegacySolver(const Scene& sc);
    virtual ~LegacySolver() = default;

    // One frame: pose capsules from the (rest) globals, run the springBoneStep port,
    // rebuild bone globals (pass-2 swing). Mirrors the applySkinning cloth block.
    virtual void step();

    // Scripted user yaw (the "React to rotation" input). Set before step().
    void  setYaw(float y) { m_yaw = y; }

    // Animation playback: pose animG from the scene's decoded clip at frame f (rest TRS
    // where a bone has no track), mirroring applySkinning's local/global build. -1 = rest.
    void setAnimFrame(int f);
    bool hasAnim() const { return sc.anim.valid && m_animFrame >= 0; }
    QVector<quint8> sbAnimMoves;             // computeAnimMoves port (per bone)

    // The cage path's motion-limit scale for cage ci (slider x tracking^2 x span) — the
    // dumps print md = attachLen x this, alongside the raw authored value.
    float cageMdScale(int ci) const;

    // ── state (public: the dumps read it directly, like the overlay reads m_sb*) ──
    const Scene& sc;
    ClothParams  P;                      // copied from scene; --param overrides land here

    int nb = 0;
    QVector<Mat4> animG;                 // per-frame animated globals (rest pose here)
    QVector<Mat4> global;                // post-sim globals (pass-2 output)

    // buildSpringBones state (names match the app's members)
    QVector<int>    sbOrder;
    QVector<quint8> sbIsCloth, sbPin, sbDriven, sbHair, sbContact;
    QVector<int>    sbChild, sbAnchorPiece, sbAnchorVert, sbSim;
    QVector<float>  sbLenParent, sbAttach, sbAnchorW;
    QVector<float>  sbSimHead, sbPrevHead;        // 3 floats per bone
    QVector<int>    sbConA, sbConB; QVector<float> sbConRest;
    bool sbSeeded = false;

    struct CageRt {
        int simIdx = -1;
        QVector<std::array<quint16,4>> J;
        QVector<std::array<float,4>>   W;
        QVector<float> pos, prev, target;         // 3 floats per cage vert
        bool seeded = false;
    };
    QVector<CageRt> cages;
    QVector<float>  cageSpan;                     // per clothSim

    // collision capsules (authored path)
    QVector<int>   colBoneA, colBoneB;
    QVector<float> colP0Bind, colP1Bind, colR0, colR1, colP0, colP1;
    bool colAuthored = false;

    // ── per-step instrumentation ──
    int   stepIndex = -1;
    int   contactCount = 0;        // resolver hits this step
    float worstPen = 0;            // worst penetration this step [wu]
    int   divergeClamped = 0;      // bones that hit kDivergeMax this step
    float worstDiv = 0; int worstDivBone = -1;
    int   cageLimitHits = 0;       // cage particles clamped by the motion limit this step
    int   cageSafetyHits = 0;      // cage particles caught by the 1.5x safety bound
    int   boneLimitHits = 0;       // bones clamped by the per-bone motion limit
    float kDivergeMax = 0;         // the cap in force (for reports)

    // Trace: watched bones ("b:327") and cage particles ("p:<cage>/<vert>"). Every phase
    // that moves a watched entity emits one line: step,phase,id,x,y,z,note.
    QVector<int> traceBones;
    QVector<QPair<int,int>> traceParts;
    std::function<void(const QString&)> traceSink;   // null = tracing off

protected:
    void buildCapsules();            // buildClothSim (authored-capsule path)
    void buildSpringBones();
    void springBoneStep();
    void poseCapsules();
    void tr(const char* phase, const QString& id, const float* p, const QString& note = {});
    bool watchedBone(int j) const { return traceSink && traceBones.contains(j); }
    bool watchedPart(int c, int v) const
    { return traceSink && traceParts.contains(qMakePair(c, v)); }

    void computeAnimMoves();

    float m_yaw = 0, m_spinPrevYaw = 0, m_spinOmega = 0;
    bool  m_spinSeeded = false;
    bool  m_built = false;
    bool  m_animMovesBuilt = false;
    int   m_animFrame = -1;
};

} // namespace d4cloth
