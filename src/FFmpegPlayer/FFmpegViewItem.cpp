// Copyright (c) 2026 yasukioo
// Author: yasukioo <yasukioo@outlook.com>
 
#include "FFmpegViewItem.h"
#include "FFmpegPlayer.h"
#include "PlaybackTracer.h"

#include <QImage>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTimer>

FFmpegViewItem::FFmpegViewItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);

    updateRenderInterval();
    player_ = new FFmpegPlayer();
    applyPlayerConfig();

    connect(player_, &FFmpegPlayer::frameReady,
        this, &FFmpegViewItem::onFrameReady,
        Qt::QueuedConnection);

    connect(player_, &FFmpegPlayer::errorOccurred,
        this, &FFmpegViewItem::onErrorOccurred,
        Qt::QueuedConnection);

    connect(player_, &FFmpegPlayer::connectedChanged,
        this, &FFmpegViewItem::onConnectedChanged,
        Qt::QueuedConnection);
}

FFmpegViewItem::~FFmpegViewItem()
{
    if (player_) {
        QObject::disconnect(player_, nullptr, this, nullptr);
        player_->stop();
        {
            QMutexLocker locker(&frameMutex_);
            currentFrame_ = QImage();
        }
        player_->deleteLater();
        player_ = nullptr;
    }
}

void FFmpegViewItem::applyPlayerConfig()
{
    if (!player_) {
        return;
    }

    player_->setRtspTransport(rtspTransport_);
    player_->setTimeoutUs(timeoutUs_);
    player_->setSocketBufferSize(socketBufferSize_);
    player_->setMaxDelayUs(maxDelayUs_);
    player_->setProbeSize(probeSize_);
    player_->setAnalyzeDurationUs(analyzeDurationUs_);
    player_->setReconnectPauseMs(reconnectPauseMs_);
    player_->setTransientReadErrorLimit(transientReadErrorLimit_);
    player_->setTransientDecodeErrorLimit(transientDecodeErrorLimit_);
    player_->setDecodeThreadCount(decodeThreadCount_);
    player_->setUseNoBuffer(useNoBuffer_);
    player_->setUseLowDelay(useLowDelay_);
    player_->setDisableReorderQueue(disableReorderQueue_);
    player_->setSkipNonRefFrames(skipNonRefFrames_);
}

void FFmpegViewItem::updateRenderInterval()
{
    targetFrameIntervalMs_ = std::max(1, 1000 / std::max(renderFps_, 1));
}

void FFmpegViewItem::restartPlayback()
{
    updateRenderInterval();

    {
        QMutexLocker locker(&frameMutex_);
        currentFrame_ = QImage();
    }

    state_ = PlayState::Loading;
    update();

    if (player_ != nullptr) {
        applyPlayerConfig();
        player_->stop();
        if (!url_.isEmpty()) {
            player_->start();
        }
    }
}

QSGNode* FFmpegViewItem::updatePaintNode(QSGNode* previous, QQuickItem::UpdatePaintNodeData*)
{
    if (!window()) {
        return nullptr;
    }

    QImage frame;
    {
        QMutexLocker locker(&frameMutex_);
        frame = currentFrame_;
    }

    if (frame.isNull()) {
        delete previous;
        return createTextNode();
    }

    if (player_) {
        PlaybackTracer::instance().mark(player_->tracerId(), "PAINT");
    }

    auto node = static_cast<QSGSimpleTextureNode*>(previous);
    if (node == nullptr) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }

    auto texture = window()->createTextureFromImage(frame);
    node->setTexture(texture);
    node->setRect(boundingRect());

    return node;
}

QSGNode* FFmpegViewItem::createTextNode()
{
    QImage textImage(boundingRect().size().toSize(), QImage::Format_RGBA8888);
    textImage.fill(Qt::transparent);

    QPainter painter(&textImage);
    painter.setPen(QPen(Qt::white));
    painter.setFont(QFont("Microsoft YaHei", 12));

    if (state_ == PlayState::Loading) {
        painter.drawText(textImage.rect(), Qt::AlignCenter, QStringLiteral("加载中..."));
    } else if (state_ == PlayState::Failed) {
        painter.setPen(QPen(Qt::red));
        painter.drawText(textImage.rect(), Qt::AlignCenter, QStringLiteral("无法加载视频"));
    }

    auto texture = window()->createTextureFromImage(textImage);
    auto node = new QSGSimpleTextureNode();
    node->setTexture(texture);
    node->setOwnsTexture(true);
    node->setRect(boundingRect());

    return node;
}

void FFmpegViewItem::onFrameReady()
{
    if (player_ == nullptr) {
        return;
    }

    const qint64 emitMs = player_->lastEmitMonoMs();
    if (emitMs > 0) {
        const qint64 delay = PlaybackTracer::monoMs() - emitMs;
        if (delay >= 100) {
            PlaybackTracer::instance().event(player_->tracerId(), "DISPATCH_SLOW",
                QStringLiteral("emit_to_take=%1ms").arg(delay));
        }
    }

    consumeNextFrame();
}

void FFmpegViewItem::consumeNextFrame()
{
    if (player_ == nullptr) {
        return;
    }

    bool more = false;
    QImage frame = player_->takeNextFrame(more);
    if (frame.isNull()) {
        return;
    }

    PlaybackTracer::instance().mark(player_->tracerId(), "FRAMEREADY");

    {
        QMutexLocker locker(&frameMutex_);
        currentFrame_ = std::move(frame);
    }

    state_ = PlayState::Playing;
    update();

    if (more) {
        // 队列里还有积压帧，按目标帧率间隔均匀出帧，把网络/UI 抖动平滑掉。
        QTimer::singleShot(targetFrameIntervalMs_, this, &FFmpegViewItem::consumeNextFrame);
    }
}

void FFmpegViewItem::setUrl(const QString& url)
{
    if (url_ != url) {
        url_ = url;
        emit urlChanged();

        if (player_) {
            player_->stop();
            player_->setUrl(url);
            if (!url.isEmpty()) {
                player_->start();
            }
        }
    }
}

void FFmpegViewItem::setRtspTransport(const QString& transport)
{
    const QString normalized = transport.trimmed().toLower();
    const QString next = (normalized == QStringLiteral("tcp")) ? QStringLiteral("tcp") : QStringLiteral("udp");
    if (rtspTransport_ == next) {
        return;
    }
    rtspTransport_ = next;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setTimeoutUs(int value)
{
    value = std::max(value, 100000);
    if (timeoutUs_ == value) return;
    timeoutUs_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setSocketBufferSize(int value)
{
    value = std::max(value, 262144);
    if (socketBufferSize_ == value) return;
    socketBufferSize_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setMaxDelayUs(int value)
{
    value = std::max(value, 0);
    if (maxDelayUs_ == value) return;
    maxDelayUs_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setProbeSize(int value)
{
    value = std::max(value, 32768);
    if (probeSize_ == value) return;
    probeSize_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setAnalyzeDurationUs(qint64 value)
{
    value = std::max<qint64>(value, 0);
    if (analyzeDurationUs_ == value) return;
    analyzeDurationUs_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setReconnectPauseMs(int value)
{
    value = std::max(value, 0);
    if (reconnectPauseMs_ == value) return;
    reconnectPauseMs_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setTransientReadErrorLimit(int value)
{
    value = std::max(value, 1);
    if (transientReadErrorLimit_ == value) return;
    transientReadErrorLimit_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setTransientDecodeErrorLimit(int value)
{
    value = std::max(value, 1);
    if (transientDecodeErrorLimit_ == value) return;
    transientDecodeErrorLimit_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setDecodeThreadCount(int value)
{
    value = std::max(value, 0);
    if (decodeThreadCount_ == value) return;
    decodeThreadCount_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setUseNoBuffer(bool value)
{
    if (useNoBuffer_ == value) return;
    useNoBuffer_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setUseLowDelay(bool value)
{
    if (useLowDelay_ == value) return;
    useLowDelay_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setDisableReorderQueue(bool value)
{
    if (disableReorderQueue_ == value) return;
    disableReorderQueue_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setSkipNonRefFrames(bool value)
{
    if (skipNonRefFrames_ == value) return;
    skipNonRefFrames_ = value;
    emit configChanged();
    restartPlayback();
}

void FFmpegViewItem::setRenderFps(int value)
{
    value = std::max(value, 1);
    if (renderFps_ == value) return;
    renderFps_ = value;
    emit configChanged();
    updateRenderInterval();
}

void FFmpegViewItem::onErrorOccurred(const QString& error)
{
    errorMessage_ = error;
    state_ = PlayState::Failed;
    update();
}

void FFmpegViewItem::onConnectedChanged(bool connected)
{
    if (connected) {
        if (state_ == PlayState::Failed) {
            errorMessage_.clear();
        }
        state_ = PlayState::Loading;
    } else {
        if (state_ != PlayState::Failed) {
            state_ = PlayState::Loading;
        }
    }
    update();
}
