#include "mpvitem.h"

#include "mpvlib.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QMetaObject>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QStandardPaths>

#include <list>
#include <vector>

// GL 2.1/3.0 enums; normally provided by Qt's GL headers, defined here in case
// the platform's gl.h is older (e.g. Windows' GL 1.1 header).
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#endif
#ifndef GL_UNPACK_SKIP_PIXELS
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif

// Owns the mpv handle. Shared by the GUI-thread item and the render-thread
// renderer; mpv is destroyed only after both are gone (its render context has
// to be freed first, on the render thread).
struct MpvCore {
    const Mpv::Api *api = nullptr;
    mpv_handle *handle = nullptr;
    bool renderContextLeaked = false;

    QMutex mutex;
    QPointer<MpvItem> item; // cleared when the item is destroyed

    ~MpvCore()
    {
        if (handle) {
            api->mpv_set_wakeup_callback(handle, nullptr, nullptr);
            // A render context that could not be freed (no GL context at
            // shutdown) must not outlive the core; skip the teardown then.
            if (!renderContextLeaked)
                api->mpv_terminate_destroy(handle);
        }
    }

    void post(const char *method)
    {
        QMutexLocker lock(&mutex);
        if (item)
            QMetaObject::invokeMethod(item.data(), method, Qt::QueuedConnection);
    }
};

namespace {

// --- mpv_node <-> QVariant ----------------------------------------------------

QVariant nodeToVariant(const mpv_node *node)
{
    switch (node->format) {
    case MPV_FORMAT_STRING:
        return QString::fromUtf8(node->u.string);
    case MPV_FORMAT_FLAG:
        return bool(node->u.flag);
    case MPV_FORMAT_INT64:
        return qlonglong(node->u.int64);
    case MPV_FORMAT_DOUBLE:
        return node->u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        QVariantList list;
        for (int i = 0; i < node->u.list->num; ++i)
            list << nodeToVariant(&node->u.list->values[i]);
        return list;
    }
    case MPV_FORMAT_NODE_MAP: {
        QVariantMap map;
        for (int i = 0; i < node->u.list->num; ++i)
            map.insert(QString::fromUtf8(node->u.list->keys[i]), nodeToVariant(&node->u.list->values[i]));
        return map;
    }
    default:
        return {};
    }
}

// Builds an mpv_node tree; storage lives as long as the builder.
class NodeBuilder
{
public:
    explicit NodeBuilder(const QVariant &v) { fill(&m_root, v); }
    mpv_node *node() { return &m_root; }

private:
    void fill(mpv_node *dst, const QVariant &v)
    {
        switch (v.typeId()) {
        case QMetaType::Bool:
            dst->format = MPV_FORMAT_FLAG;
            dst->u.flag = v.toBool() ? 1 : 0;
            break;
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::UInt:
        case QMetaType::ULongLong:
            dst->format = MPV_FORMAT_INT64;
            dst->u.int64 = v.toLongLong();
            break;
        case QMetaType::Double:
        case QMetaType::Float:
            dst->format = MPV_FORMAT_DOUBLE;
            dst->u.double_ = v.toDouble();
            break;
        case QMetaType::QVariantList:
        case QMetaType::QStringList: {
            const QVariantList list = v.toList();
            auto &values = m_nodes.emplace_back(list.size());
            m_lists.emplace_back();
            mpv_node_list &nl = m_lists.back();
            nl.num = int(list.size());
            nl.values = values.data();
            nl.keys = nullptr;
            for (int i = 0; i < list.size(); ++i)
                fill(&values[i], list[i]);
            dst->format = MPV_FORMAT_NODE_ARRAY;
            dst->u.list = &nl;
            break;
        }
        default:
            m_strings.push_back(v.toString().toUtf8());
            dst->format = MPV_FORMAT_STRING;
            dst->u.string = m_strings.back().data();
            break;
        }
    }

    mpv_node m_root{};
    std::list<QByteArray> m_strings;
    std::list<std::vector<mpv_node>> m_nodes;
    std::list<mpv_node_list> m_lists;
};

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(QByteArray(name))) : nullptr;
}

void onMpvWakeup(void *ctx)
{
    static_cast<MpvCore *>(ctx)->post("processEvents");
}

void onMpvRenderUpdate(void *ctx)
{
    static_cast<MpvCore *>(ctx)->post("requestFrame");
}

} // namespace

// --- Renderer (render thread) -------------------------------------------------

class MpvRenderer
{
public:
    explicit MpvRenderer(std::shared_ptr<MpvCore> core)
        : m_core(std::move(core))
    {
    }

    ~MpvRenderer()
    {
        if (m_ctx) {
            if (QOpenGLContext::currentContext()) {
                cleanup();
            } else {
                // Can't free GL resources without a context (app shutdown).
                m_core->renderContextLeaked = true;
            }
        }
    }

    // Written during the scene graph sync (GUI thread blocked).
    QRect target;      // device pixels, top-left origin, relative to the window
    QSize windowSize;  // device pixels
    bool visible = false;

    // QQuickWindow::beforeRendering: render the current mpv frame offscreen.
    void render(QQuickWindow *window)
    {
        if (!m_core->handle || !QOpenGLContext::currentContext())
            return;
        if (!m_ctx && !createContext())
            return;
        if (!visible || target.isEmpty()) {
            // Still let mpv know frames are being consumed.
            m_core->api->mpv_render_context_update(m_ctx);
            return;
        }

        window->beginExternalCommands();
        if (!m_fbo || m_fbo->size() != target.size()) {
            delete m_fbo;
            m_fbo = new QOpenGLFramebufferObject(target.size());
        }
        m_core->api->mpv_render_context_update(m_ctx);

        auto *gl = QOpenGLContext::currentContext()->extraFunctions();
        // Qt Quick and QtWebEngine share this context and may leave pixel
        // transfer state behind; mpv uploads frames with glTexSubImage2D.
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        // mpv assumes default GL state. Qt Quick leaves depth/stencil/blend
        // state, write masks, a program, VAO and sampler objects bound; with
        // those, mpv's float (rgba16f) intermediate passes come out corrupted
        // for two-plane formats (NV12/P010, i.e. all hardware decoding).
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glDisable(GL_DEPTH_TEST);
        gl->glDisable(GL_STENCIL_TEST);
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_CULL_FACE);
        gl->glDisable(GL_DITHER);
        gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl->glDepthMask(GL_TRUE);
        gl->glStencilMask(0xFF);
        gl->glUseProgram(0);
        gl->glBindVertexArray(0);
        gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        GLint units = 0;
        gl->glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
        for (GLint i = 0; i < qMin(units, 32); ++i)
            gl->glBindSampler(GLuint(i), 0);
        gl->glActiveTexture(GL_TEXTURE0);

        mpv_opengl_fbo mfbo{int(m_fbo->handle()), m_fbo->width(), m_fbo->height(), 0};
        // The frame is blitted into the window's framebuffer (bottom-left
        // origin), so let mpv render it flipped.
        int flipY = 1;
        int block = 0;
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_OPENGL_FBO, &mfbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        m_core->api->mpv_render_context_render(m_ctx, params);
        window->endExternalCommands();
    }

    // QQuickWindow::beforeRenderPassRecording: copy the frame into the window
    // (the pass has just been cleared; QML content is drawn after this).
    void blit(QQuickWindow *window)
    {
        if (!m_fbo || !visible || target.isEmpty())
            return;
        window->beginExternalCommands();
        auto *gl = QOpenGLContext::currentContext()->extraFunctions();
        GLint drawFbo = 0;
        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo->handle());
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
        gl->glDisable(GL_SCISSOR_TEST);
        // GL's origin is bottom-left.
        const int x0 = target.x();
        const int y0 = windowSize.height() - (target.y() + target.height());
        gl->glBlitFramebuffer(0, 0, m_fbo->width(), m_fbo->height(),
                              x0, y0, x0 + target.width(), y0 + target.height(),
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(drawFbo));
        window->endExternalCommands();
    }

    // Render thread, GL context current.
    void cleanup()
    {
        if (m_ctx) {
            m_core->api->mpv_render_context_free(m_ctx);
            m_ctx = nullptr;
        }
        delete m_fbo;
        m_fbo = nullptr;
    }

private:
    bool createContext()
    {
        mpv_opengl_init_params glInit{&getProcAddress, nullptr};
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        int err = m_core->api->mpv_render_context_create(&m_ctx, m_core->handle, params);
        if (err < 0) {
            qWarning() << "mpv_render_context_create failed:" << m_core->api->mpv_error_string(err);
            m_ctx = nullptr;
            return false;
        }
        m_core->api->mpv_render_context_set_update_callback(m_ctx, &onMpvRenderUpdate, m_core.get());
        return true;
    }

    std::shared_ptr<MpvCore> m_core;
    mpv_render_context *m_ctx = nullptr;
    QOpenGLFramebufferObject *m_fbo = nullptr;
};

namespace {
// Frees the renderer's GL resources on the render thread.
class CleanupJob : public QRunnable
{
public:
    explicit CleanupJob(std::shared_ptr<MpvRenderer> r) : m_renderer(std::move(r)) {}
    void run() override { m_renderer->cleanup(); }

private:
    std::shared_ptr<MpvRenderer> m_renderer;
};
} // namespace

// --- MpvItem -----------------------------------------------------------------

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickItem(parent)
    , m_core(std::make_shared<MpvCore>())
{
    setFlag(ItemHasContents, true);
    m_renderer = std::make_shared<MpvRenderer>(m_core);
    const Mpv::Api *api = Mpv::api(&m_loadError);
    if (!api) {
        qWarning().noquote() << m_loadError;
        return;
    }
    m_core->api = api;
    m_core->item = this;

    mpv_handle *h = api->mpv_create();
    if (!h) {
        m_loadError = QStringLiteral("mpv_create() failed");
        return;
    }

    auto opt = [&](const char *name, const QString &value) {
        int err = api->mpv_set_option_string(h, name, value.toUtf8().constData());
        if (err < 0)
            qWarning() << "mpv option" << name << "=" << value << "failed:" << api->mpv_error_string(err);
    };

    // Users can drop an mpv.conf / input.conf into <app config dir>/mpv to
    // tune playback (e.g. hwdec, scaling, audio device).
    const QString configDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(QStringLiteral("mpv"));
    QDir().mkpath(configDir);

    opt("vo", QStringLiteral("libmpv"));
    opt("config-dir", configDir);
    opt("config", QStringLiteral("yes"));
    opt("hwdec", QStringLiteral("auto-safe"));
    opt("keep-open", QStringLiteral("yes"));
    opt("idle", QStringLiteral("yes"));
    opt("osc", QStringLiteral("no"));
    opt("input-default-bindings", QStringLiteral("no"));
    opt("input-vo-keyboard", QStringLiteral("no"));
    opt("ytdl", QStringLiteral("no"));
    opt("terminal", QStringLiteral("no"));
    opt("cache", QStringLiteral("yes"));
    opt("demuxer-max-bytes", QStringLiteral("400MiB"));
    opt("demuxer-max-back-bytes", QStringLiteral("100MiB"));
    opt("audio-client-name", QStringLiteral("Stash Native Player"));
    opt("sub-auto", QStringLiteral("no"));
    opt("load-scripts", QStringLiteral("no"));

    if (api->mpv_initialize(h) < 0) {
        m_loadError = QStringLiteral("mpv_initialize() failed");
        api->mpv_terminate_destroy(h);
        return;
    }
    // SNP_MPV_LOG=v (or debug) raises mpv's log level for troubleshooting.
    const QByteArray logLevel = qEnvironmentVariableIsSet("SNP_MPV_LOG") ? qgetenv("SNP_MPV_LOG") : QByteArray("warn");
    api->mpv_request_log_messages(h, logLevel.constData());
    m_core->handle = h;
    api->mpv_set_wakeup_callback(h, &onMpvWakeup, m_core.get());
}

MpvItem::~MpvItem()
{
    {
        QMutexLocker lock(&m_core->mutex);
        m_core->item = nullptr;
    }
    detachFromWindow();
}

void MpvItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange) {
        detachFromWindow();
        if (value.window)
            attachToWindow(value.window);
    } else if (change == ItemVisibleHasChanged) {
        update();
    }
    QQuickItem::itemChange(change, value);
}

void MpvItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    update();
}

void MpvItem::attachToWindow(QQuickWindow *window)
{
    m_window = window;
    std::shared_ptr<MpvRenderer> r = m_renderer;
    m_connections << connect(window, &QQuickWindow::beforeRendering, this,
                             [r, window]() { r->render(window); }, Qt::DirectConnection);
    m_connections << connect(window, &QQuickWindow::beforeRenderPassRecording, this,
                             [r, window]() { r->blit(window); }, Qt::DirectConnection);
    m_connections << connect(window, &QQuickWindow::sceneGraphInvalidated, this,
                             [r]() { r->cleanup(); }, Qt::DirectConnection);
    m_connections << connect(window, &QQuickWindow::widthChanged, this, &QQuickItem::update);
    m_connections << connect(window, &QQuickWindow::heightChanged, this, &QQuickItem::update);
    update();
}

void MpvItem::detachFromWindow()
{
    for (const auto &c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    if (m_window) {
        m_window->scheduleRenderJob(new CleanupJob(m_renderer), QQuickWindow::NoStage);
        m_window->update();
    }
    m_window = nullptr;
}

QSGNode *MpvItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // Runs on the render thread while the GUI thread is blocked: hand the
    // current geometry to the renderer. Nothing is drawn by the item itself.
    delete oldNode;
    if (QQuickWindow *w = window()) {
        const qreal dpr = w->effectiveDevicePixelRatio();
        const QRectF r = mapRectToScene(boundingRect());
        m_renderer->target = QRect(qRound(r.x() * dpr), qRound(r.y() * dpr), qRound(r.width() * dpr),
                                   qRound(r.height() * dpr));
        m_renderer->windowSize = QSize(qRound(w->width() * dpr), qRound(w->height() * dpr));
        m_renderer->visible = isVisible() && opacity() > 0 && r.width() >= 2 && r.height() >= 2;
    }
    return nullptr;
}

void MpvItem::requestFrame()
{
    update();
    if (m_window)
        m_window->update();
}

bool MpvItem::isReady() const
{
    return m_core->handle != nullptr;
}

void MpvItem::command(const QStringList &args)
{
    if (!isReady())
        return;
    std::vector<QByteArray> storage;
    storage.reserve(args.size());
    std::vector<const char *> argv;
    for (const QString &a : args) {
        storage.push_back(a.toUtf8());
        argv.push_back(storage.back().constData());
    }
    argv.push_back(nullptr);
    int err = m_core->api->mpv_command(m_core->handle, argv.data());
    if (err < 0)
        qWarning() << "mpv command" << args.value(0) << "failed:" << m_core->api->mpv_error_string(err);
}

void MpvItem::setPropertyValue(const QString &name, const QVariant &value)
{
    if (!isReady())
        return;
    NodeBuilder b(value);
    int err = m_core->api->mpv_set_property(m_core->handle, name.toUtf8().constData(), MPV_FORMAT_NODE, b.node());
    if (err < 0)
        qWarning() << "mpv set" << name << "=" << value << "failed:" << m_core->api->mpv_error_string(err);
}

void MpvItem::observe(const QString &name)
{
    if (!isReady())
        return;
    m_core->api->mpv_observe_property(m_core->handle, 0, name.toUtf8().constData(), MPV_FORMAT_NODE);
}

void MpvItem::processEvents()
{
    if (!isReady())
        return;
    const Mpv::Api *api = m_core->api;
    while (true) {
        mpv_event *ev = api->mpv_wait_event(m_core->handle, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(ev->data);
            QVariant value;
            if (prop->format == MPV_FORMAT_NODE)
                value = nodeToVariant(static_cast<mpv_node *>(prop->data));
            emit propertyChanged(QString::fromUtf8(prop->name), value);
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            emit fileLoaded();
            break;
        case MPV_EVENT_END_FILE: {
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            QString reason;
            switch (ef->reason) {
            case MPV_END_FILE_REASON_EOF: reason = QStringLiteral("eof"); break;
            case MPV_END_FILE_REASON_STOP: reason = QStringLiteral("stop"); break;
            case MPV_END_FILE_REASON_QUIT: reason = QStringLiteral("quit"); break;
            case MPV_END_FILE_REASON_ERROR: reason = QStringLiteral("error"); break;
            case MPV_END_FILE_REASON_REDIRECT: reason = QStringLiteral("redirect"); break;
            }
            QString error;
            if (ef->reason == MPV_END_FILE_REASON_ERROR)
                error = QString::fromUtf8(api->mpv_error_string(ef->error));
            emit endFile(reason, error);
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *msg = static_cast<mpv_event_log_message *>(ev->data);
            qInfo().noquote() << "[mpv]" << msg->prefix << QString::fromUtf8(msg->text).trimmed();
            break;
        }
        default:
            break;
        }
    }
}
