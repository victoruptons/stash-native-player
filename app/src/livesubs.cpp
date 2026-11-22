#include "livesubs.h"

#include "mpvitem.h"

#include <QDir>
#include <QFile>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using subs::debugLog;
using subs::kWindow;

namespace {

constexpr double kLookahead = 60.0; // keep transcribing this far ahead of playback

const char *kOverlayId = "61"; // arbitrary osd-overlay id

QString firstFile(const QString &dir, const QStringList &patterns, const QString &exclude = {})
{
    const QFileInfoList files = QDir(dir).entryInfoList(patterns, QDir::Files, QDir::Name);
    for (const QFileInfo &f : files)
        if (exclude.isEmpty() || !f.fileName().contains(exclude, Qt::CaseInsensitive))
            return f.absoluteFilePath();
    return {};
}

int freePort()
{
    QTcpServer s;
    if (!s.listen(QHostAddress::LocalHost, 0))
        return 0;
    return s.serverPort();
}

// A helper tool: <aiDir>/<folder>/<name>[.exe] if bundled there, else from PATH
// (e.g. Homebrew's ffmpeg / whisper-cpp / llama.cpp on macOS).
QString toolPath(const QString &aiDir, const QString &folder, const QString &name)
{
#ifdef Q_OS_WIN
    const QString bundled = aiDir + QLatin1Char('/') + folder + QLatin1Char('/') + name + QStringLiteral(".exe");
#else
    const QString bundled = aiDir + QLatin1Char('/') + folder + QLatin1Char('/') + name;
#endif
    return QFileInfo::exists(bundled) ? bundled : QStandardPaths::findExecutable(name);
}

void hideConsole(QProcess *p)
{
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#else
    Q_UNUSED(p);
#endif
}

QString assEscape(QString s)
{
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    s.replace(QLatin1Char('{'), QLatin1Char('('));
    s.replace(QLatin1Char('}'), QLatin1Char(')'));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s;
}

} // namespace

LiveSubs::LiveSubs(QObject *parent)
    : QObject(parent)
{
    m_aiDir = qEnvironmentVariable("SNP_AI_DIR");
    if (m_aiDir.isEmpty())
        m_aiDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/ai");
    m_aiDir = QDir::cleanPath(m_aiDir);
    QSettings settings;
    m_mode = qBound(0, settings.value(QStringLiteral("liveSubs/mode"), 0).toInt(), 3);
    m_genPaused = settings.value(QStringLiteral("liveSubs/genPaused"), false).toBool();
    m_skipLangs = settings.value(QStringLiteral("liveSubs/skipLanguages")).toStringList();
    m_embeddedSkip = qBound(0, settings.value(QStringLiteral("liveSubs/embeddedSkip"), 2).toInt(), 2);
    m_hardsubSkip = qBound(0, settings.value(QStringLiteral("liveSubs/hardsubSkip"), 1).toInt(), 2);
    m_modelsDir = QDir::cleanPath(settings.value(QStringLiteral("liveSubs/modelsDir"),
                                                 m_aiDir + QStringLiteral("/models")).toString());
    m_whisperSetting = settings.value(QStringLiteral("liveSubs/whisperModel")).toString();
    m_liveLlmSetting = settings.value(QStringLiteral("liveSubs/liveLlm")).toString();
    m_genLlmSetting = settings.value(QStringLiteral("liveSubs/genLlm")).toString();
    refreshModels();

    // Scenes tagged for subtitles are picked up every few minutes.
    m_queueTimer.setInterval(5 * 60 * 1000);
    connect(&m_queueTimer, &QTimer::timeout, this, &LiveSubs::checkQueue);

#ifdef Q_OS_WIN
    // Put the servers in a job that is closed (killing them) when the app exits,
    // even if it crashes.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        m_jobObject = job;
    }
#endif
}

LiveSubs::~LiveSubs()
{
    if (m_extract) {
        m_extract->kill();
        m_extract->waitForFinished(2000);
    }
    if (!m_job.wavPath.isEmpty())
        QFile::remove(m_job.wavPath);
    stopServers();
#ifdef Q_OS_WIN
    if (m_jobObject)
        CloseHandle(static_cast<HANDLE>(m_jobObject));
#endif
}

void LiveSubs::setPlayer(MpvItem *player)
{
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (player)
        connect(player, &MpvItem::propertyChanged, this, &LiveSubs::onPropertyChanged);
}

void LiveSubs::setServer(const QUrl &server)
{
    if (server == m_server)
        return;
    m_server = server;
    m_queueTagId.clear();
    m_failedThisRun.clear();
    if (m_server.isEmpty()) {
        m_queueTimer.stop();
        return;
    }
    m_queueTimer.start();
    QTimer::singleShot(8000, this, &LiveSubs::checkQueue);
}

void LiveSubs::setMode(int mode)
{
    mode = qBound(0, mode, 3);
    if (mode == m_mode)
        return;
    const bool wasOn = m_mode != 0;
    m_mode = mode;
    QSettings().setValue(QStringLiteral("liveSubs/mode"), mode);
    emit modeChanged();

    if (mode == 0) {
        ++m_gen;
        m_busy = false;
        m_translating = false;
        m_waitingForStash = false;
        updateOverlay(true);
        stopServersIfIdle(); // frees the GPU memory
        setStatus(QString());
        genResume();
        return;
    }
    if (!wasOn && m_url.isValid())
        startLive();
    pumpTranslation(); // (also lets a waiting whole-video job go on if playback no longer translates)
    updateOverlay(true);
}

void LiveSubs::setStatus(const QString &s)
{
    if (s == m_status)
        return;
    m_status = s;
    emit statusChanged();
}

void LiveSubs::setGenStatus(const QString &s)
{
    if (s == m_genStatus)
        return;
    m_genStatus = s;
    emit genStatusChanged();
}

// --- servers -------------------------------------------------------------------

bool LiveSubs::serversWanted() const
{
    return m_mode != 0 || (genActive() && !m_genPaused);
}

void LiveSubs::stopServersIfIdle()
{
    if (!serversWanted())
        stopServers();
}

QString LiveSubs::ffmpegPath() const
{
    return toolPath(m_aiDir, QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg"));
}

// --- models ------------------------------------------------------------------------

void LiveSubs::refreshModels()
{
    const QDir dir(m_modelsDir);
    QStringList whisper, llm;
    for (const QString &f : dir.entryList({QStringLiteral("ggml-*.bin")}, QDir::Files, QDir::Name))
        if (!f.contains(QLatin1String("silero"), Qt::CaseInsensitive))
            whisper << f;
    llm = dir.entryList({QStringLiteral("*.gguf")}, QDir::Files, QDir::Name);
    // skip the extra parts of split models ("...-00002-of-00003.gguf") and mmproj files
    static const QRegularExpression extraPart(QStringLiteral("-0000[2-9]-of-|mmproj"),
                                              QRegularExpression::CaseInsensitiveOption);
    llm.erase(std::remove_if(llm.begin(), llm.end(), [](const QString &f) { return extraPart.match(f).hasMatch(); }),
              llm.end());
    m_whisperModels = whisper;
    m_llmModels = llm;
    emit modelsChanged();
}

QString LiveSubs::modelSize(const QString &name) const
{
    const QFileInfo f(QDir(m_modelsDir).filePath(name));
    return f.exists() ? QStringLiteral("%1 GB").arg(f.size() / 1e9, 0, 'f', 1) : QString();
}

void LiveSubs::setModelsDir(const QString &dir)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(dir));
    if (clean == m_modelsDir || clean.isEmpty())
        return;
    m_modelsDir = clean;
    QSettings().setValue(QStringLiteral("liveSubs/modelsDir"), clean);
    refreshModels();
    // A running speech server switches if its model is not in the new folder;
    // the translation server switches on its next request.
    if (m_whisper && whisperPath() != m_whisperModelPath)
        setWhisperModel(m_whisperSetting);
}

QString LiveSubs::whisperPath() const
{
    const QDir dir(m_modelsDir);
    if (!m_whisperSetting.isEmpty() && m_whisperModels.contains(m_whisperSetting))
        return dir.filePath(m_whisperSetting);
    return m_whisperModels.isEmpty() ? QString() : dir.filePath(m_whisperModels.first());
}

QString LiveSubs::whisperModel() const
{
    return QFileInfo(whisperPath()).fileName();
}

void LiveSubs::setWhisperModel(const QString &name)
{
    m_whisperSetting = name;
    QSettings().setValue(QStringLiteral("liveSubs/whisperModel"), name);
    emit modelsChanged();
    if (m_whisper && whisperPath() != m_whisperModelPath) {
        // restart with the new model
        QProcess *p = m_whisper;
        m_whisper = nullptr;
        m_whisperReady = false;
        p->kill();
        p->waitForFinished(2000);
        p->deleteLater();
        if (m_busy) {
            ++m_gen;
            m_busy = false;
        }
        if (serversWanted() && ensureServers())
            pump();
    }
}

QString LiveSubs::llmName(bool live) const
{
    const QString &s = live ? m_liveLlmSetting : m_genLlmSetting;
    if (s == QLatin1String("none"))
        return s;
    if (!s.isEmpty() && m_llmModels.contains(s))
        return s;
    return m_llmModels.isEmpty() ? QStringLiteral("none") : m_llmModels.first();
}

void LiveSubs::setLlm(bool live, const QString &name)
{
    (live ? m_liveLlmSetting : m_genLlmSetting) = name;
    QSettings().setValue(live ? QStringLiteral("liveSubs/liveLlm") : QStringLiteral("liveSubs/genLlm"), name);
    emit modelsChanged();
    // The next request loads it (with "none", playback just stops translating).
    if (live) {
        if (liveNeedsLlama())
            pumpTranslation();
        else
            genResume();
        updateOverlay(true);
    } else if (m_job.phase == Job::Translate) {
        genTranslateNext();
    }
}

QString LiveSubs::llmPath(bool live) const
{
    const QString name = llmName(live);
    return name == QLatin1String("none") || !llamaExeExists() ? QString() : QDir(m_modelsDir).filePath(name);
}

bool LiveSubs::llamaExeExists() const
{
    return !toolPath(m_aiDir, QStringLiteral("llama"), QStringLiteral("llama-server")).isEmpty();
}

// --- servers ------------------------------------------------------------------------

bool LiveSubs::ensureServers()
{
    if (!m_whisper && !startWhisper())
        return false;
    if (!m_whisperReady)
        setStatus(tr("Loading speech model…"));
    watchReady();
    return true;
}

void LiveSubs::watchReady()
{
    const bool waiting = (m_whisper && !m_whisperReady) || (m_llama && !m_llamaReady);
    if (waiting && !m_polling) {
        m_polling = true;
        QTimer::singleShot(400, this, &LiveSubs::pollReady);
    }
}

bool LiveSubs::startWhisper()
{
    const QString whisperExe = toolPath(m_aiDir, QStringLiteral("whisper"), QStringLiteral("whisper-server"));
    const QString whisperModel = whisperPath();
    if (whisperExe.isEmpty() || whisperModel.isEmpty()) {
        setStatus(tr("Subtitles need whisper-server.exe in %1 and a ggml-*.bin model in %2")
                      .arg(QDir::toNativeSeparators(m_aiDir + QStringLiteral("/whisper")),
                           QDir::toNativeSeparators(m_modelsDir)));
        return false;
    }
    m_whisperReady = false;
    m_whisperModelPath = whisperModel;
    m_whisperPort = freePort();
    QStringList wargs{QStringLiteral("-m"), whisperModel, QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
                      QStringLiteral("--port"), QString::number(m_whisperPort), QStringLiteral("-l"),
                      QStringLiteral("auto"),
                      // no text carried over between requests: with it, whisper
                      // tends to repeat the previous window's last lines
                      QStringLiteral("-mc"), QStringLiteral("0")};
    const QString vad = firstFile(m_modelsDir, {QStringLiteral("ggml-silero*.bin")});
    if (!vad.isEmpty())
        wargs << QStringLiteral("--vad") << QStringLiteral("-vm") << vad;
    debugLog(QStringLiteral("start whisper-server: ") + whisperModel);
    startServer(m_whisper, whisperExe, wargs, QStringLiteral("whisper-server"));
    return true;
}

void LiveSubs::startLlama(const QString &model)
{
    m_llamaReady = false;
    m_llamaModel = model;
    ++m_llamaRun;
    m_llamaPort = freePort();
    debugLog(QStringLiteral("start llama-server: ") + model);
    startServer(m_llama, toolPath(m_aiDir, QStringLiteral("llama"), QStringLiteral("llama-server")),
                {QStringLiteral("-m"), model, QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
                 QStringLiteral("--port"), QString::number(m_llamaPort), QStringLiteral("-ngl"),
                 QStringLiteral("99"), QStringLiteral("-c"), QStringLiteral("4096"), QStringLiteral("-np"),
                 QStringLiteral("1"), QStringLiteral("--no-webui")},
                QStringLiteral("llama-server"));
    emit modelsChanged();
}

void LiveSubs::stopLlama()
{
    QProcess *p = m_llama;
    m_llama = nullptr; // "stopped on purpose" for the finished handler
    m_llamaReady = false;
    m_llamaModel.clear();
    m_translating = false; // a request in flight fails; it is retried
    if (p) {
        p->kill();
        p->waitForFinished(3000); // free the GPU memory before the next model loads
        p->deleteLater();
    }
    emit modelsChanged();
}

bool LiveSubs::liveNeedsLlama() const
{
    if (!m_url.isValid() || !wantTranslation())
        return false;
    if (m_source.isEmpty() && !m_atEnd)
        return true; // more lines are coming
    return std::any_of(m_segments.begin(), m_segments.end(), [](const Cue &c) { return c.translation.isEmpty(); });
}

bool LiveSubs::useLlama(const QString &model, bool live)
{
    if (model.isEmpty() || m_llamaGaveUp)
        return false;
    if (m_llama && m_llamaModel == model) {
        watchReady();
        return m_llamaReady;
    }
    if (!live && m_llama && liveNeedsLlama())
        return false; // playback is using another model; wait for it
    stopLlama();
    startLlama(model);
    setStatus(tr("Loading translation model %1…").arg(QFileInfo(model).completeBaseName()));
    watchReady();
    return false;
}

// Whole-video translation was waiting for playback to stop using the server.
void LiveSubs::genResume()
{
    if (m_job.waiting && m_job.phase == Job::Translate && !liveNeedsLlama()) {
        m_job.waiting = false;
        genTranslateNext();
    }
}

// A server died on its own (a crash: whisper-server builds before b5907 crash
// on any 30 s of audio without speech). Restart it, so the subtitles don't
// stop for the rest of the video, unless it keeps dying.
void LiveSubs::onServerExited(QProcess *proc, const QString &name, int code)
{
    const bool whisper = proc == m_whisper;
    const QString llamaModel = m_llamaModel;
    debugLog(QStringLiteral("%1 exited, code %2").arg(name).arg(code));
    if (whisper) {
        m_whisper = nullptr;
        m_whisperReady = false;
    } else {
        m_llama = nullptr;
        m_llamaReady = false;
        m_llamaModel.clear();
        emit modelsChanged();
    }
    proc->deleteLater();
    if (!serversWanted())
        return;
    int &crashes = whisper ? m_whisperCrashes : m_llamaCrashes;
    if (++crashes > 3) {
        setStatus(tr("%1 keeps stopping (exit code %2)").arg(name).arg(code));
        if (!whisper)
            m_llamaGaveUp = true; // go on without translation
        return;
    }
    if (whisper && m_busy) {
        // The request in flight is lost; redo that window, or skip it if it
        // already crashed the server once.
        if (qAbs(m_crashWindow - m_cursor) < 0.01)
            m_cursor += kWindow - subs::kTail;
        m_crashWindow = m_cursor;
        ++m_gen;
        m_busy = false;
    }
    if (!whisper)
        m_translating = false;
    setStatus(tr("Restarting %1…").arg(name));
    QTimer::singleShot(1000, this, [this, whisper, llamaModel] {
        if (!serversWanted())
            return;
        if (whisper) {
            ensureServers();
        } else if (!m_llama) {
            startLlama(llamaModel);
            watchReady();
        }
    });
}

void LiveSubs::startServer(QProcess *&proc, const QString &exe, const QStringList &args, const QString &name)
{
    proc = new QProcess(this);
    hideConsole(proc);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(QFileInfo(exe).absolutePath());
    connect(proc, &QProcess::readyRead, proc, [proc, name] {
        const QByteArray out = proc->readAll();
        if (qEnvironmentVariableIsSet("SNP_DEBUG"))
            qInfo().noquote() << "[" + name + "]" << out.trimmed();
    });
    QProcess *self = proc;
    connect(proc, &QProcess::finished, this, [this, self, name](int code) {
        if (self != m_whisper && self != m_llama)
            return; // stopped on purpose
        onServerExited(self, name, code);
    });
    proc->start(exe, args);
#ifdef Q_OS_WIN
    if (m_jobObject && proc->waitForStarted(5000)) {
        if (HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(proc->processId()))) {
            AssignProcessToJobObject(static_cast<HANDLE>(m_jobObject), h);
            CloseHandle(h);
        }
    }
#endif
}

void LiveSubs::pollReady()
{
    m_polling = false;
    if (!m_whisper && !m_llama)
        return;
    auto check = [this](int port, const char *path, bool *flag) {
        QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(QLatin1String(path))));
        req.setTransferTimeout(2000);
        QNetworkReply *r = m_net.get(req);
        connect(r, &QNetworkReply::finished, this, [this, r, flag] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError || *flag)
                return;
            *flag = true;
            if (m_whisperReady || !m_whisper)
                setStatus(!m_llama || m_llamaReady ? QString() : m_status);
            pump();
            pumpTranslation();
            if (m_job.waiting) {
                m_job.waiting = false;
                if (m_job.phase == Job::Detect)
                    genDetect(m_job.detectAttempt);
                else if (m_job.phase == Job::Transcribe)
                    genTranscribeNext();
                else if (m_job.phase == Job::Translate)
                    genTranslateNext();
            }
        });
    };
    if (m_whisper && !m_whisperReady)
        check(m_whisperPort, "/", &m_whisperReady);
    if (m_llama && !m_llamaReady)
        check(m_llamaPort, "/health", &m_llamaReady);
    if ((m_whisper && !m_whisperReady) || (m_llama && !m_llamaReady)) {
        m_polling = true;
        QTimer::singleShot(500, this, &LiveSubs::pollReady);
    }
}

void LiveSubs::stopServers()
{
    for (QProcess **pp : {&m_whisper, &m_llama}) {
        QProcess *p = *pp;
        *pp = nullptr;
        if (!p)
            continue;
        p->kill();
        p->waitForFinished(2000);
        p->deleteLater();
    }
    m_whisperReady = m_llamaReady = false;
    m_whisperCrashes = m_llamaCrashes = 0;
    m_llamaGaveUp = false;
    m_llamaModel.clear();
    emit modelsChanged();
}

QNetworkReply *LiveSubs::postWhisper(const QByteArray &wav, const QString &language)
{
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto field = [multi](const char *name, const QByteArray &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"").arg(QLatin1String(name)));
        part.setBody(value);
        multi->append(part);
    };
    QHttpPart file;
    file.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"file\"; filename=\"chunk.wav\""));
    file.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
    file.setBody(wav);
    multi->append(file);
    field("response_format", "verbose_json");
    field("temperature", "0.0");
    field("language", language.isEmpty() ? QByteArray("auto") : language.toUtf8());

    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/inference").arg(m_whisperPort)));
    req.setTransferTimeout(60000);
    QNetworkReply *reply = m_net.post(req, multi);
    multi->setParent(reply);
    return reply;
}

QNetworkReply *LiveSubs::postTranslation(const QStringList &lines)
{
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(m_llamaPort)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(60000);
    return m_net.post(req, subs::translationRequest(lines));
}

// --- Stash ---------------------------------------------------------------------------

QUrl LiveSubs::stashUrl(const QString &path) const
{
    QString base = m_server.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    return QUrl(base + path);
}

QString LiveSubs::cookieFor(const QUrl &url) const
{
    return m_cookies ? m_cookies(url) : QString();
}

void LiveSubs::stashGql(const QString &query, const QJsonObject &variables, QObject *context,
                        std::function<void(const QJsonObject &, const QString &)> done)
{
    const QUrl url = stashUrl(QStringLiteral("graphql"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QString cookie = cookieFor(url);
    if (!cookie.isEmpty())
        req.setRawHeader("Cookie", cookie.toUtf8());
    req.setTransferTimeout(120000);
    const QJsonObject body{{QStringLiteral("query"), query}, {QStringLiteral("variables"), variables}};
    QNetworkReply *reply = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, context, [reply, done] {
        reply->deleteLater();
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        QStringList errors;
        for (const QJsonValue &e : root.value(QStringLiteral("errors")).toArray())
            errors << e.toObject().value(QStringLiteral("message")).toString();
        if (errors.isEmpty() && reply->error() != QNetworkReply::NoError)
            errors << reply->errorString();
        done(root.value(QStringLiteral("data")).toObject(), errors.join(QStringLiteral("; ")));
    });
}

void LiveSubs::stashGet(const QUrl &url, QObject *context, std::function<void(const QByteArray &, bool)> done)
{
    QNetworkRequest req(url);
    const QString cookie = cookieFor(url);
    if (!cookie.isEmpty())
        req.setRawHeader("Cookie", cookie.toUtf8());
    req.setTransferTimeout(60000);
    QNetworkReply *reply = m_net.get(req);
    connect(reply, &QNetworkReply::finished, context, [reply, done] {
        reply->deleteLater();
        done(reply->readAll(), reply->error() == QNetworkReply::NoError);
    });
}

void LiveSubs::findOrCreateTag(const QString &name, bool create, std::function<void(const QString &)> done)
{
    stashGql(QStringLiteral("query($n: String!) { findTags(tag_filter: {name: {value: $n, modifier: EQUALS}}) "
                            "{ tags { id } } }"),
             {{QStringLiteral("n"), name}}, this, [this, name, create, done](const QJsonObject &data, const QString &) {
                 const QJsonArray tags = data.value(QStringLiteral("findTags")).toObject().value(QStringLiteral("tags")).toArray();
                 if (!tags.isEmpty() || !create) {
                     done(tags.isEmpty() ? QString() : tags.first().toObject().value(QStringLiteral("id")).toString());
                     return;
                 }
                 stashGql(QStringLiteral("mutation($n: String!) { tagCreate(input: {name: $n}) { id } }"),
                          {{QStringLiteral("n"), name}}, this, [done](const QJsonObject &d, const QString &) {
                              done(d.value(QStringLiteral("tagCreate")).toObject().value(QStringLiteral("id")).toString());
                          });
             });
}

QString LiveSubs::sceneIdFromUrl(const QUrl &url)
{
    static const QRegularExpression re(QStringLiteral("/scene/(\\d+)/stream"));
    return re.match(url.path()).captured(1);
}

void LiveSubs::fetchCaptions(const QString &sceneId, const QJsonArray &captions, QObject *context,
                             std::function<void(const QList<Cue> &, const QString &, bool)> done)
{
    QJsonObject zh, orig;
    for (const QJsonValue &v : captions) {
        const QJsonObject c = v.toObject();
        const QString lang = c.value(QStringLiteral("language_code")).toString();
        if (subs::isChinese(lang)) {
            if (zh.isEmpty())
                zh = c;
        } else if (orig.isEmpty() || (c.value(QStringLiteral("caption_type")).toString() == QLatin1String("srt"))) {
            orig = c;
        }
    }
    auto url = [this, sceneId](const QJsonObject &c) {
        QUrl u = stashUrl(QStringLiteral("scene/%1/caption").arg(sceneId));
        u.setQuery(QStringLiteral("lang=%1&type=%2")
                       .arg(c.value(QStringLiteral("language_code")).toString(),
                            c.value(QStringLiteral("caption_type")).toString()));
        return u;
    };
    const QString origLang = orig.value(QStringLiteral("language_code")).toString();
    auto withChinese = [this, zh, url, context, done, origLang](QList<Cue> cues) {
        if (zh.isEmpty()) {
            done(cues, origLang, false);
            return;
        }
        stashGet(url(zh), context, [cues, done, origLang](const QByteArray &data, bool ok) mutable {
            const QList<Cue> zhCues = ok ? subs::parseCaptions(data) : QList<Cue>{};
            // Lines generated by this app have the same times in both files;
            // pair those. Anything else is shown on its own timing.
            for (const Cue &z : zhCues) {
                auto it = std::find_if(cues.begin(), cues.end(), [&z](const Cue &c) {
                    return c.translation.isEmpty() && qAbs(c.start - z.start) < 0.05;
                });
                if (it != cues.end())
                    it->translation = z.text;
                else
                    cues.append(Cue{z.start, z.end, z.text, z.text});
            }
            std::sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
            done(cues, origLang.isEmpty() ? QStringLiteral("zh") : origLang, !zhCues.isEmpty());
        });
    };
    if (orig.isEmpty()) {
        withChinese({});
        return;
    }
    stashGet(url(orig), context, [withChinese](const QByteArray &data, bool ok) {
        withChinese(ok ? subs::parseCaptions(data) : QList<Cue>{});
    });
}

// --- media / position -------------------------------------------------------------

void LiveSubs::mediaStarted(const QUrl &url, const QString &cookieHeader, double startSeconds)
{
    ++m_gen;
    m_url = url;
    m_cookie = cookieHeader;
    m_sceneId = sceneIdFromUrl(url);
    m_source.clear();
    emit sourceChanged();
    m_segments.clear();
    m_language.clear();
    m_duration = 0;
    m_pos = qMax(0.0, startSeconds);
    m_busy = false;
    m_translating = false;
    updateOverlay(true);
    if (m_mode != 0)
        startLive();
}

void LiveSubs::mediaStopped()
{
    ++m_gen;
    m_url.clear();
    m_sceneId.clear();
    m_source.clear();
    emit sourceChanged();
    m_segments.clear();
    m_busy = false;
    m_translating = false;
    m_waitingForStash = false;
    updateOverlay(true);
    genResume();
}

// Subtitles for the playing file: the scene's Stash captions if it has any,
// otherwise generated as it plays.
void LiveSubs::startLive()
{
    const quint64 gen = ++m_gen;
    m_busy = false;
    m_atEnd = false;
    auto generate = [this] {
        m_waitingForStash = false;
        if (ensureServers())
            resetAt(m_pos);
        if (wantTranslation())
            useLlama(llmPath(true), true); // load it while the first lines are transcribed
    };
    if (m_sceneId.isEmpty() || m_server.isEmpty()) {
        generate();
        return;
    }
    m_waitingForStash = true;
    stashGql(QStringLiteral("query($id: ID!) { findScene(id: $id) { captions { language_code caption_type } } }"),
             {{QStringLiteral("id"), m_sceneId}}, this, [this, gen, generate](const QJsonObject &data, const QString &) {
                 if (gen != m_gen)
                     return;
                 const QJsonArray captions =
                     data.value(QStringLiteral("findScene")).toObject().value(QStringLiteral("captions")).toArray();
                 if (captions.isEmpty()) {
                     generate();
                     return;
                 }
                 fetchCaptions(m_sceneId, captions, this,
                               [this, gen, generate](const QList<Cue> &cues, const QString &language, bool hasChinese) {
                                   if (gen != m_gen)
                                       return;
                                   if (cues.isEmpty()) {
                                       generate();
                                       return;
                                   }
                                   m_waitingForStash = false;
                                   m_segments = cues;
                                   m_language = language;
                                   m_source = QStringLiteral("stash");
                                   emit sourceChanged();
                                   m_coveredFrom = 0;
                                   m_cursor = 1e12; // nothing to transcribe
                                   m_atEnd = true;
                                   setStatus(QString());
                                   Q_UNUSED(hasChinese); // (pumpTranslation loads the model only if lines need it)
                                   updateOverlay(true);
                                   pumpTranslation();
                               });
             });
}

void LiveSubs::onPropertyChanged(const QString &name, const QVariant &value)
{
    if (name == QLatin1String("duration")) {
        m_duration = value.isValid() ? value.toDouble() : 0;
        return;
    }
    if (name != QLatin1String("time-pos") || !value.isValid())
        return;
    m_pos = value.toDouble();
    if (m_mode == 0 || !m_url.isValid() || m_waitingForStash)
        return;
    // A seek outside the region being transcribed restarts the pipeline there.
    const double ahead = m_busy ? m_cursor + kWindow : m_cursor;
    if (m_pos < m_coveredFrom - 0.5 || m_pos > ahead + 1.0)
        resetAt(m_pos);
    else
        pump();
    updateOverlay();
}

void LiveSubs::resetAt(double t)
{
    ++m_gen;
    m_busy = false;
    m_atEnd = false;
    m_ffmpegFailures = 0;
    m_crashWindow = -1;
    m_cursor = m_coveredFrom = qMax(0.0, t - 1.0);
    pump();
}

// --- transcription -----------------------------------------------------------------

void LiveSubs::pump()
{
    if (m_busy || m_mode == 0 || !m_whisperReady || !m_url.isValid() || m_atEnd || m_waitingForStash)
        return;
    if (m_cursor > m_pos + kLookahead)
        return;
    if (m_duration > 0 && m_cursor >= m_duration - 0.2) {
        m_atEnd = true;
        return;
    }
    m_busy = true;
    transcribe(m_cursor, m_gen);
}

void LiveSubs::transcribe(double windowStart, quint64 gen)
{
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) {
        setStatus(tr("Live subtitles need ffmpeg.exe in %1").arg(QDir::toNativeSeparators(m_aiDir + "/ffmpeg")));
        m_busy = false;
        return;
    }
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                     QStringLiteral("error")};
    if (!m_cookie.isEmpty())
        args << QStringLiteral("-headers") << QStringLiteral("Cookie: ") + m_cookie + QStringLiteral("\r\n");
    args << QStringLiteral("-ss") << QString::number(windowStart, 'f', 3) << QStringLiteral("-t")
         << QString::number(kWindow, 'f', 1) << QStringLiteral("-i") << m_url.toString(QUrl::FullyEncoded)
         << QStringLiteral("-vn") << QStringLiteral("-sn") << QStringLiteral("-dn") << QStringLiteral("-ac")
         << QStringLiteral("1") << QStringLiteral("-ar") << QStringLiteral("16000") << QStringLiteral("-c:a")
         << QStringLiteral("pcm_s16le") << QStringLiteral("-f") << QStringLiteral("wav") << QStringLiteral("-");

    auto *proc = new QProcess(this);
    hideConsole(proc);
    connect(proc, &QProcess::finished, this, [this, proc, windowStart, gen](int code) {
        proc->deleteLater();
        if (gen != m_gen)
            return;
        const QByteArray wav = proc->readAllStandardOutput();
        const int dataAt = wav.indexOf("data");
        const double len = dataAt < 0 ? 0 : double(wav.size() - dataAt - 8) / 32000.0;
        if (code != 0 || len < 0.2) {
            const bool beforeEnd = m_duration > 0 && windowStart < m_duration - 1.0;
            m_busy = false;
            if (code != 0) {
                const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
                qWarning().noquote() << "[livesubs] ffmpeg failed:" << err;
                debugLog(QStringLiteral("ffmpeg failed at %1: %2").arg(windowStart).arg(err));
            }
            // A read error before the end (a network hiccup): try again a few
            // times. Otherwise this is past the end: stop here.
            if (beforeEnd && ++m_ffmpegFailures <= 3)
                QTimer::singleShot(3000, this, &LiveSubs::pump);
            else
                m_atEnd = true;
            return;
        }
        m_ffmpegFailures = 0;
        QNetworkReply *reply = postWhisper(wav, m_language);
        connect(reply, &QNetworkReply::finished, this, [this, reply, windowStart, len, gen] {
            reply->deleteLater();
            if (gen != m_gen)
                return;
            if (reply->error() != QNetworkReply::NoError) {
                qWarning() << "[livesubs] whisper request failed:" << reply->errorString();
                m_busy = false;
                QTimer::singleShot(2000, this, &LiveSubs::pump);
                return;
            }
            onTranscribed(windowStart, len, reply->readAll(), gen);
        });
    });
    proc->start(ffmpeg, args);
}

void LiveSubs::onTranscribed(double windowStart, double windowLen, const QByteArray &json, quint64 gen)
{
    Q_UNUSED(gen);
    m_whisperCrashes = 0;
    m_crashWindow = -1;
    // A short window is the end of the file, unless the duration says
    // otherwise (then the read was cut short: carry on after it).
    const bool lastWindow =
        windowLen < kWindow - 0.5 && (m_duration <= 0 || windowStart + windowLen >= m_duration - 1.0);
    double advance = windowLen;
    const QList<Cue> cues = subs::buildLines(QJsonDocument::fromJson(json).object(), windowStart, windowLen,
                                             lastWindow, &m_language, &advance);

    // Replace anything previously transcribed in this range, then insert.
    const double from = windowStart, to = windowStart + advance;
    m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(),
                                    [&](const Cue &s) { return s.start >= from - 0.01 && s.start < to; }),
                     m_segments.end());
    for (const Cue &c : cues) {
        auto it = std::lower_bound(m_segments.begin(), m_segments.end(), c.start,
                                   [](const Cue &s, double t) { return s.start < t; });
        m_segments.insert(it, c);
    }
    m_cursor = windowStart + advance;
    m_atEnd = lastWindow;
    m_busy = false;
    updateOverlay(true);
    pumpTranslation();
    pump();
}

// --- translation -------------------------------------------------------------------

bool LiveSubs::wantTranslation() const
{
    return (m_mode == 2 || m_mode == 3) && !m_llamaGaveUp && !llmPath(true).isEmpty() &&
           !subs::isChinese(m_language);
}

void LiveSubs::pumpTranslation()
{
    if (m_translating || !wantTranslation()) {
        genResume();
        return;
    }
    // Translate the untranslated lines closest to (and after) the playback position first.
    QStringList lines;
    QList<double> keys; // segment start times identify the lines when the reply comes back
    for (int i = 0; i < m_segments.size() && lines.size() < subs::kTranslateBatch; ++i) {
        const Cue &s = m_segments[i];
        if (s.end >= m_pos - 1.0 && s.translation.isEmpty()) {
            lines << s.text;
            keys << s.start;
        }
    }
    if (lines.isEmpty()) {
        genResume();
        return;
    }
    if (!useLlama(llmPath(true), true))
        return; // pollReady() calls back once the model is loaded

    m_translating = true;
    const quint64 run = m_llamaRun;
    QNetworkReply *reply = postTranslation(lines);
    connect(reply, &QNetworkReply::finished, this, [this, reply, keys, run] {
        reply->deleteLater();
        // Lines are matched by start time, so results stay valid across seeks;
        // lines that were dropped meanwhile are simply not found.
        m_translating = false;
        QHash<int, QString> lines;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[livesubs] translation request failed:" << reply->errorString();
            debugLog(QStringLiteral("translation failed: ") + reply->errorString());
            // Retry (llama-server may be restarting or switching models: that
            // doesn't count); give up on these lines after a few failures in a row.
            if (run != m_llamaRun || ++m_translateFailures < 3) {
                QTimer::singleShot(2000, this, &LiveSubs::pumpTranslation);
                return;
            }
        } else {
            m_llamaCrashes = 0;
            lines = subs::parseTranslation(reply->readAll());
        }
        m_translateFailures = 0;
        for (int n = 0; n < keys.size(); ++n) {
            for (Cue &s : m_segments) {
                if (qAbs(s.start - keys[n]) < 1e-6) {
                    // never retry the same line forever: fall back to the original text
                    s.translation = subs::cleanTranslation(s.text, lines.value(n + 1));
                    if (s.translation.isEmpty())
                        s.translation = s.text;
                    break;
                }
            }
        }
        updateOverlay(true);
        pumpTranslation();
    });
}

// --- display -----------------------------------------------------------------------

void LiveSubs::updateOverlay(bool force)
{
    if (!m_player)
        return;
    QString main, sub;
    if (m_mode != 0 && m_url.isValid()) {
        for (const Cue &s : std::as_const(m_segments)) {
            if (s.start > m_pos)
                break;
            if (m_pos < s.end + 0.3) {
                const bool translated = !s.translation.isEmpty() && s.translation != s.text;
                if (m_mode == 1 || (!wantTranslation() && !translated)) {
                    main = s.text;
                } else if (m_mode == 2) {
                    main = translated ? s.translation : s.text;
                } else {
                    main = translated ? s.translation : s.text;
                    sub = translated ? s.text : QString();
                }
            }
        }
    }
    QString ass;
    if (!main.isEmpty()) {
        ass = QStringLiteral("{\\an2\\fnMicrosoft JhengHei UI\\fs54\\bord2.6\\shad1\\b1}") + assEscape(main);
        if (!sub.isEmpty())
            ass += QStringLiteral("\\N{\\fs38\\b0\\1c&HD8D8D8&}") + assEscape(sub);
    }
    if (ass == m_shown && !force)
        return;
    if (ass == m_shown && force && ass.isEmpty())
        return;
    m_shown = ass;
    if (ass.isEmpty())
        m_player->command({QStringLiteral("osd-overlay"), QLatin1String(kOverlayId), QStringLiteral("none"), QString()});
    else
        m_player->command({QStringLiteral("osd-overlay"), QLatin1String(kOverlayId), QStringLiteral("ass-events"), ass,
                           QStringLiteral("0"), QStringLiteral("1080")});
}
