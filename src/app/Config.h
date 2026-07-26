#pragma once
#include <QString>

// Thin wrapper over QSettings for the two paths the app needs. Persists to the
// platform's native store (registry on Windows) under the org/app names set in
// main(). Mirrors the Python fork's userdata/settings.json (game_dir, d4data_dir).
class Config {
public:
    static QString gameDir();
    static void    setGameDir(const QString& dir);

    static QString d4dataDir();
    static void    setD4dataDir(const QString& dir);

    static QString tactKeysPath();
    static void    setTactKeysPath(const QString& path);

    // CASC product code for the install. Diablo IV's code is "fenris"; exposed so
    // PTR/other products can be selected without a rebuild.
    static QString cascProduct();           // default: "fenris"
    static void    setCascProduct(const QString& code);
    // (The "show only decrypted" filter is now a per-tab toggle in the Files/Textures/Models tabs,
    //  not a global setting — the old Config accessors were removed.)
};
