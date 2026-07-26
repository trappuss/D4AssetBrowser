#pragma once
// Shared, lazily-built map of animation clip → its readable action label (and owning AnimSet),
// derived from base/meta/AnimSet/*.ans.json (each ptPowerEntryList pairs snoPower → snoAnim). Used
// by the Models tab and the Wardrobe tab so both show "Idle / Walk / Get Hit…" instead of raw
// filenames. Header-only singleton; built once per session (blocking on first use, then cached).
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QString>

class AnimActionIndex {
public:
    static AnimActionIndex& instance() { static AnimActionIndex i; return i; }

    // Build from a d4data root if not already built. Safe to call repeatedly.
    void ensure(const QString& d4)
    {
        if (m_built || d4.isEmpty()) return;
        m_built = true;
        const QString dir = d4 + QStringLiteral("/json/base/meta/AnimSet");
        // Pair the entry's Power (the action) with its clip; the "(?!snoPower)" guard stops a null
        // power from grabbing the next entry's clip. group1 = power name, group2 = clip name.
        // Power → its clip. snoAnim is the base/male clip; snoFemaleOverrideAnim is the female variant
        // in the SAME entry — both should carry the entry's action, so map the power to each. The
        // "(?!snoPower)" guard keeps a null override from grabbing the next entry's clip.
        static const QRegularExpression rxPower(QStringLiteral(
            "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"
            "(?:(?!\"snoPower\")[\\s\\S])*?"
            "\"snoAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
        static const QRegularExpression rxPowerFemale(QStringLiteral(
            "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"
            "(?:(?!\"snoPower\")[\\s\\S])*?"
            "\"snoFemaleOverrideAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
        static const QRegularExpression rxSet(QStringLiteral(
            "\"sno(?:FemaleOverride)?Anim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
        QDirIterator it(dir, QStringList{QStringLiteral("*.ans.json")}, QDir::Files);
        while (it.hasNext()) {
            QFile f(it.next());
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString raw = QString::fromUtf8(f.readAll());
            QString setName = QFileInfo(f).fileName();
            if (setName.endsWith(QLatin1String(".ans.json"))) setName.chop(9);
            auto pi = rxPower.globalMatch(raw);
            while (pi.hasNext()) {
                const auto m = pi.next();
                const QString clip = m.captured(2).toLower();
                if (!clip.isEmpty() && !m_power.contains(clip)) m_power.insert(clip, m.captured(1));
            }
            auto fi = rxPowerFemale.globalMatch(raw);
            while (fi.hasNext()) {
                const auto m = fi.next();
                const QString clip = m.captured(2).toLower();   // female-override clip
                if (!clip.isEmpty() && !m_power.contains(clip)) m_power.insert(clip, m.captured(1));
            }
            auto si = rxSet.globalMatch(raw);
            while (si.hasNext()) {
                const auto m = si.next();
                const QString clip = m.captured(1).toLower();
                if (!clip.isEmpty() && !m_set.contains(clip)) m_set.insert(clip, setName);
            }
        }
    }

    bool built() const { return m_built; }
    // Readable action for a clip (e.g. "Walk", "Get Hit"), or empty if unknown.
    QString action(const QString& clipNameLower) const { return pretty(m_power.value(clipNameLower)); }
    // Owning AnimSet name for a clip, or empty.
    QString animSet(const QString& clipNameLower) const { return m_set.value(clipNameLower); }

private:
    // Raw snoPower name → readable label: drop "AnimKey_", underscores→spaces, split camelCase.
    static QString pretty(const QString& p)
    {
        if (p.isEmpty()) return {};
        QString s = p;
        if (s.startsWith(QLatin1String("AnimKey_"), Qt::CaseInsensitive)) s = s.mid(8);
        s.replace(QLatin1Char('_'), QLatin1Char(' '));
        QString out; out.reserve(s.size() + 6);
        for (int i = 0; i < s.size(); ++i) {
            if (i > 0 && s[i].isUpper() && !s[i - 1].isUpper() && s[i - 1] != QLatin1Char(' '))
                out += QLatin1Char(' ');
            out += s[i];
        }
        return out.trimmed();
    }

    bool m_built = false;
    QHash<QString, QString> m_power;   // clip (lower) → raw power name
    QHash<QString, QString> m_set;     // clip (lower) → AnimSet name
};
