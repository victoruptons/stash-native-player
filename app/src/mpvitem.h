#pragma once

#include <QQuickItem>
#include <QVariant>

#include <memory>

struct MpvCore;
class MpvRenderer;

// Plays video with libmpv and draws it *underneath* the Qt Quick scene.
//
// mpv renders into an offscreen framebuffer on the render thread
// (QQuickWindow::beforeRendering); at the start of the window's render pass
// (beforeRenderPassRecording) the frame is blitted into the window at this
// item's position. Everything else - in particular the transparent web view -
// is then drawn on top. This is Qt's supported "OpenGL under QML" approach and
// avoids sharing mpv's output as a texture with the scene graph.
//
// The item itself paints nothing; it only defines where the video goes.
class MpvItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString loadError READ loadError CONSTANT)

public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    bool isReady() const;
    QString loadError() const { return m_loadError; }

    void command(const QStringList &args);
    void setPropertyValue(const QString &name, const QVariant &value);
    void observe(const QString &name);

signals:
    void propertyChanged(const QString &name, const QVariant &value);
    void fileLoaded();
    // reason is one of "eof", "stop", "quit", "error", "redirect"
    void endFile(const QString &reason, const QString &error);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private slots:
    void processEvents();
    void requestFrame();

private:
    void attachToWindow(QQuickWindow *window);
    void detachFromWindow();

    std::shared_ptr<MpvCore> m_core;
    std::shared_ptr<MpvRenderer> m_renderer;
    QQuickWindow *m_window = nullptr;
    QList<QMetaObject::Connection> m_connections;
    QString m_loadError;
};
