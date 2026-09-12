#pragma once
#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

// Reverse-link index: Texture ← Material ← Appearance (model). Built by crawling
// d4data Material + Appearance JSON once on a background thread and cached to disk.
// Powers the Textures tab's ASSOCIATED MODELS panel and the texture "Used by" field.
//   matTextures : matSno → [(texSno, shaderSlot), …]
//   texToMats   : texSno → [matSno, …]
//   matToApps   : matSno → [appSno, …]
class AssetLinks : public QObject {
    Q_OBJECT
public:
    static AssetLinks& instance();

    bool ready() const { return m_ready; }
    bool building() const { return m_building; }
    void ensureBuilt(const QString& d4dataDir);
    // Drop the in-memory links + delete the on-disk cache (stale-cache invalidation on a
    // game-build / d4data change). Next ensureBuilt rebuilds from scratch.
    void reset();

    struct MatLink {
        int matSno = 0;
        QVector<QPair<int, int>> texPairs;   // (texSno, slot) — material's roster
        QVector<int> apps;                   // appearances using this material
    };
    // Materials that reference `texSno`, each with its texture roster + the
    // appearances that use it.
    QVector<MatLink> linksForTexture(int texSno) const;

    // Every appearance that references this material. matToApps has been built and cached since
    // this index existed, but the only way in was through a TEXTURE — so "which appearances use
    // this material" could not be asked at all, even though the answer was already in memory.
    //
    // It is the question every material bug starts from. The Paladin HED bug (a pauldron ornament
    // material named palM_stor164_wolfHead, matched by a contains("head") test, which took the
    // whole torso off screen) was found by sweeping CoreTOC by hand; this answers it directly.
    // Empty when the index is not ready() yet — callers must check, because empty otherwise reads
    // as "nothing uses it", which is a different and alarming answer.
    QVector<int> appsForMaterial(int matSno) const { return m_matToApps.value(matSno); }

    static QString slotRole(int slot);

signals:
    void readyChanged();
    void progress(int pct);

private:
    explicit AssetLinks(QObject* parent = nullptr) : QObject(parent) {}
    void install(QHash<int, QVector<QPair<int, int>>> matTex,
                 QHash<int, QVector<int>> matToApps);

    QHash<int, QVector<QPair<int, int>>> m_matTextures;
    QHash<int, QVector<int>>             m_texToMats;
    QHash<int, QVector<int>>             m_matToApps;
    bool m_ready = false;
    bool m_building = false;
};
