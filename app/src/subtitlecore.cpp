#include "subtitlecore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTime>
#include <QtEndian>

#include <algorithm>

namespace subs {

namespace {

bool endsSentence(const QString &s)
{
    static const QString enders = QStringLiteral(".?!。？！…♪\"」』");
    const QString t = s.trimmed();
    return !t.isEmpty() && enders.contains(t.back());
}

bool isNoise(const QString &s)
{
    // "[Music]", "(laughs)", "♪", "【拍手】" ... and lines with no letters at all
    static const QRegularExpression bracketed(QStringLiteral("^[\\[\\(（【♪*].*[\\]\\)）】♪*]$"));
    static const QRegularExpression letters(QStringLiteral("[\\p{L}\\p{N}]"));
    const QString t = s.trimmed();
    return t.isEmpty() || bracketed.match(t).hasMatch() || !letters.match(t).hasMatch();
}

QString srtTime(double t)
{
    const qint64 ms = qMax<qint64>(0, qRound64(t * 1000));
    return QStringLiteral("%1:%2:%3,%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

// "01:02:03,456", "01:02:03.456" or "02:03.456" -> seconds; -1 if invalid
double parseTime(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^(?:(\\d+):)?(\\d{1,2}):(\\d{1,2})[.,](\\d{1,3})$"));
    const auto m = re.match(s.trimmed());
    if (!m.hasMatch())
        return -1;
    const QString frac = m.captured(4).leftJustified(3, QLatin1Char('0'));
    return m.captured(1).toDouble() * 3600 + m.captured(2).toDouble() * 60 + m.captured(3).toDouble() +
           frac.toDouble() / 1000.0;
}

} // namespace

void debugLog(const QString &text)
{
    if (!qEnvironmentVariableIsSet("SNP_DEBUG"))
        return;
    QFile f(QDir::temp().filePath(QStringLiteral("snp-livesubs.log")));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + text + QStringLiteral("\n")).toUtf8());
}

bool isChinese(const QString &language)
{
    return language.startsWith(QLatin1String("zh")) || language == QLatin1String("yue");
}

QList<Cue> buildLines(const QJsonObject &root, double windowStart, double windowLen, bool lastWindow,
                      QString *language, double *advance)
{
    // Lock the language after the first confident detection, so that it does
    // not flip between windows.
    if (language->isEmpty() && root.value(QStringLiteral("detected_language_probability")).toDouble() > 0.6) {
        const QJsonObject probs = root.value(QStringLiteral("language_probabilities")).toObject();
        double best = 0;
        for (auto it = probs.begin(); it != probs.end(); ++it)
            if (it.value().toDouble() > best) {
                best = it.value().toDouble();
                *language = it.key();
            }
    }

    // Whisper's segments often start or end mid-sentence, which reads badly and
    // makes the translator re-split lines. Rebuild lines that end at sentence
    // ends, pauses or kMaxCue seconds: from the word timestamps where words
    // are real words, otherwise by merging segments. (In Chinese, Japanese and
    // Korean whisper's "words" are raw tokens that can split a character.)
    struct Piece {
        double start, end;
        QString text;
    };
    QList<Piece> pieces;
    const bool cjk = QStringList{QStringLiteral("ja"), QStringLiteral("zh"), QStringLiteral("ko"), QStringLiteral("yue")}
                         .contains(*language);
    for (const QJsonValue &v : root.value(QStringLiteral("segments")).toArray()) {
        const QJsonObject s = v.toObject();
        const QString text = s.value(QStringLiteral("text")).toString();
        if (isNoise(text))
            continue;
        const QJsonArray words = s.value(QStringLiteral("words")).toArray();
        if (cjk || words.isEmpty()) {
            pieces.append({s.value(QStringLiteral("start")).toDouble(), s.value(QStringLiteral("end")).toDouble(), text});
            continue;
        }
        // With VAD on, whisper maps segment times back to the real timeline
        // but leaves word times on the VAD-shortened one, so they drift by
        // seconds. Stretch each segment's words onto the segment's own span.
        QList<Piece> segWords;
        for (const QJsonValue &w : words) {
            const QJsonObject o = w.toObject();
            const QString word = o.value(QStringLiteral("word")).toString();
            if (word.trimmed().isEmpty() || word.trimmed().startsWith(QLatin1String("[_")))
                continue; // special tokens such as [_BEG_]
            segWords.append({o.value(QStringLiteral("start")).toDouble(), o.value(QStringLiteral("end")).toDouble(), word});
        }
        if (segWords.isEmpty())
            continue;
        const double segStart = s.value(QStringLiteral("start")).toDouble();
        const double segEnd = s.value(QStringLiteral("end")).toDouble();
        const double w0 = segWords.first().start, w1 = segWords.last().end;
        const double scale = w1 > w0 ? (segEnd - segStart) / (w1 - w0) : 0;
        for (Piece &p : segWords) {
            p.start = segStart + (p.start - w0) * scale;
            p.end = segStart + (p.end - w0) * scale;
            pieces.append(p);
        }
    }
    QList<Cue> cues;
    for (const Piece &p : std::as_const(pieces)) {
        const double end = qMax(p.end, p.start + 0.05);
        if (!cues.isEmpty()) {
            Cue &c = cues.last();
            if (p.start - c.end < 0.8 && end - c.start <= kMaxCue && !endsSentence(c.text)) {
                c.text += p.text;
                c.end = end;
                continue;
            }
        }
        cues.append(Cue{p.start, end, p.text, {}});
    }
    cues.erase(std::remove_if(cues.begin(), cues.end(), [](const Cue &c) { return isNoise(c.text); }), cues.end());

    // Lines that run into the end of the window may be cut off; redo them as
    // part of the next window.
    double adv = windowLen;
    if (!lastWindow) {
        while (!cues.isEmpty() && cues.last().end > windowLen - kTail)
            cues.removeLast();
        // A last line that stops mid-sentence is also redone (whole) next time.
        if (!cjk && cues.size() > 1 && !endsSentence(cues.last().text))
            cues.removeLast();
        adv = cues.isEmpty() ? windowLen - kTail : cues.last().end;
        adv = qMax(adv, 5.0);
    }
    *advance = adv;

    for (Cue &c : cues) {
        c.start += windowStart;
        c.end += windowStart;
        c.text = c.text.simplified();
        debugLog(QStringLiteral("line %1-%2 %3").arg(c.start, 0, 'f', 2).arg(c.end, 0, 'f', 2).arg(c.text));
    }
    return cues;
}

QByteArray translationRequest(const QStringList &lines)
{
    // Lines go in as a JSON object {"1": "...", "2": "..."}; a JSON schema that
    // requires exactly the same keys keeps the model from merging, dropping or
    // re-splitting lines (which would shift every later translation).
    QJsonObject input, props;
    QJsonArray required;
    for (int n = 0; n < lines.size(); ++n) {
        const QString key = QString::number(n + 1);
        input.insert(key, lines[n]);
        props.insert(key, QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        required.append(key);
    }
    const QString user = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
    const QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                             {QStringLiteral("properties"), props},
                             {QStringLiteral("required"), required},
                             {QStringLiteral("additionalProperties"), false}};
    const QString system = QStringLiteral(
        "You translate video subtitles into Traditional Chinese as used in Taiwan (繁體中文, 台灣用語). "
        "The input is a JSON object of numbered subtitle lines in playback order. Reply with a JSON object with "
        "the same keys, where each value is the translation of that same line only (use the neighbouring lines "
        "only as context). Translate every line, even fragments. Natural, concise subtitle style; never "
        "Simplified Chinese.");
    const QJsonObject body{
        {QStringLiteral("messages"),
         QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), system}},
                    QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), user}}}},
        {QStringLiteral("temperature"), 0.2},
        {QStringLiteral("max_tokens"), 2000},
        {QStringLiteral("cache_prompt"), true},
        {QStringLiteral("response_format"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                     {QStringLiteral("json_schema"), QJsonObject{{QStringLiteral("name"), QStringLiteral("subtitles")},
                                                                 {QStringLiteral("schema"), schema}}}}},
    };
    debugLog(QStringLiteral("translate ") + user);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QHash<int, QString> parseTranslation(const QByteArray &reply)
{
    const QString content = QJsonDocument::fromJson(reply)
                                .object()
                                .value(QStringLiteral("choices"))
                                .toArray()
                                .at(0)
                                .toObject()
                                .value(QStringLiteral("message"))
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
    debugLog(QStringLiteral("translated ") + content);
    QHash<int, QString> lines;
    const QJsonObject out = QJsonDocument::fromJson(content.toUtf8()).object();
    for (auto it = out.begin(); it != out.end(); ++it)
        lines.insert(it.key().toInt(), it.value().toString().trimmed());
    return lines;
}

QString cleanTranslation(const QString &source, const QString &translation)
{
    static const QRegularExpression latinWord(QStringLiteral("[A-Za-z][A-Za-z'-]{2,}"));
    static const QRegularExpression spaceNearCjk(
        QStringLiteral("\\s+(?=[\\x{3000}-\\x{9FFF}\\x{FF00}-\\x{FFEF}])|(?<=[\\x{3000}-\\x{9FFF}\\x{FF00}-\\x{FFEF}])\\s+"));
    QString t = translation;
    // Only for a translation in Chinese: there, Latin words are names and
    // terms copied from the source line.
    static const QRegularExpression han(QStringLiteral("\\p{Han}"));
    if (han.match(t).hasMatch()) {
        QString out;
        qsizetype last = 0;
        auto it = latinWord.globalMatch(t);
        while (it.hasNext()) {
            const auto m = it.next();
            if (source.contains(m.captured(), Qt::CaseInsensitive))
                continue;
            out += t.mid(last, m.capturedStart() - last);
            last = m.capturedEnd();
        }
        out += t.mid(last);
        t = out;
        t.replace(spaceNearCjk, QString());
    }
    t = t.simplified();
    if (t.size() > source.size() * 2 + 20)
        return {}; // runaway output
    return t;
}

QString toSrt(const QList<Cue> &cues, bool translated)
{
    QString out;
    int n = 0;
    for (const Cue &c : cues) {
        const QString text = (translated && !c.translation.isEmpty() ? c.translation : c.text).trimmed();
        if (text.isEmpty())
            continue;
        out += QString::number(++n) + QLatin1Char('\n') + srtTime(c.start) + QStringLiteral(" --> ") + srtTime(c.end) +
               QLatin1Char('\n') + text + QStringLiteral("\n\n");
    }
    return out;
}

QList<Cue> parseCaptions(const QByteArray &data)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>|\\{\\\\[^}]*\\}"));
    QString text = QString::fromUtf8(data);
    text.remove(QChar(0xFEFF));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QList<Cue> cues;
    const QStringList blocks = text.split(QRegularExpression(QStringLiteral("\n\\s*\n")));
    for (const QString &block : blocks) {
        const QStringList lines = block.split(QLatin1Char('\n'));
        int i = 0;
        while (i < lines.size() && !lines[i].contains(QLatin1String("-->")))
            ++i;
        if (i >= lines.size())
            continue;
        const QStringList times = lines[i].split(QStringLiteral("-->"));
        const double start = parseTime(times.value(0));
        // VTT cue settings may follow the end time ("00:00:02.000 align:start")
        const double end = parseTime(times.value(1).trimmed().section(QLatin1Char(' '), 0, 0));
        if (start < 0 || end < start)
            continue;
        QStringList body = lines.mid(i + 1);
        QString t = body.join(QLatin1Char(' '));
        t.remove(tags);
        t = t.simplified();
        if (!t.isEmpty())
            cues.append(Cue{start, end, t, {}});
    }
    std::sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
    return cues;
}

QByteArray wavFile(const QByteArray &pcm)
{
    QByteArray h(44, '\0');
    auto put32 = [&h](int at, quint32 v) { qToLittleEndian(v, h.data() + at); };
    auto put16 = [&h](int at, quint16 v) { qToLittleEndian(v, h.data() + at); };
    memcpy(h.data(), "RIFF", 4);
    put32(4, quint32(36 + pcm.size()));
    memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);     // PCM
    put16(22, 1);     // mono
    put32(24, 16000); // sample rate
    put32(28, 32000); // byte rate
    put16(32, 2);     // block align
    put16(34, 16);    // bits per sample
    memcpy(h.data() + 36, "data", 4);
    put32(40, quint32(pcm.size()));
    return h + pcm;
}

} // namespace subs
