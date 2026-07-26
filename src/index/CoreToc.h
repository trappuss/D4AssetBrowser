#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

// One asset entry from CoreTOC.dat.
struct SnoEntry {
    qint32  snoId = 0;
    QString name;
};

// Parse raw CoreTOC.dat bytes into { groupId : [SnoEntry...] }.
//
// Faithful port of DiabloTools/d4data parse.js (and the Python fork's
// coretoc.parse_coretoc). Handles both the legacy and the 0xBCDE6611 "new" header
// formats. Returns an empty hash on a malformed / implausible buffer so callers can
// fall back rather than build a corrupt index.
QHash<int, QVector<SnoEntry>> parseCoreToc(const QByteArray& data);
