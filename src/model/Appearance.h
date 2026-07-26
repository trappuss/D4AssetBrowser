#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// One material binding from an appearance's ptAppearanceMaterials[].ptSOAs[].
struct AppMaterial {
    QString name;        // snoMaterial.name (e.g. "shield_base07_mat")
    qint64  sno   = 0;   // snoMaterial.__raw__
    qint64  hash  = 0;   // dwMaterialHash
    int     flags = 0;   // SOA dwFlags
};

// Parsed appearance (.app.json): its material bindings + look names. The same
// chain d4analyzer's Models tab walks (appearance → materials → … → textures).
struct AppearanceInfo {
    bool                 valid = false;
    QVector<AppMaterial> materials;
    QStringList          looks;
};

AppearanceInfo parseAppearanceJson(const QByteArray& json);
