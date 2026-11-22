#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Pieces shared by live subtitles and whole-video subtitle generation.
namespace subs {

constexpr double kWindow = 30.0; // seconds of audio per whisper request
constexpr double kTail = 2.5;    // lines ending this close to a window's end are redone in the next window
constexpr double kMaxCue = 7.0;  // longest line, in seconds
constexpr int kTranslateBatch = 12;

struct Cue {
    double start = 0;
    double end = 0;
    QString text;        // original language
    QString translation; // empty until translated
};

// SNP_DEBUG=1: log to %TEMP%\snp-livesubs.log (UTF-8; the console mangles
// non-Latin text).
void debugLog(const QString &text);

// Turns whisper-server's verbose_json for one window (audio starting at
// windowStart, windowLen seconds long) into subtitle lines on the video's
// timeline. Locks *language on the first confident detection. Sets *advance
// to how far the next window should start after windowStart: lines that may
// be cut off by the window's end are left out, to be redone by that window.
QList<Cue> buildLines(const QJsonObject &whisperResult, double windowStart, double windowLen, bool lastWindow,
                      QString *language, double *advance);

// Body of a llama-server /v1/chat/completions request translating `lines`
// into Traditional Chinese, and the parser for its reply (line number, from
// 1, -> translation).
QByteArray translationRequest(const QStringList &lines);
QHash<int, QString> parseTranslation(const QByteArray &reply);
// Drops junk a small model sometimes emits: Latin words that are not in the
// source line (Qwen2.5-7B writes " Pendant" into some lines), and runaway
// repetition (returns "" then, so the original text is shown instead).
QString cleanTranslation(const QString &source, const QString &translation);

// True if the language needs no translation into Chinese.
bool isChinese(const QString &language);

// SubRip text of the lines (the translations if `translated`, falling back to
// the original text).
QString toSrt(const QList<Cue> &cues, bool translated);
// Parses SubRip or WebVTT (what Stash's /scene/<id>/caption serves).
QList<Cue> parseCaptions(const QByteArray &data);

// A 16 kHz mono 16-bit WAV file holding `pcm`.
QByteArray wavFile(const QByteArray &pcm);

} // namespace subs
