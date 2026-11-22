#pragma once

#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QUrl>

#include "bridge.h"
#include "livesubs.h"

// QML-facing application state: server address and wiring of the player,
// web profile and bridge. Not exposed to web pages.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl serverUrl READ serverUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(bool hasServer READ hasServer NOTIFY serverUrlChanged)
    Q_PROPERTY(Bridge *bridge READ bridge CONSTANT)
    Q_PROPERTY(LiveSubs *liveSubs READ liveSubs CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString injectedScript READ injectedScript CONSTANT)
    Q_PROPERTY(qreal zoomFactor READ zoomFactor WRITE setZoomFactor NOTIFY zoomFactorChanged)
    Q_PROPERTY(QStringList recentServers READ recentServers NOTIFY serverUrlChanged)

public:
    explicit AppController(QObject *parent = nullptr);

    QUrl serverUrl() const { return m_serverUrl; }
    bool hasServer() const { return m_serverUrl.isValid() && !m_serverUrl.isEmpty(); }
    Bridge *bridge() const { return m_bridge; }
    LiveSubs *liveSubs() const { return m_liveSubs; }
    QString version() const;
    QString injectedScript() const;
    qreal zoomFactor() const { return m_zoom; }
    void setZoomFactor(qreal zoom);
    // Previously used servers, most recent first (for quick switching).
    QStringList recentServers() const { return m_recent; }

    // Accepts "host:port", "http://host:port/..." etc. Returns an empty URL
    // when the input can't be used.
    Q_INVOKABLE QUrl normalizeServerUrl(const QString &input) const;
    Q_INVOKABLE bool setServer(const QString &input);
    Q_INVOKABLE void forgetServer(const QString &url);
    Q_INVOKABLE void attach(QObject *webProfile, QObject *player);
    Q_INVOKABLE bool isOnServer(const QUrl &url) const;

signals:
    void serverUrlChanged();
    void zoomFactorChanged();

private:
    QSettings m_settings;
    QUrl m_serverUrl;
    qreal m_zoom = 1.0;
    QStringList m_recent;
    bool m_temporaryServer = false; // set by --server, not saved
    Bridge *m_bridge;
    LiveSubs *m_liveSubs;
};
