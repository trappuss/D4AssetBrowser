#include "model/MaterialReport.h"

#include "casc/CascReader.h"
#include "index/AssetLinks.h"
#include "index/SnoIndex.h"
#include "model/Material.h"
#include "model/AppearanceMatBin.h"
#include "model/MaterialDecode.h"
#include "tex/TextureDefTable.h"

#include <QStringList>

#include <algorithm>

// Appearance. Hard-coded because it is one of the most stable ids in the format, and it is only
// used to turn an sno the report already holds into a name.
//
// There is deliberately NO kGroupMaterial: materials live in TWO groups (37 and 57 — MainWindow
// builds its name resolver from both, under a comment saying nothing has established that group 37
// holds nothing of interest), and the report already resolves the material sno through
// MaterialDecode::snoForMaterial, which covers both AND reads the sno straight out of an
// "~unnamed_<sno>" placeholder. A second lookup here through SnoIndex::snoForName would have been
// narrower on both counts — it skips placeholders outright, so it would have answered "no sno" for
// exactly the encrypted materials this section exists to trace.
constexpr int kGroupAppearance = 9;

namespace {

// Every line is either a measurement or an explicit "not known". There is deliberately no third
// state: a blank value is what made these failures invisible in the first place.
QString row(const QString& label, const QString& value)
{
    return QStringLiteral("  %1  %2\n").arg(label, -22).arg(value);
}


}   // namespace

QString MaterialReport::explain(CascReader* reader, SnoIndex* index, const QString& d4,
                                const QString& appearanceName, int appearanceSno,
                                const QString& materialName)
{
    QString out;
    out += QStringLiteral("WHY THIS PART LOOKS THE WAY IT DOES\n"
                          "===================================\n\n");

    // ── The appearance ──────────────────────────────────────────────────────────────────────────
    out += QStringLiteral("APPEARANCE\n");
    out += row(QStringLiteral("name"), appearanceName.isEmpty()
                                           ? QStringLiteral("(unknown)") : appearanceName);
    out += row(QStringLiteral("sno"), appearanceSno > 0 ? QString::number(appearanceSno)
                                                        : QStringLiteral("(unknown)"));
    QByteArray meta;
    bool encrypted = false, keyHeld = true;
    if (reader && reader->isReady() && appearanceSno > 0) {
        const QByteArray kn = reader->tactKeyFor(quint64(appearanceSno));
        encrypted = !kn.isEmpty();
        keyHeld   = encrypted ? reader->haveTactKey(kn) : true;
        meta      = reader->readMetaBySno(quint64(appearanceSno));
        out += row(QStringLiteral("TACT-encrypted"),
                   encrypted ? QStringLiteral("YES — key %1 (%2)")
                                   .arg(QString::fromLatin1(kn.toHex()),
                                        keyHeld ? QStringLiteral("held") : QStringLiteral("NOT HELD"))
                             : QStringLiteral("no"));
    } else {
        out += row(QStringLiteral("TACT-encrypted"),
                   QStringLiteral("(no CASC reader — cannot tell)"));
    }

    // ── Where the roster came from ──────────────────────────────────────────────────────────────
    // The single most useful line here. An appearance that can only be read from the binary is one
    // that every .app.json-only call site loses in silence, and that is not visible anywhere else.
    out += QStringLiteral("\nMATERIAL ROSTER\n");
    // Hoisted: the whole-model summary below reports every entry, and re-deriving it there would
    // let the two halves of one report disagree about what the roster is.
    QStringList roster;
    if (appearanceName.isEmpty()) {
        out += row(QStringLiteral("source"), QStringLiteral("(no appearance name to look up)"));
    } else {
        const QStringList jsonRoster = MaterialDecode::appearanceRoster(d4, appearanceName);
        QStringList metaRoster;
        QString why;
        if (!meta.isEmpty())
            metaRoster = MaterialDecode::appearanceRosterFromMeta(meta, index, &why);
        const bool haveJson = !jsonRoster.isEmpty();
        roster = haveJson ? jsonRoster : metaRoster;
        out += row(QStringLiteral("source"),
                   haveJson ? QStringLiteral(".app.json  (%1 entries)").arg(jsonRoster.size())
                 : !metaRoster.isEmpty()
                            ? QStringLiteral("CASC meta binary  (%1 entries) — no .app.json in "
                                             "this d4data snapshot").arg(metaRoster.size())
                            : QStringLiteral("NEITHER — this part has no material name at all"));
        if (!haveJson && metaRoster.isEmpty() && !why.isEmpty())
            out += row(QStringLiteral("binary read said"), why);
        if (!haveJson && !metaRoster.isEmpty())
            out += QStringLiteral(
                "    Any code path that reads only the .app.json loses this appearance entirely:\n"
                "    empty roster, empty material names, untextured mesh, no error. Read it\n"
                "    through MaterialDecode::appearanceRosterAny.\n");
    }

    // ── The material ────────────────────────────────────────────────────────────────────────────
    // ── Whole-model mode ────────────────────────────────────────────────────────────────────────
    // Asked from the browse list, where no single part has been clicked. Every material the
    // appearance uses, one line each — because "which of these is the reason it looks wrong" is a
    // question you ask BEFORE you know which part to right-click, and making someone load the model
    // and drill into a part first is asking them to already know the answer.
    if (materialName.isEmpty() && !roster.isEmpty()) {
        QStringList seen;
        for (const QString& m : roster)
            if (!m.isEmpty() && !seen.contains(m)) seen << m;
        out += QStringLiteral("\nMATERIALS (%1)\n").arg(seen.size());
        int defaulted = 0;
        for (const QString& m : seen) {
            const bool haveJson = !MaterialDecode::uberMaterial(d4, m).isEmpty();
            if (!haveJson) ++defaulted;
            const QVector<MatTexture> ts = MaterialDecode::texturesFor(reader, d4, m);
            int ok = 0;
            for (const MatTexture& t : ts)
                if (t.texSno > 0 && TextureDefTable::instance().lookup(int(t.texSno)).valid()) ++ok;
            out += row(m, QStringLiteral("%1  ·  %2/%3 texture(s) resolve")
                              .arg(haveJson ? QStringLiteral("values authored")
                                            : QStringLiteral("VALUES DEFAULTED"))
                              .arg(ok).arg(ts.size()));
        }
        if (defaulted > 0)
            out += QStringLiteral(
                       "\n  %1 of %2 material(s) have no .mat.json, so their metallic, roughness,\n"
                       "  emissive multiplier and emissive colour are stand-ins the tool chose —\n"
                       "  not values the game authored. Right-click a PART for the full breakdown.\n")
                       .arg(defaulted).arg(seen.size());
        else
            out += QStringLiteral("\n  Every material's values are authored. Right-click a PART for "
                                  "the full breakdown of one.\n");
        return out;
    }

    out += QStringLiteral("\nMATERIAL\n");
    if (materialName.isEmpty()) {
        out += row(QStringLiteral("name"), QStringLiteral("(none — the roster gave this part no "
                                                          "material)"));
        out += QStringLiteral(
            "\n  A part with no material name renders untextured. That is a ROSTER failure, not a\n"
            "  texture one — see the section above for which route answered.\n");
        return out;
    }
    out += row(QStringLiteral("name"), materialName);
    // ── The SNO round trip ──────────────────────────────────────────────────────────────────────
    // Two independent routes reach this material's sno, and everything downstream depends on them
    // agreeing:
    //
    //   the APPEARANCE binary states it directly (AppearanceMatBin, +24 in the SOA), and
    //   snoForMaterial() takes the NAME back through the index and returns a sno.
    //
    // texturesFor() uses the SECOND one — matTexFromMeta(reader, snoForMaterial(name)) — so if the
    // name round-trips to a different record, every "texture" sno below is whatever those unrelated
    // bytes happened to contain, and each one still reports "definition resolves" because a valid
    // sno is a valid sno. That failure is invisible by construction, which is why it is checked
    // here rather than assumed. snoForMaterial's own header warns about exactly this.
    const qint64 matSno = MaterialDecode::snoForMaterial(materialName);
    int binSno = 0;
    if (!meta.isEmpty()) {
        const QVector<AppearanceMatBin::Entry> ents = AppearanceMatBin::read(meta);
        const QStringList metaNames = MaterialDecode::appearanceRosterFromMeta(meta, index);
        // Positional: entry i IS roster entry i, by the reader's own contract.
        for (int i = 0; i < ents.size() && i < metaNames.size(); ++i)
            if (metaNames.at(i).compare(materialName, Qt::CaseInsensitive) == 0) {
                binSno = ents.at(i).sno;
                break;
            }
    }
    out += row(QStringLiteral("sno (by name)"),
               matSno > 0 ? QString::number(matSno) : QStringLiteral("(not in the index)"));
    if (binSno > 0) {
        out += row(QStringLiteral("sno (in the binary)"), QString::number(binSno));
        if (matSno > 0 && qint64(binSno) != matSno)
            out += QStringLiteral(
                       "\n  *** MISMATCH ***  The appearance names material sno %1; the material NAME\n"
                       "  resolves back to %2. texturesFor() follows the NAME, so every texture listed\n"
                       "  below was read out of the wrong record — and each will still say its\n"
                       "  definition resolves, because it is a real sno for something else.\n")
                       .arg(binSno).arg(matSno);
        else if (matSno > 0)
            out += row(QStringLiteral("round trip"), QStringLiteral("agrees"));
    }
    // The shader decides how the Wardrobe treats this part — facial hair is identified by
    // hero_opaque_hollow, eyes by Hero_Eye, hair by hero_hair — so a report that omits it cannot
    // explain why a part took the path it took. This line is the one that identified rogM_P00's
    // lambert1_skin as the facial-hair slot rather than as ordinary skin.
    const QString shader = MaterialDecode::shaderMap(d4, materialName);
    out += row(QStringLiteral("shader"),
               shader.isEmpty() ? QStringLiteral("(none recorded)") : shader);
    const bool haveMatJson = !MaterialDecode::uberMaterial(d4, materialName).isEmpty();
    out += row(QStringLiteral(".mat.json"),
               haveMatJson ? QStringLiteral("present")
                           : QStringLiteral("ABSENT — every authored value below is a stand-in"));

    // ── Authored versus assumed ─────────────────────────────────────────────────────────────────
    // The distinction the whole report exists for. "roughness 0.6" reads identically whether the
    // game authored it or the tool picked it, and that ambiguity hid encrypted content for months.
    out += QStringLiteral("\nVALUES\n");
    float metal = 0.0f, rough = 0.6f;
    bool  authored = false;
    MaterialDecode::factors(reader, d4, materialName, metal, rough, &authored);
    // ── Does the defaulted scalar actually REACH the pixel? ─────────────────────────────────────
    // Usually not, and saying "DEFAULT (assumed)" without saying so overstates it. The shader is
    // unambiguous (GLModelWidget kFrag):
    //
    //     float metal=uMetal, rough=uRough, ao=1.0;
    //     if (uHasOrm==1) { vec3 o=texture(uOrmTex,vUV).rgb; ao=o.r; rough=o.g; metal=o.b; }
    //
    // A packed ORM overrides both scalars per texel, so for any material that ships AO, ROUGHNESS
    // or METALLIC maps the authored scalars are dead weight and their absence costs nothing. The
    // scalars only reach the surface on a material with none of those three — and THAT is the
    // population a missing .mat.json actually damages.
    bool hasOrmTex = false;
    for (const char* role : {"AO", "ROUGHNESS", "METALLIC"})
        if (!MaterialDecode::byRole(reader, d4, materialName, role).isNull()) { hasOrmTex = true; break; }
    const QString tag = authored ? QStringLiteral("authored") : QStringLiteral("DEFAULT (assumed)");
    const QString ormNote = hasOrmTex
        ? QStringLiteral("  — UNUSED: the ORM map overrides it per texel")
        : QString();
    out += row(QStringLiteral("metallic"),
               QStringLiteral("%1  %2%3").arg(metal, 0, 'f', 3).arg(tag, ormNote));
    out += row(QStringLiteral("roughness"),
               QStringLiteral("%1  %2%3").arg(rough, 0, 'f', 3).arg(tag, ormNote));
    const float emisMul = MaterialDecode::materialScalar(d4, materialName,
                                                         "emissive multiplier", 1.0f);
    out += row(QStringLiteral("emissive multiplier"),
               QStringLiteral("%1  %2").arg(emisMul, 0, 'f', 3)
                   .arg(haveMatJson ? QStringLiteral("authored")
                                    : QStringLiteral("DEFAULT (assumed)")));
    const QColor authoredCol = MaterialDecode::materialColor(d4, materialName, "emissive color");
    if (authoredCol.isValid())
        out += row(QStringLiteral("emissive colour"),
                   QStringLiteral("%1  authored").arg(authoredCol.name()));
    else
        out += row(QStringLiteral("emissive colour"),
                   QStringLiteral("not authored — derived from the base map under the mask, or "
                                  "white if the map is already coloured"));
    if (!haveMatJson) {
        out += QStringLiteral(
            "\n  There is a binary route for a material's TEXTURES and none yet for its VALUES, so\n"
            "  every number above was chosen by the tool rather than read from the game. It is not a\n"
            "  decode failure and it will not show up as one. See D4_MATVALUE_DUMP.\n");
        // What that actually COSTS, per material, instead of one blanket warning. The two emissive
        // values have no texture that can stand in for them; metallic and roughness usually do.
        out += hasOrmTex
            ? QStringLiteral(
                  "\n  For THIS material the damage is limited to the emissive pair: it ships an ORM\n"
                  "  map, so metallic and roughness come from real texels regardless. Emissive\n"
                  "  multiplier and colour have no texture equivalent — the shader multiplies by\n"
                  "  them directly — so those two are the only fabricated values that reach the\n"
                  "  screen.\n")
            : QStringLiteral(
                  "\n  This material ships NO AO, roughness or metallic map, so the defaulted\n"
                  "  scalars above are what the shader actually uses. This is the case where a\n"
                  "  missing .mat.json changes the surface, not just the glow.\n");
    }

    // ── Textures ────────────────────────────────────────────────────────────────────────────────
    out += QStringLiteral("\nTEXTURES\n");
    const QVector<MatTexture> texs = MaterialDecode::texturesFor(reader, d4, materialName);
    if (texs.isEmpty()) {
        out += row(QStringLiteral("(none)"),
                   QStringLiteral("the material references no textures by either route"));
    } else {
        int resolved = 0;
        for (const MatTexture& t : texs) {
            // int(): lookup takes an int and texSno is qint64. Narrowing is explicit here so it
            // reads as a decision rather than a warning nobody looked at.
            const bool def = t.texSno > 0
                          && TextureDefTable::instance().lookup(int(t.texSno)).valid();
            if (def) ++resolved;
            out += row(t.role.isEmpty() ? QStringLiteral("slot %1").arg(t.slot) : t.role,
                       QStringLiteral("sno %1  %2%3")
                           .arg(t.texSno)
                           .arg(def ? QStringLiteral("definition resolves")
                                    : QStringLiteral("NO DEFINITION — cannot decode"))
                           .arg(t.texName.isEmpty() ? QString()
                                                    : QStringLiteral("  (%1)").arg(t.texName)));
        }
        out += QStringLiteral("\n  %1 of %2 texture definition(s) resolve.\n")
                   .arg(resolved).arg(texs.size());
    }

    // ── Who else uses it ───────────────────────────────────────────────────────────────────────
    // Materials are shared far more than they look: the female Paladin sets50 pieces author no
    // material of their own and reference the MALE set's, and the wolfHead ornament that broke the
    // HED toggle is one material on several pieces. Without this, "is this material mine or shared"
    // is a CoreTOC sweep; with it, it is the bottom of a report already one right-click away.
    //
    // AssetLinks built this map for the Textures tab and only ever exposed it by texture. Reading
    // it costs a hash lookup — no files, no decode — so the report stays pure.
    out += QStringLiteral("\nUSED BY\n");
    {
        // matSno is the one resolved further up and already printed as "sno (by name)" — reused so
        // this section cannot quietly disagree with the line above it. int() because AssetLinks
        // keys on int; the narrowing is explicit rather than implicit, and snos are well inside it.
        const AssetLinks& links = AssetLinks::instance();
        if (!links.ready()) {
            // Said plainly rather than printing an empty list: "no appearances use this" and "the
            // index has not finished" look identical and mean opposite things.
            out += row(QStringLiteral("(unavailable)"),
                       links.building()
                           ? QStringLiteral("the asset-link index is still building — reopen this "
                                            "report in a moment")
                           : QStringLiteral("the asset-link index has not been built"));
        } else if (!index || !index->isLoaded()) {
            // Distinct from "the name resolved to nothing" — same reason as the branch above.
            out += row(QStringLiteral("(unavailable)"),
                       QStringLiteral("the SNO index is not loaded, so appearance snos cannot be "
                                      "named"));
        } else if (matSno <= 0) {
            out += row(QStringLiteral("(unknown)"),
                       QStringLiteral("the material name resolved to no sno — see \"sno (by name)\" "
                                      "above"));
        } else {
            QVector<int> apps = links.appsForMaterial(int(matSno));
            std::sort(apps.begin(), apps.end());
            apps.erase(std::unique(apps.begin(), apps.end()), apps.end());
            if (apps.isEmpty()) {
                out += row(QStringLiteral("(none)"),
                           QStringLiteral("no appearance in d4data references material sno %1")
                               .arg(matSno));
            } else {
                out += QStringLiteral("  material sno %1 is referenced by %2 appearance(s)")
                           .arg(matSno).arg(apps.size());
                // Capped: a shared body material runs to hundreds, and a context-menu report that
                // scrolls for a page has buried its own first section.
                const int kMax = 40;
                out += apps.size() > kMax ? QStringLiteral(" — first %1:\n").arg(kMax)
                                          : QStringLiteral(":\n");
                for (int i = 0; i < apps.size() && i < kMax; ++i) {
                    const QString nm = index->nameForSno(kGroupAppearance, apps[i]);
                    // NOT "encrypted": nameForSno returns empty both for a blanked name and for an
                    // sno this index simply does not hold. AssetLinks is built from d4data while
                    // the index normally comes from live CASC, so a d4data snapshot AHEAD of the
                    // installed build lands here routinely and has nothing to do with encryption.
                    out += QStringLiteral("    %1  %2\n")
                               .arg(apps[i], 9)
                               .arg(nm.isEmpty() ? QStringLiteral("(no name in this index)") : nm);
                }
                if (apps.size() > kMax)
                    out += QStringLiteral("    … and %1 more\n").arg(apps.size() - kMax);
            }
        }
    }

    out += QStringLiteral("\n(Nothing here decodes pixels — these are the definitions the renderer "
                          "starts from.)\n");
    return out;
}
