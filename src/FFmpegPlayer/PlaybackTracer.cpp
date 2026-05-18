#include "PlaybackTracer.h"

#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <chrono>

extern "C" {
#include <libavutil/log.h>
}

PlaybackTracer& PlaybackTracer::instance()
{
    static PlaybackTracer tracer;
    return tracer;
}

qint64 PlaybackTracer::monoMs()
{
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - origin).count();
}

void PlaybackTracer::start(const QString& dir, qint64 stallThresholdMs)
{
    QMutexLocker locker(&mutex_);
    if (enabled_.load(std::memory_order_acquire)) {
        return;
    }

    QDir().mkpath(dir);
    const QString filename = QStringLiteral("%1/ffmpeg_trace_%2.log")
        .arg(dir, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    file_.setFileName(filename);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    stream_.setDevice(&file_);
    stream_.setEncoding(QStringConverter::Utf8);
    stallThresholdMs_ = stallThresholdMs;
    mono_.start();
    enabled_.store(true, std::memory_order_release);

    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(&PlaybackTracer::avLogTrampoline);

    stream_ << QStringLiteral("# session start %1, stall threshold %2 ms\n")
                   .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                   .arg(stallThresholdMs_);
    stream_.flush();
}

void PlaybackTracer::stop()
{
    QMutexLocker locker(&mutex_);
    if (!enabled_.load(std::memory_order_acquire)) {
        return;
    }
    enabled_.store(false, std::memory_order_release);
    av_log_set_callback(av_log_default_callback);
    stream_ << QStringLiteral("# session end\n");
    stream_.flush();
    stream_.setDevice(nullptr);
    file_.close();
    lastMs_.clear();
}

void PlaybackTracer::mark(const QString& streamId, const char* stage, const QString& extra)
{
    if (!enabled_.load(std::memory_order_acquire)) {
        return;
    }

    const qint64 nowMs = mono_.elapsed();
    const QString key = streamId + QLatin1Char('|') + QLatin1String(stage);

    qint64 gap = -1;
    {
        QMutexLocker locker(&mutex_);
        auto it = lastMs_.find(key);
        if (it != lastMs_.end()) {
            gap = nowMs - it.value();
        }
        lastMs_.insert(key, nowMs);

        if (gap >= 0 && gap >= stallThresholdMs_) {
            const QString line = QStringLiteral("%1 [%2] STALL %3 gap=%4ms %5\n")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                .arg(streamId)
                .arg(QLatin1String(stage))
                .arg(gap)
                .arg(extra);
            writeLineLocked(line);
        }
    }
}

void PlaybackTracer::event(const QString& streamId, const char* tag, const QString& msg)
{
    if (!enabled_.load(std::memory_order_acquire)) {
        return;
    }
    const QString line = QStringLiteral("%1 [%2] %3 %4\n")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(streamId)
        .arg(QLatin1String(tag))
        .arg(msg);

    QMutexLocker locker(&mutex_);
    writeLineLocked(line);
}

void PlaybackTracer::writeLineLocked(const QString& line)
{
    if (!file_.isOpen()) {
        return;
    }
    stream_ << line;
    stream_.flush();
}

void PlaybackTracer::avLogTrampoline(void* /*ptr*/, int level, const char* fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) {
        return;
    }
    char buf[1024] = {0};
    vsnprintf(buf, sizeof(buf) - 1, fmt, vl);
    QString msg = QString::fromUtf8(buf).trimmed();
    if (msg.isEmpty()) {
        return;
    }
    PlaybackTracer::instance().event(
        QStringLiteral("ffmpeg"), "AVLOG",
        QStringLiteral("lvl=%1 %2").arg(level).arg(msg));
}
