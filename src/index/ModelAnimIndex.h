#pragma once
// The model animation + entity indexes, shared by D4's Models tab and the AssetBrowser core's D4
// store (phase 6b). MOVED from tabs/ModelsTab.cpp (2026-09-23): the scans, their disk cache, the
// family resolution and the clip queries are ModelsTab's code, called from both places.
//
//   anim   — every Anim/*.ani.json (owner appearances, frame counts), every AnimSet (clip → set,
//            female overrides, power names), each clip owner's rig bones (parsed from CASC), and the
//            base-family index (family prefix → merged clip rows, → union of rig bones).
//   entity — every Actor (appearances it wears, AnimSets it plays, monster family, skin variants)
//            and every Item (via its actor → the appearance it renders).
#include "model/AnimParser.h"

#include <QDataStream>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class CascReader;
class SnoIndex;
struct ModelGeometry;
struct ModelJoint;

namespace modelanim {

// The full result of the animation scan (Anim + AnimSet + rig-bone parse + family index).
struct AnimBlob {
    QSet<int>                     animatedSnos;
    QHash<int, QStringList>       rowsBySno;
    QSet<QString>                 famPrefixes;
    QHash<QString, QStringList>   famRows;
    QHash<QString, QString>       famOwner;
    QHash<QString, QSet<quint32>> famBones;
    QHash<QString, QString>       clipSet;
    QHash<QString, QStringList>   setClips;
    QSet<QString>                 femaleClips;
    QHash<QString, QString>       femalePair;
    QHash<QString, QString>       clipPower;
};

// The full result of the entity scan (Actor + Item metadata).
struct EntityBlob {
    QHash<int, QStringList> apprActors;
    QHash<int, int>         apprActorN;
    QHash<int, QString>     apprFamily;
    QHash<int, QStringList> apprItems;
    QHash<int, int>         apprItemN;
    QHash<QString, int>     itemAppr;
    QHash<int, QStringList> apprSets;
    QHash<int, QStringList> apprVariants;
    QHash<int, QList<int>>  apprVariantSnos;
    QHash<int, QString>     apprName;
};

// Progress text for a status line ("Scanning animations… 40%"). Called from the scan's thread.
using Progress = std::function<void(const QString&)>;

// Cache key: d4data buildVersion.txt (content + mtime) and the game build id (rig bones come from
// CASC, so a patch must invalidate it). `d4` is the d4data folder.
QString dataSignature(const QString& d4, const QString& gameBuildId);
QString indexCachePath(const QString& name);   // <data>/index_cache/<name>
bool readAnim(const QString& sig, AnimBlob& out);
void writeAnim(const QString& sig, const AnimBlob& blob);
bool readEntity(const QString& sig, EntityBlob& out);
void writeEntity(const QString& sig, const EntityBlob& blob);

// The anim scan in two halves, as ModelsTab ran it: the file + rig scan (worker thread), then the
// family pass over the SNO index (which ModelsTab runs on the GUI thread).
struct AnimScan { AnimBlob blob; QHash<int, QSet<quint32>> bonesBySno; };
AnimScan scanAnim(const QString& d4, CascReader* reader, const Progress& progress);
AnimBlob finishFamilies(const AnimScan& scan, const SnoIndex* index);
EntityBlob scanEntity(const QString& d4, const Progress& progress);
QSet<QString> rigFamilies(const SnoIndex& index);   // every *_base<NN> appearance's family prefix

// ── Queries ──────────────────────────────────────────────────────────────────────────────────────
QString animFamilyPrefix(const QString& nameLower);
QString animLongestFamily(const QString& nameLower, const QSet<QString>& families);
bool clipInFamily(const QString& clipName, const QStringList& fams);
QStringList clipNamesOf(const QStringList& rows);

// What the clip queries read. `skel` is the model's own skeleton for the bone-overlap fallback —
// only when the caller holds it (ModelsTab: the model on screen; the store: the model it loads).
struct Lookup {
    const AnimBlob* anim = nullptr;
    const EntityBlob* ent = nullptr;
    const QSet<QString>* rigFams = nullptr;
    const QVector<ModelJoint>* skel = nullptr;
};
QStringList familiesBySkeleton(const AnimBlob& a, const QVector<ModelJoint>& skel, double minScore);
QStringList clipFamiliesFor(const Lookup& L, const QString& nameLower);
QStringList authoredAnimClips(const Lookup& L, int sno, const QString& nameLower);
QStringList setAnimClips(const Lookup& L, int sno, const QString& nameLower);
QStringList ownAnimClips(const Lookup& L, int sno, const QString& nameLower);
QStringList baseAnimClips(const Lookup& L, int sno, const QString& nameLower);
QStringList animClipsFor(const Lookup& L, int sno, const QString& nameLower,
                         bool wantOriginal, bool wantSets, bool wantBase);

// Decode one clip for a model's skeleton (its rest pose fills empty curves). SEH-guarded.
AnimParser::DecodedAnim decodeForSkeleton(CascReader* reader, const QString& d4, const QString& animName,
                                          const ModelGeometry& geo);

}  // namespace modelanim
