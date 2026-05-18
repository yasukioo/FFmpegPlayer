// Copyright (c) 2026 yasukioo
// Author: yasukioo <yasukioo@outlook.com>

#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QTextStream>
#include <atomic>
#include <cstdarg>

class PlaybackTracer
{
public:
    static PlaybackTracer& instance();
    static qint64 monoMs();

    void start(const QString& dir, qint64 stallThresholdMs);
    void stop();

    bool enabled() const { return enabled_.load(std::memory_order_acquire); }

    // Record a stage event. Emits a STALL line if the gap since the last call
    // with the same (streamId, stage) exceeds the configured threshold.
    void mark(const QString& streamId, const char* stage, const QString& extra = {});

    // Always log; not gated by the stall threshold.
    void event(const QString& streamId, const char* tag, const QString& msg);

private:
    PlaybackTracer() = default;
    void writeLineLocked(const QString& line);
    static void avLogTrampoline(void* ptr, int level, const char* fmt, va_list vl);

    std::atomic_bool enabled_{false};
    QMutex mutex_;
    QFile file_;
    QTextStream stream_;
    qint64 stallThresholdMs_ = 300;
    QElapsedTimer mono_;
    QHash<QString, qint64> lastMs_;
};
