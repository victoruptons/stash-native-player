// Whole-video subtitle generation (part of LiveSubs; see livesubs.h).
//
// The queue is a Stash tag: every scene tagged LiveSubs::kQueueTag gets
// subtitles, one at a time, and loses the tag when done (or gets
// kFailedTag instead). Tag scenes one by one, or many at once with Stash's
// bulk edit; "generate for this scene" in the app tags the playing scene.

#include "livesubs.h"

#include "screenocr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <memory>

using subs::Cue;
using subs::debugLog;
using subs::kWindow;

namespace {

void hideConsoleWindow(QProcess *p)
{
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; /* CREATE_NO_WINDOW */ });
#else
    Q_UNUSED(p);
#endif
}

const QString kSceneFields = QStringLiteral(
    "id title files { basename duration } paths { stream } captions { language_code caption_type }");

} // namespace

void LiveSubs::setGenPaused(bool paused)
{
    if (paused == m_genPaused)
        return;
    m_genPaused = paused;
    QSettings().setValue(QStringLiteral("liveSubs/genPaused"), paused);
    emit genStatusChanged();
    if (paused) {
        setGenStatus(genActive() ? tr("字幕產生已暫停：%1").arg(m_job.title) : QString());
        stopServersIfIdle();
        return;
    }
    if (!genActive()) {
        checkQueue();
        return;
    }
    if (m_job.phase == Job::Detect)
        genDetect(m_job.detectAttempt);
    else if (m_job.phase == Job::Transcribe)
        genTranscribeNext();
    else if (m_job.phase == Job::Translate)
        genTranslateNext();
}

void LiveSubs::queueCurrentScene()
{
    if (m_sceneId.isEmpty() || m_server.isEmpty())
        return;
    const QString sceneId = m_sceneId;
    m_failedThisRun.removeAll(sceneId);
    setGenStatus(tr("加入字幕佇列…"));
    findOrCreateTag(QString::fromUtf8(kQueueTag), true, [this, sceneId](const QString &tagId) {
        if (tagId.isEmpty()) {
            setGenStatus(tr("無法建立「%1」標籤").arg(QString::fromUtf8(kQueueTag)));
            return;
        }
        m_queueTagId = tagId;
        stashGql(QStringLiteral("mutation($ids: [ID!], $t: [ID!]) { bulkSceneUpdate(input: {ids: $ids, "
                                "tag_ids: {ids: $t, mode: ADD}}) { id } }"),
                 {{QStringLiteral("ids"), QJsonArray{sceneId}}, {QStringLiteral("t"), QJsonArray{tagId}}}, this,
                 [this](const QJsonObject &, const QString &error) {
                     if (!error.isEmpty()) {
                         setGenStatus(tr("加入字幕佇列失敗：%1").arg(error));
                         return;
                     }
                     if (!genActive())
                         setGenStatus(tr("已加入字幕佇列"));
                     checkQueue();
                 });
    });
}

void LiveSubs::checkQueue()
{
    if (m_server.isEmpty() || m_checkingQueue || genActive() || m_genPaused)
        return;
    m_checkingQueue = true;
    findOrCreateTag(QString::fromUtf8(kQueueTag), false, [this](const QString &tagId) {
        if (tagId.isEmpty()) {
            m_checkingQueue = false;
            return;
        }
        m_queueTagId = tagId;
        stashGql(QStringLiteral("query($t: [ID!]) { findScenes(scene_filter: {tags: {value: $t, modifier: INCLUDES}}, "
                                "filter: {per_page: 50, sort: \"created_at\", direction: ASC}) { count scenes { ") +
                     kSceneFields + QStringLiteral(" } } }"),
                 {{QStringLiteral("t"), QJsonArray{tagId}}}, this, [this](const QJsonObject &data, const QString &error) {
                     m_checkingQueue = false;
                     if (!error.isEmpty() || genActive() || m_genPaused) {
                         if (!error.isEmpty())
                             debugLog(QStringLiteral("queue check failed: ") + error);
                         return;
                     }
                     const QJsonObject found = data.value(QStringLiteral("findScenes")).toObject();
                     for (const QJsonValue &v : found.value(QStringLiteral("scenes")).toArray()) {
                         const QJsonObject scene = v.toObject();
                         if (m_failedThisRun.contains(scene.value(QStringLiteral("id")).toString()))
                             continue;
                         m_job.left = found.value(QStringLiteral("count")).toInt();
                         genStart(scene);
                         return;
                     }
                     stopServersIfIdle();
                 });
    });
}

void LiveSubs::genStart(const QJsonObject &scene)
{
    const int left = m_job.left;
    m_job = Job{};
    ++m_jobGen;
    m_job.left = left;
    m_job.sceneId = scene.value(QStringLiteral("id")).toString();
    const QJsonObject file = scene.value(QStringLiteral("files")).toArray().at(0).toObject();
    m_job.title = scene.value(QStringLiteral("title")).toString();
    if (m_job.title.isEmpty())
        m_job.title = file.value(QStringLiteral("basename")).toString();
    m_job.duration = file.value(QStringLiteral("duration")).toDouble();
    m_job.stream = QUrl(scene.value(QStringLiteral("paths")).toObject().value(QStringLiteral("stream")).toString());
    emit genStatusChanged();
    debugLog(QStringLiteral("generate scene %1 (%2)").arg(m_job.sceneId, m_job.title));

    const QJsonArray captions = scene.value(QStringLiteral("captions")).toArray();
    bool hasChinese = false, hasOriginal = false;
    for (const QJsonValue &v : captions) {
        if (subs::isChinese(v.toObject().value(QStringLiteral("language_code")).toString()))
            hasChinese = true;
        else
            hasOriginal = true;
    }
    if (hasChinese) {
        genFinish(QString()); // already done (by us or by hand): just untag it
        return;
    }
    if (!m_job.stream.isValid() || m_job.duration <= 0) {
        genFinish(tr("影片沒有可讀取的檔案"));
        return;
    }
    if (hasOriginal) {
        // Subtitles in another language exist already: only translate them.
        const quint64 jobGen = m_jobGen;
        m_job.phase = Job::Translate;
        m_job.haveOriginal = true;
        genProgress(tr("讀取現有字幕"), 0);
        fetchCaptions(m_job.sceneId, captions, this,
                      [this, jobGen](const QList<Cue> &cues, const QString &language, bool) {
                          if (jobGen != m_jobGen)
                              return;
                          if (cues.isEmpty()) {
                              genFinish(tr("無法讀取現有字幕"));
                              return;
                          }
                          m_job.cues = cues;
                          m_job.language = language;
                          if (m_skipLangs.contains(language.left(2))) {
                              genSkip(kSkippedTag, tr("略過（%1）").arg(language));
                              return;
                          }
                          genTranslateNext();
                      });
        return;
    }
    if (!ensureServers()) {
        genFinish(m_status);
        return;
    }
    genProbe();
}

void LiveSubs::genSkip(const char *tag, const QString &note)
{
    m_job.finishTag = QString::fromUtf8(tag);
    m_job.finishNote = note;
    genFinish(QString());
}

void LiveSubs::setSkipLanguages(const QString &codes)
{
    static const QRegularExpression code(QStringLiteral("^[a-z]{2,3}$"));
    static const QRegularExpression separators(QStringLiteral("[\\s,;、，]+"));
    QStringList list;
    for (const QString &c : codes.toLower().split(separators, Qt::SkipEmptyParts))
        if (code.match(c).hasMatch() && !list.contains(c))
            list << c;
    if (list == m_skipLangs)
        return;
    m_skipLangs = list;
    QSettings().setValue(QStringLiteral("liveSubs/skipLanguages"), list);
    emit skipChanged();
}

void LiveSubs::setHardsubSkip(int policy)
{
    policy = qBound(0, policy, 2);
    if (policy == m_hardsubSkip)
        return;
    m_hardsubSkip = policy;
    QSettings().setValue(QStringLiteral("liveSubs/hardsubSkip"), policy);
    emit skipChanged();
}

void LiveSubs::setEmbeddedSkip(int policy)
{
    policy = qBound(0, policy, 2);
    if (policy == m_embeddedSkip)
        return;
    m_embeddedSkip = policy;
    QSettings().setValue(QStringLiteral("liveSubs/embeddedSkip"), policy);
    emit skipChanged();
}

namespace {
QString languageName(const QString &code)
{
    static const QHash<QString, QString> names{{QStringLiteral("zh"), QStringLiteral("中文")},
                                               {QStringLiteral("en"), QStringLiteral("英語")},
                                               {QStringLiteral("ja"), QStringLiteral("日語")},
                                               {QStringLiteral("ko"), QStringLiteral("韓語")}};
    return names.value(code, code);
}
} // namespace

// Looks at the file's tracks (ffmpeg reads just the header): skip files that
// carry their own subtitles, if so configured.
void LiveSubs::genProbe()
{
    const QString ffmpeg = ffmpegPath();
    if (m_embeddedSkip == 0 || ffmpeg.isEmpty()) {
        genDetect(0);
        return;
    }
    m_job.phase = Job::Probe;
    genProgress(tr("檢查內嵌字幕"), 0);
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-hide_banner")};
    const QString cookie = cookieFor(m_job.stream);
    if (!cookie.isEmpty())
        args << QStringLiteral("-headers") << QStringLiteral("Cookie: ") + cookie + QStringLiteral("\r\n");
    args << QStringLiteral("-i") << m_job.stream.toString(QUrl::FullyEncoded);
    auto *proc = new QProcess(this);
    hideConsoleWindow(proc);
    const quint64 jobGen = m_jobGen;
    connect(proc, &QProcess::finished, this, [this, proc, jobGen] {
        proc->deleteLater();
        if (jobGen != m_jobGen)
            return;
        // "  Stream #0:2(chi): Subtitle: subrip (default)" + "      title : 繁體中文"
        static const QRegularExpression stream(
            QStringLiteral("^\\s*Stream #\\d+:\\d+(?:\\[\\w+\\])?(?:\\((\\w+)\\))?: (\\w+):"));
        static const QRegularExpression title(QStringLiteral("^\\s+title\\s*:\\s*(.*)$"));
        static const QRegularExpression chineseLang(QStringLiteral("^(chi|zho|zh|chs|cht|cmn|yue)"),
                                                    QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression chineseTitle(QStringLiteral("中|繁|简|chinese|chs|cht"),
                                                     QRegularExpression::CaseInsensitiveOption);
        int subtitles = 0;
        bool chinese = false, inSubtitle = false;
        const QStringList lines = QString::fromUtf8(proc->readAllStandardError()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const auto m = stream.match(line);
            if (m.hasMatch()) {
                inSubtitle = m.captured(2) == QLatin1String("Subtitle");
                if (inSubtitle) {
                    ++subtitles;
                    chinese = chinese || chineseLang.match(m.captured(1)).hasMatch();
                }
                continue;
            }
            const auto t = title.match(line);
            if (inSubtitle && t.hasMatch() && chineseTitle.match(t.captured(1)).hasMatch())
                chinese = true;
        }
        debugLog(QStringLiteral("probe scene %1: %2 subtitle track(s)%3")
                     .arg(m_job.sceneId)
                     .arg(subtitles)
                     .arg(chinese ? QStringLiteral(", Chinese") : QString()));
        if ((m_embeddedSkip == 2 && subtitles > 0) || (m_embeddedSkip == 1 && chinese)) {
            genSkip(kEmbeddedTag, chinese ? tr("已有內嵌中文字幕") : tr("已有內嵌字幕"));
            return;
        }
        genDetect(0);
    });
    proc->start(ffmpeg, args);
}

// Before spending time on the whole file, listen to 30 s samples (at 20%,
// then 50%, 80% of the video): whisper names the spoken language (for the
// language skip list) and tells when people speak (where burned-in subtitles
// would be on screen, for genOcr()).
void LiveSubs::genDetect(int attempt)
{
    static const double at[] = {0.2, 0.5, 0.8};
    const bool needLanguage = !m_skipLangs.isEmpty() && m_job.language.isEmpty();
    const bool needSpeech = m_hardsubSkip != 0 && m_job.speechTimes.size() < 6;
    if ((!needLanguage && !needSpeech) || attempt >= 3) {
        genAfterDetect(); // (no clear speech in the samples: transcribe anyway)
        return;
    }
    m_job.phase = Job::Detect;
    m_job.detectAttempt = attempt;
    if (m_genPaused)
        return;
    if (!m_whisperReady) {
        m_job.waiting = true;
        if (!m_whisper && !ensureServers())
            genFinish(m_status);
        return;
    }
    genProgress(tr("判斷語言"), 0);
    const QString ffmpeg = ffmpegPath();
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                     QStringLiteral("error")};
    const QString cookie = cookieFor(m_job.stream);
    if (!cookie.isEmpty())
        args << QStringLiteral("-headers") << QStringLiteral("Cookie: ") + cookie + QStringLiteral("\r\n");
    args << QStringLiteral("-ss") << QString::number(m_job.duration * at[attempt], 'f', 1) << QStringLiteral("-t")
         << QStringLiteral("30") << QStringLiteral("-i") << m_job.stream.toString(QUrl::FullyEncoded)
         << QStringLiteral("-vn") << QStringLiteral("-sn") << QStringLiteral("-dn") << QStringLiteral("-ac")
         << QStringLiteral("1") << QStringLiteral("-ar") << QStringLiteral("16000") << QStringLiteral("-c:a")
         << QStringLiteral("pcm_s16le") << QStringLiteral("-f") << QStringLiteral("wav") << QStringLiteral("-");
    auto *proc = new QProcess(this);
    hideConsoleWindow(proc);
    const quint64 jobGen = m_jobGen;
    const double sampleAt = m_job.duration * at[attempt];
    connect(proc, &QProcess::finished, this, [this, proc, jobGen, attempt, sampleAt] {
        proc->deleteLater();
        if (jobGen != m_jobGen)
            return;
        const QByteArray wav = proc->readAllStandardOutput();
        if (wav.size() < 44 + 32000) { // unreadable or no audio here
            genDetect(attempt + 1);
            return;
        }
        QNetworkReply *reply = postWhisper(wav, QString());
        connect(reply, &QNetworkReply::finished, this, [this, reply, jobGen, attempt, sampleAt] {
            reply->deleteLater();
            if (jobGen != m_jobGen)
                return;
            const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
            // moments with speech: the middle of up to 4 lines of this sample
            const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
            const int step = qMax(1, int(segments.size()) / 4);
            for (int i = 0; i < segments.size() && m_job.speechTimes.size() < 12; i += step) {
                const QJsonObject s = segments[i].toObject();
                const double mid = (s.value(QStringLiteral("start")).toDouble() + s.value(QStringLiteral("end")).toDouble()) / 2;
                m_job.speechTimes << sampleAt + mid;
            }
            QString language;
            if (reply->error() == QNetworkReply::NoError &&
                root.value(QStringLiteral("detected_language_probability")).toDouble() > 0.6) {
                double best = 0;
                const QJsonObject probs = root.value(QStringLiteral("language_probabilities")).toObject();
                for (auto it = probs.begin(); it != probs.end(); ++it)
                    if (it.value().toDouble() > best) {
                        best = it.value().toDouble();
                        language = it.key();
                    }
            }
            debugLog(QStringLiteral("detect scene %1 sample %2: %3, %4 speech moments")
                         .arg(m_job.sceneId).arg(attempt).arg(language).arg(m_job.speechTimes.size()));
            if (!language.isEmpty()) {
                if (m_skipLangs.contains(language)) {
                    genSkip(kSkippedTag, tr("略過（%1）").arg(languageName(language)));
                    return;
                }
                if (m_job.language.isEmpty())
                    m_job.language = language; // known now: whisper doesn't need to guess per window
            }
            genDetect(attempt + 1); // returns to genAfterDetect() once nothing more is needed
        });
    });
    proc->start(ffmpeg, args);
}

void LiveSubs::genAfterDetect()
{
    if (m_hardsubSkip != 0 && m_job.speechTimes.size() >= 3)
        genFrames(0);
    else
        genExtract();
}

// One frame per moment with speech: the bottom 30 % of the picture, as PNG.
void LiveSubs::genFrames(int index)
{
    const int count = qMin<int>(m_job.speechTimes.size(), 8);
    if (index >= count) {
        genOcr();
        return;
    }
    if (index == 0) {
        m_job.frames.clear();
        genProgress(tr("檢查畫面字幕"), 0);
    }
    // spread over all the moments found
    const double t = m_job.speechTimes[index * m_job.speechTimes.size() / count];
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                     QStringLiteral("error")};
    const QString cookie = cookieFor(m_job.stream);
    if (!cookie.isEmpty())
        args << QStringLiteral("-headers") << QStringLiteral("Cookie: ") + cookie + QStringLiteral("\r\n");
    args << QStringLiteral("-ss") << QString::number(t, 'f', 2) << QStringLiteral("-i")
         << m_job.stream.toString(QUrl::FullyEncoded) << QStringLiteral("-frames:v") << QStringLiteral("1")
         << QStringLiteral("-an") << QStringLiteral("-vf")
         << QStringLiteral("crop=iw:ih*0.3:0:ih*0.7,scale='min(1600,iw)':-2") << QStringLiteral("-f")
         << QStringLiteral("image2pipe") << QStringLiteral("-c:v") << QStringLiteral("png") << QStringLiteral("-");
    auto *proc = new QProcess(this);
    hideConsoleWindow(proc);
    const quint64 jobGen = m_jobGen;
    connect(proc, &QProcess::finished, this, [this, proc, jobGen, index] {
        proc->deleteLater();
        if (jobGen != m_jobGen)
            return;
        const QByteArray png = proc->readAllStandardOutput();
        if (png.startsWith("\x89PNG"))
            m_job.frames << png;
        genFrames(index + 1);
    });
    proc->start(ffmpegPath(), args);
}

namespace {

struct HardsubVerdict {
    int frames = 0;      // frames read
    int textFrames = 0;  // frames with changing text (subtitles, not a logo)
    int chineseFrames = 0;
};

// Burned-in subtitles: text at the bottom of most frames taken while people
// speak, and different text in each (a logo, watermark or channel name is the
// same everywhere: lines found in half the frames or more are ignored).
HardsubVerdict judgeFrames(const QList<QStringList> &frames)
{
    static const QRegularExpression han(QStringLiteral("\\p{Han}"));
    static const QRegularExpression letter(QStringLiteral("[A-Za-z]"));
    auto norm = [](QString s) {
        s.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]")));
        return s.toLower();
    };
    QHash<QString, int> lineCount;
    for (const QStringList &lines : frames) {
        QSet<QString> seen;
        for (const QString &l : lines)
            seen.insert(norm(l));
        for (const QString &l : std::as_const(seen))
            ++lineCount[l];
    }
    HardsubVerdict v;
    v.frames = frames.size();
    QSet<QString> distinct;
    for (const QStringList &lines : frames) {
        QString text;
        for (const QString &l : lines) {
            const QString n = norm(l);
            if (lineCount.value(n) * 2 < frames.size() || frames.size() < 4)
                text += n;
        }
        const int hanCount = int(text.count(han));
        const int letters = int(text.count(letter));
        if (hanCount >= 2 || letters >= 6) {
            ++v.textFrames;
            distinct.insert(text);
            if (hanCount >= 2)
                ++v.chineseFrames;
        }
    }
    if (distinct.size() * 2 < v.textFrames) // mostly the same text: not subtitles
        v.textFrames = v.chineseFrames = 0;
    return v;
}

} // namespace

void LiveSubs::genOcr()
{
    if (m_job.frames.size() < 3) {
        genExtract();
        return;
    }
    // Windows OCR blocks: run it on a worker thread. The Chinese (Traditional)
    // engine also reads Latin letters; without it, the user's languages.
    const QList<QByteArray> frames = m_job.frames;
    const quint64 jobGen = m_jobGen;
    QPointer<LiveSubs> self(this);
    QThread *worker = QThread::create([self, frames, jobGen] {
        std::wstring language;
        for (const std::wstring &l : screenocr::languages())
            if (l.rfind(L"zh-Hant", 0) == 0 || (language.empty() && l.rfind(L"zh", 0) == 0))
                language = l;
        QList<QStringList> texts;
        for (const QByteArray &png : frames) {
            QStringList lines;
            for (const std::wstring &l : screenocr::recognize(std::vector<std::uint8_t>(png.begin(), png.end()), language))
                lines << QString::fromStdWString(l);
            texts << lines;
        }
        QMetaObject::invokeMethod(self, [self, texts, jobGen] {
            if (!self || jobGen != self->m_jobGen)
                return;
            const HardsubVerdict v = judgeFrames(texts);
            subs::debugLog(QStringLiteral("hardsub scene %1: %2/%3 frames with subtitles, %4 Chinese: %5")
                               .arg(self->m_job.sceneId)
                               .arg(v.textFrames)
                               .arg(v.frames)
                               .arg(v.chineseFrames)
                               .arg(texts.isEmpty() ? QString() : texts.first().join(QLatin1Char('|'))));
            const bool found = v.textFrames >= 3 && v.textFrames * 5 >= v.frames * 2; // >= 40 %
            const bool chinese = found && v.chineseFrames * 2 >= v.textFrames;
            if ((self->m_hardsubSkip == 2 && found) || (self->m_hardsubSkip == 1 && chinese)) {
                self->genSkip(kHardsubTag, chinese ? tr("畫面已有中文字幕") : tr("畫面已有字幕"));
                return;
            }
            self->genExtract();
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// The whole audio track, once, into a temporary 16 kHz mono WAV.
void LiveSubs::genExtract()
{
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) {
        genFinish(tr("找不到 ffmpeg.exe"));
        return;
    }
    m_job.phase = Job::Extract;
    m_job.wavPath = QDir::temp().filePath(QStringLiteral("snp-subs-%1.wav").arg(m_job.sceneId));
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                     QStringLiteral("error"), QStringLiteral("-nostats"), QStringLiteral("-progress"),
                     QStringLiteral("pipe:1")};
    const QString cookie = cookieFor(m_job.stream);
    if (!cookie.isEmpty())
        args << QStringLiteral("-headers") << QStringLiteral("Cookie: ") + cookie + QStringLiteral("\r\n");
    args << QStringLiteral("-i") << m_job.stream.toString(QUrl::FullyEncoded) << QStringLiteral("-vn")
         << QStringLiteral("-sn") << QStringLiteral("-dn") << QStringLiteral("-ac") << QStringLiteral("1")
         << QStringLiteral("-ar") << QStringLiteral("16000") << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le")
         << QStringLiteral("-f") << QStringLiteral("wav") << QStringLiteral("-y") << m_job.wavPath;

    auto *proc = new QProcess(this);
    m_extract = proc;
    hideConsoleWindow(proc);
    genProgress(tr("讀取音訊"), 0);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        static const QRegularExpression re(QStringLiteral("out_time_us=(\\d+)"));
        auto it = re.globalMatch(QString::fromLatin1(proc->readAllStandardOutput()));
        double t = -1;
        while (it.hasNext())
            t = it.next().captured(1).toDouble() / 1e6;
        if (t >= 0 && m_job.duration > 0 && proc == m_extract)
            genProgress(tr("讀取音訊"), t / m_job.duration);
    });
    const quint64 jobGen = m_jobGen;
    connect(proc, &QProcess::finished, this, [this, proc, jobGen](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (proc == m_extract)
            m_extract = nullptr;
        if (jobGen != m_jobGen)
            return;
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        QFile f(m_job.wavPath);
        qint64 at = -1;
        if (code == 0 && status == QProcess::NormalExit && f.open(QIODevice::ReadOnly))
            at = f.read(4096).indexOf("data");
        if (at < 0) {
            debugLog(QStringLiteral("audio extraction failed: ") + err);
            if (++m_job.failures <= 2) {
                QTimer::singleShot(3000, this, [this, jobGen] {
                    if (jobGen == m_jobGen)
                        genExtract();
                });
            } else {
                genFinish(tr("無法讀取音訊：%1").arg(err.isEmpty() ? tr("ffmpeg 錯誤 %1").arg(code) : err));
            }
            return;
        }
        m_job.failures = 0;
        m_job.dataOffset = at + 8;
        m_job.dataSize = (f.size() - m_job.dataOffset) & ~qint64(1);
        m_job.phase = Job::Transcribe;
        m_job.cursor = 0;
        genTranscribeNext();
    });
    proc->start(ffmpeg, args);
}

void LiveSubs::genTranscribeNext()
{
    if (m_genPaused || m_job.phase != Job::Transcribe)
        return;
    if (!m_whisperReady) {
        m_job.waiting = true;
        if (!m_whisper && !ensureServers())
            genFinish(m_status);
        return;
    }
    const double total = m_job.dataSize / 32000.0;
    if (m_job.cursor >= total - 0.2) {
        m_job.phase = Job::Translate;
        genTranslateNext();
        return;
    }
    QFile f(m_job.wavPath);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(m_job.dataOffset + qint64(m_job.cursor * 16000) * 2)) {
        genFinish(tr("暫存音訊檔不見了"));
        return;
    }
    const QByteArray pcm = f.read(qint64(kWindow * 16000) * 2);
    const double windowStart = m_job.cursor;
    const double windowLen = pcm.size() / 32000.0;
    const bool lastWindow = windowStart + windowLen >= total - 0.2;
    genProgress(tr("轉錄"), windowStart / qMax(1.0, total));

    const quint64 jobGen = m_jobGen;
    QNetworkReply *reply = postWhisper(subs::wavFile(pcm), m_job.language);
    connect(reply, &QNetworkReply::finished, this, [this, reply, jobGen, windowStart, windowLen, lastWindow] {
        reply->deleteLater();
        if (jobGen != m_jobGen)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            debugLog(QStringLiteral("generate: whisper failed at %1: %2").arg(windowStart).arg(reply->errorString()));
            if (++m_job.failures > 3) {
                m_job.failures = 0;
                m_job.cursor = windowStart + windowLen; // give up on this window
            }
            QTimer::singleShot(2000, this, [this, jobGen] {
                if (jobGen == m_jobGen)
                    genTranscribeNext();
            });
            return;
        }
        m_job.failures = 0;
        m_whisperCrashes = 0;
        double advance = windowLen;
        m_job.cues += subs::buildLines(QJsonDocument::fromJson(reply->readAll()).object(), windowStart, windowLen,
                                       lastWindow, &m_job.language, &advance);
        m_job.cursor = windowStart + (lastWindow ? windowLen : advance);
        genTranscribeNext();
    });
}

void LiveSubs::genTranslateNext()
{
    if (m_genPaused || m_job.phase != Job::Translate)
        return;
    const QString model = llmPath(false);
    // Nothing (more) to translate, or no translation model: save what there is.
    if (subs::isChinese(m_job.language) || m_job.translated >= m_job.cues.size() || model.isEmpty() ||
        m_llamaGaveUp) {
        genUpload();
        return;
    }
    if (!useLlama(model, false)) {
        m_job.waiting = true; // resumed by pollReady() or genResume()
        if (m_llamaModel != model) {
            // Playback holds the other model: look again in a while.
            QTimer::singleShot(5000, this, [this, jobGen = m_jobGen] {
                if (jobGen == m_jobGen)
                    genResume();
            });
            setGenStatus(tr("產生字幕：%1 — 等播放結束再翻譯（播放中使用 %2）")
                             .arg(m_job.title, QFileInfo(m_llamaModel).completeBaseName()));
        } else {
            genProgress(tr("載入翻譯模型"), double(m_job.translated) / m_job.cues.size());
        }
        return;
    }
    genProgress(tr("翻譯"), double(m_job.translated) / m_job.cues.size());
    const int from = m_job.translated;
    const int count = qMin(subs::kTranslateBatch, int(m_job.cues.size()) - from);
    QStringList lines;
    for (int i = from; i < from + count; ++i)
        lines << m_job.cues[i].text;
    const quint64 jobGen = m_jobGen;
    const quint64 run = m_llamaRun;
    QNetworkReply *reply = postTranslation(lines);
    connect(reply, &QNetworkReply::finished, this, [this, reply, jobGen, from, count, run] {
        reply->deleteLater();
        if (jobGen != m_jobGen)
            return;
        QHash<int, QString> out;
        if (reply->error() != QNetworkReply::NoError) {
            debugLog(QStringLiteral("generate: translation failed: ") + reply->errorString());
            // (a model switch for playback doesn't count as a failure)
            if (run != m_llamaRun || ++m_job.failures <= 3) {
                QTimer::singleShot(2000, this, [this, jobGen] {
                    if (jobGen == m_jobGen)
                        genTranslateNext();
                });
                return;
            }
        } else {
            m_llamaCrashes = 0;
            out = subs::parseTranslation(reply->readAll());
        }
        m_job.failures = 0;
        for (int n = 0; n < count; ++n) {
            Cue &c = m_job.cues[from + n];
            c.translation = subs::cleanTranslation(c.text, out.value(n + 1));
            if (c.translation.isEmpty())
                c.translation = c.text;
        }
        m_job.translated = from + count;
        genTranslateNext();
    });
}

// Saves the subtitle files next to the video through the plugin's server task.
void LiveSubs::genUpload()
{
    m_job.phase = Job::Upload;
    if (!m_job.wavPath.isEmpty()) {
        QFile::remove(m_job.wavPath);
        m_job.wavPath.clear();
    }
    if (m_job.cues.isEmpty()) {
        genSkip(kNoSpeechTag, tr("沒有對白")); // no speech in the whole video: nothing to save
        return;
    }
    static const QRegularExpression code(QStringLiteral("^[a-z]{2,3}$"));
    QList<QPair<QString, QString>> files; // language code, SubRip text
    const bool chinese = subs::isChinese(m_job.language);
    if (!m_job.haveOriginal)
        files.append({chinese ? QStringLiteral("zh")
                              : (code.match(m_job.language).hasMatch() ? m_job.language : QStringLiteral("und")),
                      subs::toSrt(m_job.cues, false)});
    if (!chinese && m_job.translated > 0)
        files.append({QStringLiteral("zh"), subs::toSrt(m_job.cues, true)});
    if (files.isEmpty()) {
        genFinish(QString());
        return;
    }
    genProgress(tr("儲存到 Stash"), 1);

    const quint64 jobGen = m_jobGen;
    auto upload = std::make_shared<std::function<void(int)>>();
    *upload = [this, files, jobGen, upload](int i) {
        if (jobGen != m_jobGen)
            return;
        if (i >= files.size()) {
            genFinish(QString());
            return;
        }
        const QJsonObject args{{QStringLiteral("op"), QStringLiteral("save")},
                               {QStringLiteral("scene_id"), m_job.sceneId},
                               {QStringLiteral("lang"), files[i].first},
                               {QStringLiteral("srt"), files[i].second}};
        stashGql(QStringLiteral("mutation($a: Map) { runPluginOperation(plugin_id: \"StashNativePlayer\", args: $a) }"),
                 {{QStringLiteral("a"), args}}, this, [this, upload, i, jobGen](const QJsonObject &data, const QString &error) {
                     if (jobGen != m_jobGen)
                         return;
                     debugLog(QStringLiteral("save: ") +
                              QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)) + QLatin1Char(' ') +
                              error);
                     if (!error.isEmpty() || data.value(QStringLiteral("runPluginOperation")).isNull()) {
                         genFinish(tr("無法存到 Stash：%1").arg(error.isEmpty() ? tr("外掛沒有回應") : error));
                         return;
                     }
                     (*upload)(i + 1);
                 });
    };
    (*upload)(0);
}

// The scene is done (or failed): untag it, and go on with the next one.
void LiveSubs::genFinish(const QString &error)
{
    const QString sceneId = m_job.sceneId;
    const QString title = m_job.title;
    const int left = m_job.left;
    const QString finishTag = m_job.finishTag;
    const QString finishNote = m_job.finishNote;
    if (m_extract) {
        QProcess *p = m_extract;
        m_extract = nullptr;
        p->kill();
    }
    if (!m_job.wavPath.isEmpty())
        QFile::remove(m_job.wavPath);
    ++m_jobGen;
    m_job = Job{};
    emit genStatusChanged();
    if (sceneId.isEmpty())
        return;

    if (error.isEmpty()) {
        const QString done = finishNote.isEmpty() ? tr("字幕完成：%1").arg(title) : tr("%1：%2").arg(finishNote, title);
        setGenStatus(left > 1 ? tr("%1（佇列還有 %2 部）").arg(done).arg(left - 1) : done);
        // The playing scene: switch to the new captions.
        if (sceneId == m_sceneId && m_mode != 0 && m_source.isEmpty())
            startLive();
    } else {
        // (It is untagged below, so tagging it again retries it.)
        setGenStatus(tr("字幕產生失敗：%1 — %2").arg(title, error));
    }
    debugLog(QStringLiteral("generate scene %1 done: %2").arg(sceneId, error.isEmpty() ? QStringLiteral("ok") : error));

    auto next = [this] {
        QTimer::singleShot(500, this, [this] {
            checkQueue();
            stopServersIfIdle();
        });
    };
    const QString queueTag = m_queueTagId;
    auto untag = [this, sceneId, queueTag, next](const QString &failedTag) {
        QJsonObject vars{{QStringLiteral("ids"), QJsonArray{sceneId}}, {QStringLiteral("t"), QJsonArray{queueTag}}};
        stashGql(QStringLiteral("mutation($ids: [ID!], $t: [ID!]) { bulkSceneUpdate(input: {ids: $ids, "
                                "tag_ids: {ids: $t, mode: REMOVE}}) { id } }"),
                 vars, this, [this, sceneId, failedTag, next](const QJsonObject &, const QString &err) {
                     if (!err.isEmpty())
                         m_failedThisRun << sceneId; // could not untag: don't loop on it
                     if (failedTag.isEmpty()) {
                         next();
                         return;
                     }
                     stashGql(QStringLiteral("mutation($ids: [ID!], $t: [ID!]) { bulkSceneUpdate(input: {ids: $ids, "
                                             "tag_ids: {ids: $t, mode: ADD}}) { id } }"),
                              {{QStringLiteral("ids"), QJsonArray{sceneId}}, {QStringLiteral("t"), QJsonArray{failedTag}}},
                              this, [next](const QJsonObject &, const QString &) { next(); });
                 });
    };
    if (queueTag.isEmpty()) {
        next();
    } else if (error.isEmpty() && !finishTag.isEmpty()) {
        findOrCreateTag(finishTag, true, untag); // so "all without subtitles" skips it
    } else if (error.isEmpty()) {
        untag(QString());
    } else {
        findOrCreateTag(QString::fromUtf8(kFailedTag), true, untag);
    }
}

void LiveSubs::genProgress(const QString &step, double fraction)
{
    QString s = tr("產生字幕：%1 — %2 %3%").arg(m_job.title, step).arg(qBound(0, int(fraction * 100), 100));
    if (m_job.left > 1)
        s += tr("（佇列還有 %1 部）").arg(m_job.left - 1);
    setGenStatus(s);
}

// --- "all scenes without subtitles" -------------------------------------------------

// Scenes that have a video file, no captions at all, and none of the queue /
// failed / done-without-subtitles (no speech, embedded, skipped) tags.
void LiveSubs::findUncaptioned(bool ids, std::function<void(const QJsonObject &, const QString &)> done)
{
    if (m_server.isEmpty()) {
        done({}, tr("沒有連線的 Stash 伺服器"));
        return;
    }
    findOrCreateTag(QString::fromUtf8(kQueueTag), false, [this, ids, done](const QString &queueTag) {
        findOrCreateTag(QString::fromUtf8(kFailedTag), false, [this, ids, done, queueTag](const QString &failedTag) {
          // (one lookup for the three "done without subtitles" tags)
          stashGql(QStringLiteral("{ findTags(tag_filter: {name: {value: \"^(無對白|內嵌字幕|略過字幕|畫面字幕)$\", "
                                  "modifier: MATCHES_REGEX}}, filter: {per_page: -1}) { tags { id } } }"),
                   {}, this, [this, ids, done, queueTag, failedTag](const QJsonObject &tagData, const QString &) {
            QJsonArray exclude;
            for (const QString &t : {queueTag, failedTag})
                if (!t.isEmpty())
                    exclude.append(t);
            for (const QJsonValue &t : tagData.value(QStringLiteral("findTags")).toObject().value(QStringLiteral("tags")).toArray())
                exclude.append(t.toObject().value(QStringLiteral("id")).toString());
            QJsonObject filter{
                {QStringLiteral("captions"),
                 QJsonObject{{QStringLiteral("value"), QString()}, {QStringLiteral("modifier"), QStringLiteral("IS_NULL")}}},
                {QStringLiteral("file_count"),
                 QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("modifier"), QStringLiteral("GREATER_THAN")}}}};
            if (!exclude.isEmpty())
                filter.insert(QStringLiteral("tags"), QJsonObject{{QStringLiteral("value"), exclude},
                                                                  {QStringLiteral("modifier"), QStringLiteral("EXCLUDES")}});
            const QString query =
                ids ? QStringLiteral("query($f: SceneFilterType) { findScenes(scene_filter: $f, filter: {per_page: -1}) "
                                     "{ count duration scenes { id } } }")
                    : QStringLiteral("query($f: SceneFilterType) { findScenes(scene_filter: $f, filter: {per_page: 0}) "
                                     "{ count duration } }");
            stashGql(query, {{QStringLiteral("f"), filter}}, this, [done](const QJsonObject &data, const QString &error) {
                done(data.value(QStringLiteral("findScenes")).toObject(), error);
            });
          });
        });
    });
}

void LiveSubs::countUncaptioned()
{
    findUncaptioned(false, [this](const QJsonObject &found, const QString &error) {
        if (!error.isEmpty())
            emit uncaptionedCounted(-1, 0, error);
        else
            emit uncaptionedCounted(found.value(QStringLiteral("count")).toInt(),
                                    found.value(QStringLiteral("duration")).toDouble(), QString());
    });
}

void LiveSubs::queueAllUncaptioned()
{
    setGenStatus(tr("加入字幕佇列…"));
    findUncaptioned(true, [this](const QJsonObject &found, const QString &error) {
        if (!error.isEmpty()) {
            setGenStatus(tr("加入字幕佇列失敗：%1").arg(error));
            return;
        }
        QJsonArray ids;
        for (const QJsonValue &v : found.value(QStringLiteral("scenes")).toArray())
            ids.append(v.toObject().value(QStringLiteral("id")).toString());
        if (ids.isEmpty()) {
            setGenStatus(tr("沒有需要加入的影片"));
            return;
        }
        findOrCreateTag(QString::fromUtf8(kQueueTag), true, [this, ids](const QString &tagId) {
            if (tagId.isEmpty()) {
                setGenStatus(tr("無法建立「%1」標籤").arg(QString::fromUtf8(kQueueTag)));
                return;
            }
            m_queueTagId = tagId;
            // Tag them in chunks, one request after the other.
            auto step = std::make_shared<std::function<void(int)>>();
            *step = [this, ids, tagId, step](int from) {
                if (from >= ids.size()) {
                    m_failedThisRun.clear();
                    if (!genActive())
                        setGenStatus(tr("已把 %1 部影片加入字幕佇列").arg(ids.size()));
                    checkQueue();
                    return;
                }
                QJsonArray chunk;
                for (int i = from; i < qMin<int>(from + 200, ids.size()); ++i)
                    chunk.append(ids[i]);
                stashGql(QStringLiteral("mutation($ids: [ID!], $t: [ID!]) { bulkSceneUpdate(input: {ids: $ids, "
                                        "tag_ids: {ids: $t, mode: ADD}}) { id } }"),
                         {{QStringLiteral("ids"), chunk}, {QStringLiteral("t"), QJsonArray{tagId}}}, this,
                         [this, step, from, ids](const QJsonObject &, const QString &err) {
                             if (!err.isEmpty()) {
                                 setGenStatus(tr("加入字幕佇列失敗（已加入 %1 部）：%2").arg(from).arg(err));
                                 return;
                             }
                             if (!genActive())
                                 setGenStatus(tr("加入字幕佇列… %1/%2").arg(qMin<int>(from + 200, ids.size())).arg(ids.size()));
                             (*step)(from + 200);
                         });
            };
            (*step)(0);
        });
    });
}
