#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// One row of a StringList: a label (key), its translated text, and the label hash.
struct StringRow {
    QString label;
    QString text;
    qint64  hLabel = 0;
};

// Parse a d4data StringList JSON (<name>.stl.json) — reads the arStrings[] array
// of { szLabel, szText, hLabel }. Returns an empty vector on malformed input.
QVector<StringRow> parseStringTableJson(const QByteArray& json);
