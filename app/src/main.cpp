#include "appcontroller.h"
#include "bridge.h"
#include "livesubs.h"
#include "mpvitem.h"
#include "winframe.h"

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <clocale>

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("StashNativePlayer"));
    QCoreApplication::setApplicationName(QStringLiteral("Stash Native Player"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SNP_VERSION));

    // mpv's render API is OpenGL; Qt Quick must use OpenGL too (Qt 6 defaults
    // to Direct3D 11 on Windows and Metal on macOS).
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    // With Qt Quick on desktop OpenGL, Chromium leaves page rasterization on
    // the CPU, which makes scrolling and page changes much slower than in a
    // normal browser. Turn GPU rasterization on (SNP_NO_GPU_RASTER=1 skips it).
    if (!qEnvironmentVariableIsSet("SNP_NO_GPU_RASTER")) {
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        flags += " --enable-gpu-rasterization --ignore-gpu-blocklist --enable-zero-copy";
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.trimmed());
    }
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    // taskbar / Alt+Tab icon (the .exe icon itself comes from app.rc)
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/stash.ico")));

    // libmpv requires the C numeric locale.
    std::setlocale(LC_NUMERIC, "C");

    qmlRegisterType<MpvItem>("StashNative", 1, 0, "MpvItem");
    qmlRegisterUncreatableType<LiveSubs>("StashNative", 1, 0, "LiveSubs", QStringLiteral("Provided by the app"));
    qmlRegisterUncreatableType<Bridge>("StashNative", 1, 0, "Bridge", QStringLiteral("Provided by the app"));

    AppController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    // SNP_QML lets developers load an alternative QML file (for testing).
    const QString qmlOverride = qEnvironmentVariable("SNP_QML");
    engine.load(qmlOverride.isEmpty() ? QUrl(QStringLiteral("qrc:/qml/Main.qml")) : QUrl::fromLocalFile(qmlOverride));
    if (engine.rootObjects().isEmpty())
        return 1;
    installWindowFrame(qobject_cast<QWindow *>(engine.rootObjects().first()));

    return app.exec();
}
