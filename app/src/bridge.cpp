#include "bridge.h"

#include "livesubs.h"
#include "mpvitem.h"

#include <QDebug>
#include <QNetworkCookie>
#include <QWebEngineCookieStore>

namespace {
constexpr qint64 kPositionIntervalMs = 250;
}

Bridge::Bridge(QObject *parent)
    : QObject(parent)
{
    m_positionTimer.start();
}

void Bridge::setPlayer(MpvItem *player)
{
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (!player)
        return;
    connect(player, &MpvItem::propertyChanged, this, &Bridge::onPropertyChanged);
    connect(player, &MpvItem::fileLoaded, this, &Bridge::onFileLoaded);
    connect(player, &MpvItem::endFile, this, &Bridge::onEndFile);
    for (const char *p : {"time-pos", "duration", "pause", "paused-for-cache", "seeking", "volume", "mute",
                          "speed", "eof-reached", "track-list"})
        player->observe(QString::fromLatin1(p));
}

void Bridge::setCookieStore(QWebEngineCookieStore *store)
{
    if (!store)
        return;
    auto key = [](const QNetworkCookie &c) { return c.name() + '|' + c.domain().toUtf8() + '|' + c.path().toUtf8(); };
    connect(store, &QWebEngineCookieStore::cookieAdded, this, [this, key](const QNetworkCookie &c) {
        m_cookies.insert(key(c), Cookie{c.domain(), c.path(), c.name() + '=' + c.value()});
    });
    connect(store, &QWebEngineCookieStore::cookieRemoved, this,
            [this, key](const QNetworkCookie &c) { m_cookies.remove(key(c)); });
    store->loadAllCookies();
}

bool Bridge::isAllowedUrl(const QUrl &url) const
{
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    if (!m_server.isValid())
        return true;
    return url.host().compare(m_server.host(), Qt::CaseInsensitive) == 0 &&
           url.port(scheme == QLatin1String("https") ? 443 : 80) ==
               m_server.port(m_server.scheme() == QLatin1String("https") ? 443 : 80);
}

QString Bridge::cookieHeaderFor(const QUrl &url) const
{
    const QString host = url.host().toLower();
    const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    QStringList parts;
    for (const Cookie &c : m_cookies) {
        QString domain = c.domain.toLower();
        if (domain.startsWith(QLatin1Char('.')))
            domain.remove(0, 1);
        const bool domainOk = domain.isEmpty() || host == domain || host.endsWith(QLatin1Char('.') + domain);
        const bool pathOk = c.path.isEmpty() || path.startsWith(c.path);
        if (domainOk && pathOk)
            parts << QString::fromUtf8(c.value);
    }
    return parts.join(QStringLiteral("; "));
}

void Bridge::load(const QString &urlString, double startSeconds, bool paused)
{
    const QUrl url(urlString);
    if (!m_player || !m_player->isReady()) {
        emit errorOccurred(m_player ? m_player->loadError() : QStringLiteral("Native player is not available"));
        return;
    }
    if (!isAllowedUrl(url)) {
        emit errorOccurred(QStringLiteral("Refusing to play a URL outside the Stash server: ") +
                           url.toDisplayString());
        return;
    }

    m_loaded = false;
    m_eofSent = false;
    m_pendingSubs.clear();

    const QString cookies = cookieHeaderFor(url);
    m_player->setPropertyValue(QStringLiteral("http-header-fields"),
                               cookies.isEmpty() ? QStringList{} : QStringList{QStringLiteral("Cookie: ") + cookies});
    m_player->setPropertyValue(QStringLiteral("start"),
                               startSeconds > 0 ? QString::number(startSeconds, 'f', 3) : QStringLiteral("none"));
    m_player->setPropertyValue(QStringLiteral("pause"), paused);
    m_player->command({QStringLiteral("loadfile"), url.toString(QUrl::FullyEncoded), QStringLiteral("replace")});
    if (m_liveSubs)
        m_liveSubs->mediaStarted(url, cookies, startSeconds);

    m_pendingState.insert(QStringLiteral("position"), startSeconds > 0 ? startSeconds : 0.0);
    m_pendingState.insert(QStringLiteral("paused"), paused);
    m_pendingState.insert(QStringLiteral("buffering"), true);
    flushState();
}

void Bridge::stop()
{
    m_loaded = false;
    if (m_liveSubs)
        m_liveSubs->mediaStopped();
    m_pendingSubs.clear();
    if (m_player)
        m_player->command({QStringLiteral("stop")});
}

void Bridge::setPaused(bool paused)
{
    if (m_player)
        m_player->setPropertyValue(QStringLiteral("pause"), paused);
}

void Bridge::seek(double seconds)
{
    if (!m_player)
        return;
    m_eofSent = false;
    if (!m_loaded) {
        // not loaded yet: just move the start position
        m_player->setPropertyValue(QStringLiteral("start"), QString::number(qMax(0.0, seconds), 'f', 3));
        return;
    }
    m_player->command({QStringLiteral("seek"), QString::number(qMax(0.0, seconds), 'f', 3),
                       QStringLiteral("absolute")});
}

void Bridge::seekRelative(double seconds)
{
    if (!m_player || !m_loaded)
        return;
    m_eofSent = false;
    m_player->command({QStringLiteral("seek"), QString::number(seconds, 'f', 3),
                       QStringLiteral("relative+exact")});
}

void Bridge::setVolume(double volume)
{
    if (m_player)
        m_player->setPropertyValue(QStringLiteral("volume"), qBound(0.0, volume, 100.0));
}

void Bridge::setMuted(bool muted)
{
    if (m_player)
        m_player->setPropertyValue(QStringLiteral("mute"), muted);
}

void Bridge::setSpeed(double speed)
{
    if (m_player)
        m_player->setPropertyValue(QStringLiteral("speed"), qBound(0.1, speed, 8.0));
}

void Bridge::setTrack(const QString &type, int id)
{
    if (!m_player || (type != QLatin1String("aid") && type != QLatin1String("sid")))
        return;
    if (id <= 0)
        m_player->setPropertyValue(type, QStringLiteral("no"));
    else
        m_player->setPropertyValue(type, qlonglong(id));
}

void Bridge::addSubtitle(const QString &urlString, const QString &title, const QString &lang, bool select)
{
    const QUrl url(urlString);
    if (!m_player || !isAllowedUrl(url))
        return;
    if (!m_loaded) {
        m_pendingSubs.append(QVariantList{urlString, title, lang, select});
        return;
    }
    // While live subtitles are on, they show the scene's captions themselves
    // (with the translation); don't show a caption track on top of them.
    if (m_liveSubs && m_liveSubs->mode() != 0)
        select = false;
    m_player->command({QStringLiteral("sub-add"), url.toString(QUrl::FullyEncoded),
                       select ? QStringLiteral("select") : QStringLiteral("auto"), title, lang});
}

void Bridge::setVideoGeometry(double x, double y, double width, double height, bool visible)
{
    const QRectF rect(x, y, qMax(0.0, width), qMax(0.0, height));
    if (rect == m_rect && visible == m_visible)
        return;
    m_rect = rect;
    m_visible = visible;
    emit videoGeometryChanged();
}

void Bridge::onFileLoaded()
{
    m_loaded = true;
    if (m_player)
        m_player->setPropertyValue(QStringLiteral("start"), QStringLiteral("none"));
    const auto subs = m_pendingSubs;
    m_pendingSubs.clear();
    for (const QVariantList &s : subs)
        addSubtitle(s[0].toString(), s[1].toString(), s[2].toString(), s[3].toBool());
    emit fileLoaded();
}

void Bridge::onEndFile(const QString &reason, const QString &error)
{
    m_loaded = false;
    if (reason == QLatin1String("error"))
        emit errorOccurred(error.isEmpty() ? QStringLiteral("Playback failed") : QStringLiteral("Playback failed: ") + error);
}

void Bridge::onPropertyChanged(const QString &name, const QVariant &value)
{
    if (qEnvironmentVariableIsSet("SNP_DEBUG") && name != QLatin1String("time-pos"))
        qInfo() << "[bridge] mpv" << name << value;
    if (name == QLatin1String("time-pos")) {
        if (!value.isValid())
            return;
        m_pendingState.insert(QStringLiteral("position"), value.toDouble());
        if (m_positionTimer.elapsed() >= kPositionIntervalMs)
            flushState();
        return;
    }
    if (name == QLatin1String("duration")) {
        if (value.isValid())
            m_pendingState.insert(QStringLiteral("duration"), value.toDouble());
    } else if (name == QLatin1String("pause")) {
        m_pendingState.insert(QStringLiteral("paused"), value.toBool());
    } else if (name == QLatin1String("paused-for-cache") || name == QLatin1String("seeking")) {
        m_pendingState.insert(QStringLiteral("buffering"), value.toBool());
    } else if (name == QLatin1String("volume")) {
        m_pendingState.insert(QStringLiteral("volume"), value.toDouble());
    } else if (name == QLatin1String("mute")) {
        m_pendingState.insert(QStringLiteral("muted"), value.toBool());
    } else if (name == QLatin1String("speed")) {
        m_pendingState.insert(QStringLiteral("speed"), value.toDouble());
    } else if (name == QLatin1String("eof-reached")) {
        if (value.toBool() && !m_eofSent) {
            m_eofSent = true;
            flushState();
            emit endReached();
        } else if (!value.toBool()) {
            m_eofSent = false;
        }
        return;
    } else if (name == QLatin1String("track-list")) {
        QVariantList tracks;
        for (const QVariant &t : value.toList()) {
            const QVariantMap m = t.toMap();
            tracks << QVariantMap{
                {QStringLiteral("id"), m.value(QStringLiteral("id"))},
                {QStringLiteral("type"), m.value(QStringLiteral("type"))},
                {QStringLiteral("title"), m.value(QStringLiteral("title"))},
                {QStringLiteral("lang"), m.value(QStringLiteral("lang"))},
                {QStringLiteral("codec"), m.value(QStringLiteral("codec"))},
                {QStringLiteral("selected"), m.value(QStringLiteral("selected"))},
                {QStringLiteral("external"), m.value(QStringLiteral("external"))},
            };
        }
        emit tracksChanged(tracks);
        return;
    } else {
        return;
    }
    flushState();
}

void Bridge::flushState()
{
    if (m_pendingState.isEmpty())
        return;
    m_positionTimer.restart();
    const QVariantMap state = m_pendingState;
    m_pendingState.clear();
    if (qEnvironmentVariableIsSet("SNP_DEBUG"))
        qInfo() << "[bridge] state" << state;
    emit stateChanged(state);
}
