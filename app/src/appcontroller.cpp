#include "appcontroller.h"

#include "bridge.h"
#include "livesubs.h"
#include "mpvitem.h"

#include <QCoreApplication>
#include <QFile>
#include <QQuickWebEngineProfile>

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_bridge(new Bridge(this))
    , m_liveSubs(new LiveSubs(this))
{
    m_serverUrl = normalizeServerUrl(m_settings.value(QStringLiteral("serverUrl")).toString());
    m_zoom = qBound(0.25, m_settings.value(QStringLiteral("zoomFactor"), 1.0).toReal(), 5.0);
    m_recent = m_settings.value(QStringLiteral("recentServers")).toStringList();
    if (m_recent.isEmpty() && hasServer())
        m_recent << m_serverUrl.toString();

    // "--server <url>" opens a server for this run only, without changing the
    // saved address (handy for a test server).
    const QStringList args = QCoreApplication::arguments();
    const qsizetype i = args.indexOf(QStringLiteral("--server"));
    if (i >= 0 && i + 1 < args.size()) {
        const QUrl url = normalizeServerUrl(args.at(i + 1));
        if (!url.isEmpty()) {
            m_serverUrl = url;
            m_temporaryServer = true;
        }
    }
    m_bridge->setServerUrl(m_serverUrl);
    m_bridge->setLiveSubs(m_liveSubs);
    m_liveSubs->setCookieProvider([bridge = m_bridge](const QUrl &url) { return bridge->cookieHeaderFor(url); });
    m_liveSubs->setServer(m_serverUrl);
}

QString AppController::version() const
{
    return QCoreApplication::applicationVersion();
}

QString AppController::injectedScript() const
{
    QFile f(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    // Mark the document as running inside the app as early as possible, so the
    // plugin's CSS keeps the page root transparent from the first paint (see
    // native-player.js for why that matters).
    static const char marker[] = R"JS(
;(function () {
  window.__stashNativePlayer = true;
  function mark() {
    if (!document.documentElement) return false;
    document.documentElement.classList.add("snp-app");
    return true;
  }
  if (!mark()) {
    new MutationObserver(function (_, observer) {
      if (mark()) observer.disconnect();
    }).observe(document, { childList: true });
  }
})();
)JS";
    return QString::fromUtf8(f.readAll()) + QString::fromLatin1(marker);
}

void AppController::setZoomFactor(qreal zoom)
{
    zoom = qBound(0.25, zoom, 5.0);
    if (qFuzzyCompare(zoom, m_zoom))
        return;
    m_zoom = zoom;
    m_settings.setValue(QStringLiteral("zoomFactor"), zoom);
    emit zoomFactorChanged();
}

QUrl AppController::normalizeServerUrl(const QString &input) const
{
    QString s = input.trimmed();
    if (s.isEmpty())
        return {};
    if (!s.contains(QStringLiteral("://")))
        s.prepend(QStringLiteral("http://"));
    QUrl url(s, QUrl::TolerantMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    if (url.path().isEmpty())
        url.setPath(QStringLiteral("/"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

bool AppController::setServer(const QString &input)
{
    const QUrl url = normalizeServerUrl(input);
    if (url.isEmpty())
        return false;
    m_serverUrl = url;
    m_temporaryServer = false;
    m_settings.setValue(QStringLiteral("serverUrl"), url.toString());
    m_recent.removeAll(url.toString());
    m_recent.prepend(url.toString());
    while (m_recent.size() > 8)
        m_recent.removeLast();
    m_settings.setValue(QStringLiteral("recentServers"), m_recent);
    m_bridge->setServerUrl(url);
    m_liveSubs->setServer(url);
    emit serverUrlChanged();
    return true;
}

void AppController::forgetServer(const QString &url)
{
    if (m_recent.removeAll(url) == 0)
        return;
    m_settings.setValue(QStringLiteral("recentServers"), m_recent);
    emit serverUrlChanged();
}

bool AppController::isOnServer(const QUrl &url) const
{
    if (!hasServer())
        return false;
    auto port = [](const QUrl &u) { return u.port(u.scheme() == QLatin1String("https") ? 443 : 80); };
    return url.host().compare(m_serverUrl.host(), Qt::CaseInsensitive) == 0 && port(url) == port(m_serverUrl);
}

void AppController::attach(QObject *webProfile, QObject *player)
{
    if (auto *profile = qobject_cast<QQuickWebEngineProfile *>(webProfile))
        m_bridge->setCookieStore(profile->cookieStore());
    m_bridge->setPlayer(qobject_cast<MpvItem *>(player));
    m_liveSubs->setPlayer(qobject_cast<MpvItem *>(player));
}
