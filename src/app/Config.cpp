#include "app/Config.h"
#include <QSettings>

namespace {
constexpr auto kGameDir    = "paths/gameDir";
constexpr auto kD4dataDir  = "paths/d4dataDir";
constexpr auto kTactKeys   = "paths/tactKeysPath";
constexpr auto kCascProduct = "casc/product";
}

QString Config::gameDir()                    { return QSettings().value(kGameDir).toString(); }
void    Config::setGameDir(const QString& d) { QSettings().setValue(kGameDir, d); }

QString Config::d4dataDir()                    { return QSettings().value(kD4dataDir).toString(); }
void    Config::setD4dataDir(const QString& d) { QSettings().setValue(kD4dataDir, d); }

QString Config::tactKeysPath()                    { return QSettings().value(kTactKeys).toString(); }
void    Config::setTactKeysPath(const QString& p) { QSettings().setValue(kTactKeys, p); }

QString Config::cascProduct()
{
    return QSettings().value(kCascProduct, QStringLiteral("fenris")).toString();
}
void Config::setCascProduct(const QString& c) { QSettings().setValue(kCascProduct, c); }
