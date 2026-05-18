// Copyright (c) 2026 yasukioo
// Author: yasukioo <yasukioo@outlook.com>

#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QThread>
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

class FFmpegPlayer : public QObject
{
    Q_OBJECT

public:
    explicit FFmpegPlayer(QObject* parent = nullptr);
    ~FFmpegPlayer();

    void setUrl(const QString& url);
    void setRtspTransport(const QString& transport);
    void setTimeoutUs(int value);
    void setSocketBufferSize(int value);
    void setMaxDelayUs(int value);
    void setProbeSize(int value);
    void setAnalyzeDurationUs(qint64 value);
    void setReconnectPauseMs(int value);
    void setTransientReadErrorLimit(int value);
    void setTransientDecodeErrorLimit(int value);
    void setDecodeThreadCount(int value);
    void setUseNoBuffer(bool value);
    void setUseLowDelay(bool value);
    void setDisableReorderQueue(bool value);
    void setSkipNonRefFrames(bool value);
    void start();
    void stop();
    QImage takeNextFrame(bool& moreAvailable);
    QString tracerId() const { return tracerId_; }
    qint64 lastEmitMonoMs() const { return lastEmitMonoMs_.load(std::memory_order_acquire); }

signals:
    void frameReady();
    void errorOccurred(const QString& err);
    void connectedChanged(bool connected);

private:
    struct FrameBuffer {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
        int bytesPerLine = 0;
    };

    class FrameBufferPool : public std::enable_shared_from_this<FrameBufferPool>
    {
    public:
        static std::shared_ptr<FrameBufferPool> create();
        std::shared_ptr<FrameBuffer> acquire(int width, int height, int bytesPerLine);

    private:
        FrameBufferPool() = default;
        void recycle(FrameBuffer* buf);

        QMutex mutex_;
        std::vector<std::unique_ptr<FrameBuffer>> free_;
    };

    struct RuntimeConfig {
        QString rtspTransport = QStringLiteral("tcp");
        int timeoutUs = 1000000;
        int socketBufferSize = 1048576;
        int maxDelayUs = 200000;
        int probeSize = 1024 * 1024;
        qint64 analyzeDurationUs = 500000;
        int reconnectPauseMs = 150;
        int transientReadErrorLimit = 5;
        int transientDecodeErrorLimit = 20;
        int decodeThreadCount = 0;
        bool useNoBuffer = true;
        bool useLowDelay = true;
        bool disableReorderQueue = true;
        bool skipNonRefFrames = false;
    };

    void run();
    void sleepInterruptible(int ms);
    static int interruptCallback(void* ctx);

    bool openStream(int& videoStream);
    void queueFrame(QImage&& frame);
    void cleanupResources();
    void requestKeyFrame();
    bool isFrameCorrupt(const AVFrame* f) const;

    QString rtsp_url_;
    QString tracerId_;

    QThread* thread_;
    std::atomic_bool running_;

    AVFormatContext* fmt_ctx_;
    AVCodecContext* codec_ctx_;
    const AVCodec* codec_;
    AVPacket* packet_;
    AVFrame* frame_;
    SwsContext* sws_ctx_;

    static constexpr int kMaxQueueDepth = 3;

    QMutex frameMutex_;
    std::deque<QImage> frameQueue_;
    std::atomic<qint64> lastEmitMonoMs_{0};
    bool frameNotificationPending_ = false;
    bool awaitingKeyFrame_ = true;
    RuntimeConfig config_;
    std::shared_ptr<FrameBufferPool> bufferPool_;
};
