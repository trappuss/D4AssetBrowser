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
#include <QMutex>
#include <QRegularExpression>
#include <QString>

#include <atomic>

class AnimActionIndex {
public:
    static AnimActionIndex& instance() { static AnimActionIndex i; return i; }

    // Build from a d4data root if not already built. Safe to call repeatedly AND from more than
    // one thread: the first caller builds, any other blocks until it is done. Measured at ~1700 ms
    // over the AnimSet/Emote/StringList trees, which is why it must not run on the GUI thread —
    // prewarm it from a worker (MainWindow's reload thread does) before the Wardrobe needs it.
    void ensure(const QString& d4)
    {
        if (m_built.load(std::memory_order_acquire) || d4.isEmpty()) return;
        QMutexLocker lock(&m_mutex);
        if (m_built.load(std::memory_order_relaxed)) return;   // another thread built it while we waited
        // NB: the flag is set at the END of this function, not here.
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
        // ── Emote Power -> the emote's LOCALIZED name ──
        // An emote clip's action label came out as its raw Power: "HRO Emote Bye", or for a store
        // emote "emote bar005 stor". Emote/<stem>.emo.json names the Power it fires, and
        // StringList/Emote_<stem>.stl carries the name the game actually shows — so
        // emote_bar005_stor is "Soulcrushed", not a product code. 160 of the 161 emote definitions
        // have one. The stem is the fallback for the one that does not. Read first so the map is
        // ready below.
        {
            const QString eDir = d4 + QStringLiteral("/json/base/meta/Emote");
            const QString sDir = d4 + QStringLiteral("/json/enUS_Text/meta/StringList");
            static const QRegularExpression rxEmotePower(QStringLiteral(
                "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"));
            // The Name entry inside the string list; szLabel and szText can appear either way round,
            // so both orders are accepted rather than assuming the one this snapshot happens to use.
            static const QRegularExpression rxName(QStringLiteral(
                "\"szLabel\":\\s*\"Name\"\\s*,\\s*\"szText\":\\s*\"((?:[^\"\\\\]|\\\\.)*)\""
                "|\"szText\":\\s*\"((?:[^\"\\\\]|\\\\.)*)\"\\s*,\\s*\"szLabel\":\\s*\"Name\""));
            QDirIterator ei(eDir, QStringList{QStringLiteral("*.emo.json")}, QDir::Files);
            while (ei.hasNext()) {
                QFile f(ei.next());
                if (!f.open(QIODevice::ReadOnly)) continue;
                const auto m = rxEmotePower.match(QString::fromUtf8(f.readAll()));
                if (!m.hasMatch()) continue;
                QString stem = QFileInfo(f).fileName();
                if (stem.endsWith(QLatin1String(".emo.json"))) stem.chop(9);
                QString label = stem;
                QFile sf(sDir + QStringLiteral("/Emote_%1.stl.json").arg(stem));
                if (sf.open(QIODevice::ReadOnly)) {
                    const auto sm = rxName.match(QString::fromUtf8(sf.readAll()));
                    if (sm.hasMatch()) {
                        const QString t = sm.captured(1).isEmpty() ? sm.captured(2) : sm.captured(1);
                        if (!t.isEmpty()) label = jsonUnescape(t);
                    }
                }
                m_emote.insert(m.captured(1).toLower(), label);
            }
        }

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
        // LAST, with release ordering: everything written above must be visible to any thread that
        // subsequently sees this flag without taking the mutex (the fast path at the top).
        m_built.store(true, std::memory_order_release);
    }

    bool built() const { return m_built.load(std::memory_order_acquire); }

    // Drop everything so the next ensure() re-reads the snapshot.
    //
    // This index is memory-only and was built once per process with no way back, so switching the
    // d4data folder — or pulling a newer snapshot — left every clip still labelled from the OLD
    // one until the app was restarted. Nothing said so; the labels simply stayed wrong. Wired into
    // MainWindow's fingerprint guard alongside the other indexes.
    //
    // Takes the same mutex ensure() holds, so a reset landing while a build is in flight waits for
    // it rather than tearing the maps out from under it. The build then finishes and marks itself
    // built; the following ensure() sees a cleared, unbuilt index and redoes it.
    void reset()
    {
        QMutexLocker lock(&m_mutex);
        m_built.store(false, std::memory_order_release);
        m_power.clear();
        m_set.clear();
        m_emote.clear();
    }
    // Readable action for a clip (e.g. "Walk", "Get Hit"), or empty if unknown.
    QString action(const QString& clipNameLower) const
    {
        const QString power = m_power.value(clipNameLower);
        // An emote is named by its Emote definition, not by the Power that fires it.
        const QString e = m_emote.value(power.toLower());
        // A localized name is shown as authored — "To War!" keeps its punctuation. Only the raw
        // stem fallback goes through the prettifier.
        if (e.isEmpty()) return pretty(power);
        return e.contains(QLatin1Char('_')) ? prettyEmote(e) : e;
    }
    // Owning AnimSet name for a clip, or empty.
    QString animSet(const QString& clipNameLower) const { return m_set.value(clipNameLower); }

private:
    // The capture is raw JSON text, so a name that quotes itself arrives escaped — two of the 161
    // emotes do exactly that and read as \"To War!\" without this. Only the escapes a string-table
    // name can actually contain are handled; anything else is left alone rather than mangled.
    static QString jsonUnescape(const QString& in)
    {
        if (!in.contains(QLatin1Char('\\'))) return in;
        QString out; out.reserve(in.size());
        for (int i = 0; i < in.size(); ++i) {
            if (in[i] != QLatin1Char('\\') || i + 1 >= in.size()) { out += in[i]; continue; }
            const QChar n = in[++i];
            if      (n == QLatin1Char('n'))  out += QLatin1Char('\n');
            else if (n == QLatin1Char('t'))  out += QLatin1Char('\t');
            else if (n == QLatin1Char('r'))  {}                       // dropped: never wanted in a label
            else                             out += n;                // \" \\ \/ and anything else
        }
        return out;
    }

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

    // Emote stems are mostly already readable ("Bye", "Cheer"); the store ones are not
    // ("emote_promo_WingsOfTheCreator"), so the shared prettifier runs after the prefix is dropped.
    static QString prettyEmote(const QString& n)
    {
        QString s = n;
        for (const char* pfx : {"emote_promo_", "emote_"})
            if (s.startsWith(QLatin1String(pfx), Qt::CaseInsensitive)) { s = s.mid(int(qstrlen(pfx))); break; }
        return pretty(s);
    }

    // Atomic + mutex because ensure() is now called from a WORKER thread at startup as well as
    // from the GUI thread on first use. The old `bool m_built = true;` set at the TOP of the build
    // was a race waiting to happen: a second caller saw "built" and read a half-filled map while
    // the first was still writing it. The flag is now set at the END, and a caller that arrives
    // mid-build waits rather than reading torn state.
    std::atomic<bool> m_built{false};
    QMutex m_mutex;
    QHash<QString, QString> m_power;   // clip (lower) → raw power name
    QHash<QString, QString> m_set;     // clip (lower) → AnimSet name
    QHash<QString, QString> m_emote;   // power (lower) → Emote definition name
};
