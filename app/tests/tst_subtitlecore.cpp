// Unit tests for src/subtitlecore.cpp (subtitle lines, SRT/VTT, translation
// request/reply handling). Build with -DSNP_BUILD_TESTS=ON, run with ctest.

#include "subtitlecore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>

using subs::Cue;

namespace {

QJsonObject segment(double start, double end, const QString &text)
{
    return QJsonObject{{QStringLiteral("start"), start}, {QStringLiteral("end"), end}, {QStringLiteral("text"), text}};
}

QJsonObject whisperResult(const QJsonArray &segments, double probability = 0.9)
{
    return QJsonObject{
        {QStringLiteral("detected_language_probability"), probability},
        {QStringLiteral("language_probabilities"),
         QJsonObject{{QStringLiteral("en"), 0.9}, {QStringLiteral("ja"), 0.05}}},
        {QStringLiteral("segments"), segments},
    };
}

QByteArray chatReply(const QString &content)
{
    const QJsonObject reply{
        {QStringLiteral("choices"),
         QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}}}}}};
    return QJsonDocument(reply).toJson(QJsonDocument::Compact);
}

} // namespace

class TestSubtitleCore : public QObject
{
    Q_OBJECT

private slots:
    void isChinese()
    {
        QVERIFY(subs::isChinese(QStringLiteral("zh")));
        QVERIFY(subs::isChinese(QStringLiteral("zh-TW")));
        QVERIFY(subs::isChinese(QStringLiteral("yue")));
        QVERIFY(!subs::isChinese(QStringLiteral("en")));
        QVERIFY(!subs::isChinese(QStringLiteral("ja")));
        QVERIFY(!subs::isChinese(QString()));
    }

    void toSrtFormatsTimesAndSkipsEmptyLines()
    {
        const QList<Cue> cues{
            {1.0, 2.5, QStringLiteral("Hello"), QStringLiteral("你好")},
            {3.0, 4.0, QStringLiteral("  "), QString()},
            {3661.5, 3662.25, QStringLiteral("No translation"), QString()},
        };
        QCOMPARE(subs::toSrt(cues, false),
                 QStringLiteral("1\n00:00:01,000 --> 00:00:02,500\nHello\n\n"
                                "2\n01:01:01,500 --> 01:01:02,250\nNo translation\n\n"));
        // translated: uses the translation, falling back to the original text
        QCOMPARE(subs::toSrt(cues, true),
                 QStringLiteral("1\n00:00:01,000 --> 00:00:02,500\n你好\n\n"
                                "2\n01:01:01,500 --> 01:01:02,250\nNo translation\n\n"));
    }

    void srtRoundTrip()
    {
        const QList<Cue> cues{
            {0.5, 1.75, QStringLiteral("First line"), {}},
            {10.0, 12.0, QStringLiteral("第二行"), {}},
        };
        const QList<Cue> back = subs::parseCaptions(subs::toSrt(cues, false).toUtf8());
        QCOMPARE(back.size(), 2);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(back[i].start, cues[i].start);
            QCOMPARE(back[i].end, cues[i].end);
            QCOMPARE(back[i].text, cues[i].text);
        }
    }

    void parseCaptionsWebVtt()
    {
        // BOM, CRLF, header, cue ids, cue settings, tags, multi-line cues,
        // short (mm:ss.mmm) times, out-of-order cues
        const QByteArray vtt = "\xEF\xBB\xBFWEBVTT\r\n\r\n"
                               "2\r\n00:00:05.000 --> 00:00:06.500 align:start\r\n<i>Second</i>\r\nline\r\n\r\n"
                               "1\r\n00:01.000 --> 00:02.000\r\n<c.yellow>First</c>\r\n\r\n"
                               "bad\r\n00:00:09.000 --> 00:00:08.000\r\nend before start\r\n";
        const QList<Cue> cues = subs::parseCaptions(vtt);
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0].start, 1.0);
        QCOMPARE(cues[0].end, 2.0);
        QCOMPARE(cues[0].text, QStringLiteral("First"));
        QCOMPARE(cues[1].start, 5.0);
        QCOMPARE(cues[1].end, 6.5);
        QCOMPARE(cues[1].text, QStringLiteral("Second line"));
    }

    void parseCaptionsIgnoresGarbage()
    {
        QVERIFY(subs::parseCaptions(QByteArray()).isEmpty());
        QVERIFY(subs::parseCaptions("just some text\nwith no times").isEmpty());
    }

    void buildLinesMergesAndTrimsTail()
    {
        const QJsonArray segments{
            segment(0.0, 2.0, QStringLiteral(" Hello there.")),
            segment(2.2, 3.0, QStringLiteral(" [Music]")),  // noise
            segment(3.1, 5.0, QStringLiteral(" How are")),  // merged with the next piece
            segment(5.2, 6.0, QStringLiteral(" you?")),
            segment(28.0, 29.5, QStringLiteral(" Cut off")), // runs into the window's tail
        };
        QString language;
        double advance = 0;
        const QList<Cue> cues = subs::buildLines(whisperResult(segments), 100.0, 30.0, false, &language, &advance);
        QCOMPARE(language, QStringLiteral("en"));
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0].start, 100.0);
        QCOMPARE(cues[0].end, 102.0);
        QCOMPARE(cues[0].text, QStringLiteral("Hello there."));
        QCOMPARE(cues[1].start, 103.1);
        QCOMPARE(cues[1].end, 106.0);
        QCOMPARE(cues[1].text, QStringLiteral("How are you?"));
        QCOMPARE(advance, 6.0); // the next window starts after the last kept line
    }

    void buildLinesLastWindowKeepsEverything()
    {
        const QJsonArray segments{
            segment(0.0, 2.0, QStringLiteral(" Hello there.")),
            segment(28.0, 29.5, QStringLiteral(" The end")),
        };
        QString language;
        double advance = 0;
        const QList<Cue> cues = subs::buildLines(whisperResult(segments), 0.0, 30.0, true, &language, &advance);
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[1].text, QStringLiteral("The end"));
        QCOMPARE(advance, 30.0);
    }

    void buildLinesLanguageLock()
    {
        const QJsonArray segments{segment(0.0, 2.0, QStringLiteral(" Hi."))};
        double advance = 0;
        QString language;
        subs::buildLines(whisperResult(segments, 0.3), 0.0, 30.0, true, &language, &advance);
        QVERIFY(language.isEmpty()); // not confident enough
        language = QStringLiteral("ja");
        subs::buildLines(whisperResult(segments), 0.0, 30.0, true, &language, &advance);
        QCOMPARE(language, QStringLiteral("ja")); // stays locked
    }

    void buildLinesUsesWordTimes()
    {
        // word times are stretched onto the segment's own span
        QJsonObject seg = segment(10.0, 14.0, QStringLiteral(" One. Two."));
        seg.insert(QStringLiteral("words"),
                   QJsonArray{
                       QJsonObject{{QStringLiteral("word"), QStringLiteral("[_BEG_]")},
                                   {QStringLiteral("start"), 0.0},
                                   {QStringLiteral("end"), 0.0}},
                       QJsonObject{{QStringLiteral("word"), QStringLiteral(" One.")},
                                   {QStringLiteral("start"), 1.0},
                                   {QStringLiteral("end"), 2.0}},
                       QJsonObject{{QStringLiteral("word"), QStringLiteral(" Two.")},
                                   {QStringLiteral("start"), 2.0},
                                   {QStringLiteral("end"), 3.0}},
                   });
        QString language = QStringLiteral("en");
        double advance = 0;
        const QList<Cue> cues = subs::buildLines(whisperResult({seg}), 0.0, 30.0, true, &language, &advance);
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0].text, QStringLiteral("One."));
        QCOMPARE(cues[0].start, 10.0);
        QCOMPARE(cues[0].end, 12.0);
        QCOMPARE(cues[1].text, QStringLiteral("Two."));
        QCOMPARE(cues[1].start, 12.0);
        QCOMPARE(cues[1].end, 14.0);
    }

    void translationRequestSchema()
    {
        const QJsonObject body =
            QJsonDocument::fromJson(subs::translationRequest({QStringLiteral("a"), QStringLiteral("b")})).object();
        const QJsonObject schema = body.value(QStringLiteral("response_format"))
                                       .toObject()
                                       .value(QStringLiteral("json_schema"))
                                       .toObject()
                                       .value(QStringLiteral("schema"))
                                       .toObject();
        QCOMPARE(schema.value(QStringLiteral("required")).toArray(),
                 (QJsonArray{QStringLiteral("1"), QStringLiteral("2")}));
        QCOMPARE(schema.value(QStringLiteral("additionalProperties")).toBool(true), false);
        const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 2);
        const QJsonObject user =
            QJsonDocument::fromJson(messages[1].toObject().value(QStringLiteral("content")).toString().toUtf8())
                .object();
        QCOMPARE(user.value(QStringLiteral("1")).toString(), QStringLiteral("a"));
        QCOMPARE(user.value(QStringLiteral("2")).toString(), QStringLiteral("b"));
    }

    void parseTranslationReply()
    {
        const auto lines = subs::parseTranslation(chatReply(QStringLiteral("{\"1\": \" 你好 \", \"2\": \"再見\"}")));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.value(1), QStringLiteral("你好"));
        QCOMPARE(lines.value(2), QStringLiteral("再見"));
        QVERIFY(subs::parseTranslation("not json").isEmpty());
        QVERIFY(subs::parseTranslation(chatReply(QStringLiteral("not json either"))).isEmpty());
    }

    void cleanTranslationDropsInventedLatinWords()
    {
        // " Pendant" is not in the source: removed, with the space next to the Chinese
        QCOMPARE(subs::cleanTranslation(QStringLiteral("I like it."), QStringLiteral("我喜歡 Pendant 它。")),
                 QStringLiteral("我喜歡它。"));
        // names copied from the source stay
        QCOMPARE(subs::cleanTranslation(QStringLiteral("Tom is here."), QStringLiteral("Tom 在這裡。")),
                 QStringLiteral("Tom在這裡。"));
        // not a Chinese translation: left alone
        QCOMPARE(subs::cleanTranslation(QStringLiteral("Bonjour"), QStringLiteral("Hello  there")),
                 QStringLiteral("Hello there"));
    }

    void cleanTranslationRejectsRunawayOutput()
    {
        QCOMPARE(subs::cleanTranslation(QStringLiteral("Hi"), QString(40, QChar(0x54C8))), QString());
        QCOMPARE(subs::cleanTranslation(QStringLiteral("Hi"), QStringLiteral("嗨")), QStringLiteral("嗨"));
    }

    void wavHeader()
    {
        const QByteArray pcm(320, '\x01');
        const QByteArray wav = subs::wavFile(pcm);
        QCOMPARE(wav.size(), 44 + pcm.size());
        QVERIFY(wav.startsWith("RIFF"));
        QCOMPARE(wav.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 4), quint32(36 + pcm.size()));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 24), quint32(16000));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 40), quint32(pcm.size()));
        QCOMPARE(wav.mid(44), pcm);
    }
};

QTEST_GUILESS_MAIN(TestSubtitleCore)
#include "tst_subtitlecore.moc"
