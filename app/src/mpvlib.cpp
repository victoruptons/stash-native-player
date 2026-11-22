#include "mpvlib.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QStringList>

namespace Mpv {

namespace {

QStringList candidateLibraries()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList names;
#if defined(Q_OS_WIN)
    for (const char *n : {"libmpv-2.dll", "mpv-2.dll", "mpv-1.dll"})
        names << QDir(appDir).filePath(QString::fromLatin1(n)) << QString::fromLatin1(n);
#elif defined(Q_OS_MACOS)
    names << QDir(appDir).filePath(QStringLiteral("../Frameworks/libmpv.2.dylib"))
          << QDir(appDir).filePath(QStringLiteral("../Frameworks/libmpv.dylib"))
          << QStringLiteral("/opt/homebrew/lib/libmpv.2.dylib")
          << QStringLiteral("/usr/local/lib/libmpv.2.dylib")
          << QStringLiteral("libmpv.2.dylib");
#else
    names << QDir(appDir).filePath(QStringLiteral("libmpv.so.2"))
          << QStringLiteral("libmpv.so.2") << QStringLiteral("libmpv.so.1")
          << QStringLiteral("libmpv.so");
#endif
    return names;
}

struct Loader {
    Api api;
    bool ok = false;
    QString error;

    Loader()
    {
        QStringList tried;
        for (const QString &name : candidateLibraries()) {
            auto *lib = new QLibrary(name);
            if (!lib->load()) {
                tried << QStringLiteral("%1 (%2)").arg(name, lib->errorString());
                delete lib;
                continue;
            }
            bool all = true;
#define SNP_RESOLVE(fn)                                                                    \
    api.fn = reinterpret_cast<decltype(api.fn)>(lib->resolve(#fn));                        \
    if (!api.fn) {                                                                         \
        all = false;                                                                       \
        tried << QStringLiteral("%1 (missing symbol " #fn ")").arg(name);                  \
    }
            SNP_MPV_FUNCTIONS(SNP_RESOLVE)
#undef SNP_RESOLVE
            if (all) {
                ok = true; // keep the library loaded for the process lifetime
                return;
            }
            lib->unload();
            delete lib;
            api = Api{};
        }
        error = QStringLiteral("Could not load libmpv. Tried:\n  ") + tried.join(QStringLiteral("\n  "));
    }
};

} // namespace

const Api *api(QString *error)
{
    static Loader loader;
    if (!loader.ok) {
        if (error)
            *error = loader.error;
        return nullptr;
    }
    return &loader.api;
}

} // namespace Mpv
