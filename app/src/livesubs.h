#pragma once

#include "subtitlecore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QFileInfo>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>

class MpvItem;
class QNetworkReply;
class QProcess;

// Subtitles made on this PC: speech -> text with a local whisper.cpp server,
// translated with a local llama.cpp server.
//
// Live (while playing; one 30 s window at a time, kept up to kLookahead
// seconds ahead of the playback position):
//   ffmpeg (audio of [t, t+30) from the Stash stream URL, 16 kHz mono WAV)
//     -> whisper-server /inference (timed segments)
//     -> llama-server /v1/chat/completions (batch translation)
//     -> osd-overlay while time-pos is inside a line
// If the scene already has captions in Stash, those are shown instead (and
// only translated if there is no Chinese one).
//
// Whole video (scenes tagged kQueueTag in Stash, one at a time, in the
// background): the same steps over the whole file, then the StashNativePlayer
// plugin's server-side task saves <video>.<lang>.srt and <video>.zh.srt next
// to the video, so they become Stash captions.
//
// Everything runs on this PC; the tools and models live in aiDir() (see the
// README, "Live subtitles"). Not exposed to web pages.
class LiveSubs : public QObject
{
    Q_OBJECT
    // 0 = off, 1 = original language, 2 = translated, 3 = both
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString aiDir READ aiDir CONSTANT)
    // "stash" when the playing scene's subtitles come from its Stash captions
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
    Q_PROPERTY(bool hasScene READ hasScene NOTIFY sourceChanged)
    // whole-video generation
    Q_PROPERTY(QString genStatus READ genStatus NOTIFY genStatusChanged)
    Q_PROPERTY(bool genActive READ genActive NOTIFY genStatusChanged)
    Q_PROPERTY(bool genPaused READ genPaused WRITE setGenPaused NOTIFY genStatusChanged)
    // models (file names in modelsDir; for the translation models "none" =
    // no translation). Real-time and whole-video translation can use
    // different models; only one is loaded at a time (see useLlama()).
    Q_PROPERTY(QString modelsDir READ modelsDir WRITE setModelsDir NOTIFY modelsChanged)
    Q_PROPERTY(QStringList whisperModels READ whisperModels NOTIFY modelsChanged)
    Q_PROPERTY(QStringList llmModels READ llmModels NOTIFY modelsChanged)
    Q_PROPERTY(QString whisperModel READ whisperModel WRITE setWhisperModel NOTIFY modelsChanged)
    Q_PROPERTY(QString liveLlm READ liveLlm WRITE setLiveLlm NOTIFY modelsChanged)
    Q_PROPERTY(QString genLlm READ genLlm WRITE setGenLlm NOTIFY modelsChanged)
    Q_PROPERTY(QString loadedLlm READ loadedLlm NOTIFY modelsChanged)
    // whole-video generation skips: spoken languages (whisper codes, "en,ja")
    // and files with embedded subtitles (0 = never, 1 = Chinese ones, 2 = any)
    Q_PROPERTY(QString skipLanguages READ skipLanguages WRITE setSkipLanguages NOTIFY skipChanged)
    Q_PROPERTY(int embeddedSkip READ embeddedSkip WRITE setEmbeddedSkip NOTIFY skipChanged)
    // subtitles burned into the picture (found with Windows OCR): 0 = never
    // skip, 1 = skip Chinese ones, 2 = skip any
    Q_PROPERTY(int hardsubSkip READ hardsubSkip WRITE setHardsubSkip NOTIFY skipChanged)

public:
    static constexpr const char *kQueueTag = "產生字幕";
    static constexpr const char *kFailedTag = "產生字幕失敗";
    static constexpr const char *kNoSpeechTag = "無對白"; // no speech anywhere: nothing to save
    static constexpr const char *kEmbeddedTag = "內嵌字幕"; // skipped: the file has subtitle tracks
    static constexpr const char *kSkippedTag = "略過字幕";  // skipped: spoken language on the skip list
    static constexpr const char *kHardsubTag = "畫面字幕";  // skipped: subtitles burned into the picture

    explicit LiveSubs(QObject *parent = nullptr);
    ~LiveSubs() override;

    int mode() const { return m_mode; }
    void setMode(int mode);
    QString status() const { return m_status; }
    QString aiDir() const { return m_aiDir; }
    QString source() const { return m_source; }
    bool hasScene() const { return !m_sceneId.isEmpty(); }
    QString genStatus() const { return m_genStatus; }
    bool genActive() const { return !m_job.sceneId.isEmpty(); }
    bool genPaused() const { return m_genPaused; }
    void setGenPaused(bool paused);

    QString modelsDir() const { return m_modelsDir; }
    void setModelsDir(const QString &dir);
    QStringList whisperModels() const { return m_whisperModels; }
    QStringList llmModels() const { return m_llmModels; }
    QString whisperModel() const;
    void setWhisperModel(const QString &name);
    QString liveLlm() const { return llmName(true); }
    void setLiveLlm(const QString &name) { setLlm(true, name); }
    QString genLlm() const { return llmName(false); }
    void setGenLlm(const QString &name) { setLlm(false, name); }
    QString loadedLlm() const { return QFileInfo(m_llamaModel).fileName(); }
    QString skipLanguages() const { return m_skipLangs.join(QLatin1Char(',')); }
    void setSkipLanguages(const QString &codes);
    int embeddedSkip() const { return m_embeddedSkip; }
    void setEmbeddedSkip(int policy);
    int hardsubSkip() const { return m_hardsubSkip; }
    void setHardsubSkip(int policy);
    // Rescans modelsDir.
    Q_INVOKABLE void refreshModels();
    // "4.4 GB"
    Q_INVOKABLE QString modelSize(const QString &name) const;

    void setPlayer(MpvItem *player);
    // The Stash server, and how to get its cookies (the session cookie is
    // HttpOnly: it never goes through the page).
    void setServer(const QUrl &server);
    void setCookieProvider(std::function<QString(const QUrl &)> provider) { m_cookies = std::move(provider); }

    // Generate whole-video subtitles for the playing scene (tags it in Stash).
    Q_INVOKABLE void queueCurrentScene();
    // Look for scenes tagged kQueueTag now.
    Q_INVOKABLE void checkQueue();
    // Scenes with no captions at all (not already queued or failed): count
    // them (-> uncaptionedCounted), or tag them all kQueueTag.
    Q_INVOKABLE void countUncaptioned();
    Q_INVOKABLE void queueAllUncaptioned();

public slots:
    // From Bridge: a new file starts playing / playback stopped.
    void mediaStarted(const QUrl &url, const QString &cookieHeader, double startSeconds);
    void mediaStopped();

signals:
    void modeChanged();
    void statusChanged();
    void sourceChanged();
    void genStatusChanged();
    void modelsChanged();
    void skipChanged();
    // count < 0: the query failed (error says why)
    void uncaptionedCounted(int count, double seconds, const QString &error);

private:
    using Cue = subs::Cue;

    void setStatus(const QString &s);
    void setGenStatus(const QString &s);
    bool serversWanted() const;
    bool ensureServers();
    bool startWhisper();
    void startLlama(const QString &model);
    void stopLlama();
    void watchReady();
    // Makes sure the translation server runs `model`; true once it is ready.
    // Real-time subtitles come first: whole-video generation never swaps away
    // the model playback is using (it waits, see genResume()).
    bool useLlama(const QString &model, bool live);
    bool liveNeedsLlama() const;
    void genResume();
    QString llmName(bool live) const;
    void setLlm(bool live, const QString &name);
    QString llmPath(bool live) const; // "" = no translation
    QString whisperPath() const;
    bool llamaExeExists() const;
    void onServerExited(QProcess *proc, const QString &name, int code);
    void startServer(QProcess *&proc, const QString &exe, const QStringList &args, const QString &name);
    void pollReady();
    void stopServers();
    void stopServersIfIdle();
    QString ffmpegPath() const;
    QNetworkReply *postWhisper(const QByteArray &wav, const QString &language);
    QNetworkReply *postTranslation(const QStringList &lines);

    // Stash
    QUrl stashUrl(const QString &path) const;
    QString cookieFor(const QUrl &url) const;
    void stashGql(const QString &query, const QJsonObject &variables, QObject *context,
                  std::function<void(const QJsonObject &data, const QString &error)> done);
    void stashGet(const QUrl &url, QObject *context, std::function<void(const QByteArray &, bool ok)> done);
    void findOrCreateTag(const QString &name, bool create, std::function<void(const QString &id)> done);
    static QString sceneIdFromUrl(const QUrl &url);
    // Downloads a scene's Stash captions: the original-language one (any
    // non-Chinese) into Cue::text and the Chinese one into Cue::translation.
    void fetchCaptions(const QString &sceneId, const QJsonArray &captions, QObject *context,
                       std::function<void(const QList<Cue> &cues, const QString &language, bool hasChinese)> done);

    // live
    void startLive();
    void onPropertyChanged(const QString &name, const QVariant &value);
    void resetAt(double t);
    void pump();
    void transcribe(double windowStart, quint64 gen);
    void onTranscribed(double windowStart, double windowLen, const QByteArray &json, quint64 gen);
    void pumpTranslation();
    bool wantTranslation() const;
    void updateOverlay(bool force = false);

    // whole video (subtitlegen.cpp)
    void genNext();
    void genStart(const QJsonObject &scene);
    void genProbe();
    void genDetect(int attempt);
    void genAfterDetect();
    void genFrames(int index);
    void genOcr();
    void genExtract();
    void genSkip(const char *tag, const QString &note);
    void genTranscribeNext();
    void genTranslateNext();
    void genUpload();
    void genFinish(const QString &error);
    void genProgress(const QString &step, double fraction);
    void findUncaptioned(bool ids, std::function<void(const QJsonObject &found, const QString &error)> done);

    QPointer<MpvItem> m_player;
    QNetworkAccessManager m_net;
    QString m_aiDir;
    int m_mode = 0;
    QString m_status;
    QUrl m_server;
    std::function<QString(const QUrl &)> m_cookies;

    QProcess *m_whisper = nullptr;
    QProcess *m_llama = nullptr;
    int m_whisperPort = 0;
    int m_llamaPort = 0;
    bool m_whisperReady = false;
    bool m_llamaReady = false;
    bool m_llamaGaveUp = false; // kept crashing
    QString m_llamaModel;      // model the running llama-server has
    QString m_whisperModelPath; // model the running whisper-server has
    quint64 m_llamaRun = 0;    // bumped on every llama-server start
    QString m_modelsDir;
    QStringList m_whisperModels, m_llmModels;
    QString m_whisperSetting, m_liveLlmSetting, m_genLlmSetting;
    bool m_polling = false;
    int m_whisperCrashes = 0; // in a row; reset by a successful request
    int m_llamaCrashes = 0;
    double m_crashWindow = -1; // window being transcribed when whisper last crashed
    int m_ffmpegFailures = 0;
    int m_translateFailures = 0;
    void *m_jobObject = nullptr; // Windows job object: servers die with the app

    // current media
    QUrl m_url;
    QString m_cookie;
    QString m_sceneId;
    QString m_source; // "" (generated live) or "stash"
    quint64 m_gen = 0; // bumped on file change / seek; stale results are dropped
    double m_pos = 0;
    double m_duration = 0;
    double m_coveredFrom = 0; // transcribed region is [m_coveredFrom, m_cursor)
    double m_cursor = 0;
    bool m_busy = false;
    bool m_atEnd = false;
    bool m_waitingForStash = false;
    QString m_language; // detected on the first window, then fixed
    QList<Cue> m_segments; // sorted by start
    bool m_translating = false;
    QString m_shown;

    // whole-video generation: one scene at a time
    struct Job {
        enum Phase { Idle, Probe, Detect, Extract, Transcribe, Translate, Upload };
        Phase phase = Idle;
        bool waiting = false; // for a server to be ready
        QString sceneId;
        QString title;
        QUrl stream;
        double duration = 0;
        bool haveOriginal = false; // Stash already has an original-language caption
        QString language;
        QString wavPath;
        qint64 dataOffset = 0;
        qint64 dataSize = 0;
        double cursor = 0;
        int failures = 0;
        QList<Cue> cues;
        int translated = 0; // cues[0, translated) are done
        int left = 0;       // scenes still tagged, for the status
        int detectAttempt = 0;
        QList<double> speechTimes;  // moments with speech (from the language samples)
        QList<QByteArray> frames;   // PNGs of the bottom of the picture at those moments
        QString finishTag;  // tag the scene gets when done (no speech / skipped)
        QString finishNote; // status text then
    };
    Job m_job;
    quint64 m_jobGen = 0; // bumped when a job ends; stale callbacks are dropped
    QProcess *m_extract = nullptr;
    QString m_queueTagId;
    QString m_genStatus;
    bool m_genPaused = false;
    bool m_checkingQueue = false;
    QStringList m_skipLangs;
    int m_embeddedSkip = 2;
    int m_hardsubSkip = 1;
    QStringList m_failedThisRun; // never retried in a loop
    QTimer m_queueTimer;
};
