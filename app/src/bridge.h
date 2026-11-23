#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QUrl>
#include <QVariantMap>

class LiveSubs;
class MpvItem;
class QWebEngineCookieStore;

// The object the Stash page talks to (registered on the web channel as
// "stashNative"). Every public slot here is callable from page JavaScript, so
// inputs are validated: only http(s) URLs on the configured Stash server are
// ever handed to mpv.
class Bridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int apiVersion READ apiVersion CONSTANT)
    Q_PROPERTY(QRectF videoRect READ videoRect NOTIFY videoGeometryChanged)
    Q_PROPERTY(bool videoVisible READ videoVisible NOTIFY videoGeometryChanged)

public:
    explicit Bridge(QObject *parent = nullptr);

    int apiVersion() const { return 1; }
    QRectF videoRect() const { return m_rect; }
    bool videoVisible() const { return m_visible; }

    // Not callable from the page (not slots / invokables).
    void setPlayer(MpvItem *player);
    void setCookieStore(QWebEngineCookieStore *store);
    void setServerUrl(const QUrl &url) { m_server = url; }
    // Told about every file started/stopped (gets the cookie header, which must
    // never go to the page, so this is a direct call and not a signal).
    void setLiveSubs(LiveSubs *subs) { m_liveSubs = subs; }
    // "name=value; ..." of the page's cookies for a URL on the Stash server.
    QString cookieHeaderFor(const QUrl &url) const;

public slots:
    void load(const QString &url, double startSeconds, bool paused);
    void stop();
    void setPaused(bool paused);
    void seek(double seconds);
    // Exact seek by an offset in seconds (mouse wheel over the video).
    void seekRelative(double seconds);
    void setVolume(double volume);
    void setMuted(bool muted);
    void setSpeed(double speed);
    // type: "aid" (audio) or "sid" (subtitles); id <= 0 disables the track
    void setTrack(const QString &type, int id);
    void addSubtitle(const QString &url, const QString &title, const QString &lang, bool select);
    // Position of the video box in CSS pixels, relative to the web view.
    void setVideoGeometry(double x, double y, double width, double height, bool visible);

signals:
    void stateChanged(const QVariantMap &state);
    void tracksChanged(const QVariantList &tracks);
    void endReached();
    void fileLoaded();
    void errorOccurred(const QString &message);
    void videoGeometryChanged();

private:
    void onPropertyChanged(const QString &name, const QVariant &value);
    void onFileLoaded();
    void onEndFile(const QString &reason, const QString &error);
    void flushState();
    bool isAllowedUrl(const QUrl &url) const;

    QPointer<MpvItem> m_player;
    QPointer<LiveSubs> m_liveSubs;
    QUrl m_server;
    QRectF m_rect;
    bool m_visible = false;

    QVariantMap m_pendingState;
    QElapsedTimer m_positionTimer;
    bool m_loaded = false;
    bool m_eofSent = false;
    // Last pause state the page asked for; re-applied once a file has loaded
    // (see onFileLoaded).
    bool m_wantPaused = true;
    QList<QVariantList> m_pendingSubs; // [url, title, lang, select]

    struct Cookie {
        QString domain;
        QString path;
        QByteArray value;
    };
    QHash<QByteArray, Cookie> m_cookies; // key: name + domain + path
};
