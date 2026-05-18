// Copyright (c) 2026 yasukioo
// Author: yasukioo <yasukioo@outlook.com>

#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickItem>

#include <qqmlintegration.h>

class FFmpegPlayer;

class FFmpegViewItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(QString rtspTransport READ rtspTransport WRITE setRtspTransport NOTIFY configChanged)
    Q_PROPERTY(int timeoutUs READ timeoutUs WRITE setTimeoutUs NOTIFY configChanged)
    Q_PROPERTY(int socketBufferSize READ socketBufferSize WRITE setSocketBufferSize NOTIFY configChanged)
    Q_PROPERTY(int maxDelayUs READ maxDelayUs WRITE setMaxDelayUs NOTIFY configChanged)
    Q_PROPERTY(int probeSize READ probeSize WRITE setProbeSize NOTIFY configChanged)
    Q_PROPERTY(qint64 analyzeDurationUs READ analyzeDurationUs WRITE setAnalyzeDurationUs NOTIFY configChanged)
    Q_PROPERTY(int reconnectPauseMs READ reconnectPauseMs WRITE setReconnectPauseMs NOTIFY configChanged)
    Q_PROPERTY(int transientReadErrorLimit READ transientReadErrorLimit WRITE setTransientReadErrorLimit NOTIFY configChanged)
    Q_PROPERTY(int transientDecodeErrorLimit READ transientDecodeErrorLimit WRITE setTransientDecodeErrorLimit NOTIFY configChanged)
    Q_PROPERTY(int decodeThreadCount READ decodeThreadCount WRITE setDecodeThreadCount NOTIFY configChanged)
    Q_PROPERTY(bool useNoBuffer READ useNoBuffer WRITE setUseNoBuffer NOTIFY configChanged)
    Q_PROPERTY(bool useLowDelay READ useLowDelay WRITE setUseLowDelay NOTIFY configChanged)
    Q_PROPERTY(bool disableReorderQueue READ disableReorderQueue WRITE setDisableReorderQueue NOTIFY configChanged)
    Q_PROPERTY(bool skipNonRefFrames READ skipNonRefFrames WRITE setSkipNonRefFrames NOTIFY configChanged)
    Q_PROPERTY(int renderFps READ renderFps WRITE setRenderFps NOTIFY configChanged)
    QML_ELEMENT

public:
    explicit FFmpegViewItem(QQuickItem* parent = nullptr);
    ~FFmpegViewItem();

    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;

    QString url() const { return url_; }
    void setUrl(const QString& url);
    QString rtspTransport() const { return rtspTransport_; }
    void setRtspTransport(const QString& transport);
    int timeoutUs() const { return timeoutUs_; }
    void setTimeoutUs(int value);
    int socketBufferSize() const { return socketBufferSize_; }
    void setSocketBufferSize(int value);
    int maxDelayUs() const { return maxDelayUs_; }
    void setMaxDelayUs(int value);
    int probeSize() const { return probeSize_; }
    void setProbeSize(int value);
    qint64 analyzeDurationUs() const { return analyzeDurationUs_; }
    void setAnalyzeDurationUs(qint64 value);
    int reconnectPauseMs() const { return reconnectPauseMs_; }
    void setReconnectPauseMs(int value);
    int transientReadErrorLimit() const { return transientReadErrorLimit_; }
    void setTransientReadErrorLimit(int value);
    int transientDecodeErrorLimit() const { return transientDecodeErrorLimit_; }
    void setTransientDecodeErrorLimit(int value);
    int decodeThreadCount() const { return decodeThreadCount_; }
    void setDecodeThreadCount(int value);
    bool useNoBuffer() const { return useNoBuffer_; }
    void setUseNoBuffer(bool value);
    bool useLowDelay() const { return useLowDelay_; }
    void setUseLowDelay(bool value);
    bool disableReorderQueue() const { return disableReorderQueue_; }
    void setDisableReorderQueue(bool value);
    bool skipNonRefFrames() const { return skipNonRefFrames_; }
    void setSkipNonRefFrames(bool value);
    int renderFps() const { return renderFps_; }
    void setRenderFps(int value);

signals:
    void urlChanged();
    void configChanged();

private slots:
    void onFrameReady();
    void onErrorOccurred(const QString& error);
    void onConnectedChanged(bool connected);

private:
    QSGNode* createTextNode();
    void applyPlayerConfig();
    void updateRenderInterval();
    void restartPlayback();
    void consumeNextFrame();

    enum class PlayState {
        Loading,
        Playing,
        Failed
    };

    PlayState state_ = PlayState::Loading;
    QString errorMessage_;

    QImage currentFrame_;
    QMutex frameMutex_;

    FFmpegPlayer* player_ = nullptr;
    QString url_;
    int targetFrameIntervalMs_ = 33;
    QString rtspTransport_ = QStringLiteral("udp");
    int timeoutUs_ = 1000000;
    int socketBufferSize_ = 1048576;
    int maxDelayUs_ = 200000;
    int probeSize_ = 1048576;
    qint64 analyzeDurationUs_ = 500000;
    int reconnectPauseMs_ = 150;
    int transientReadErrorLimit_ = 5;
    int transientDecodeErrorLimit_ = 20;
    int decodeThreadCount_ = 0;
    bool useNoBuffer_ = true;
    bool useLowDelay_ = true;
    bool disableReorderQueue_ = true;
    bool skipNonRefFrames_ = false;
    int renderFps_ = 30;
};
