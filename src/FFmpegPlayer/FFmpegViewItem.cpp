// Copyright (c) 2026 yasukioo
// Author: yasukioo <yasukioo@outlook.com>

#include "FFmpegViewItem.h"
#include "FFmpegPlayer.h"

#include <QImage>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

FFmpegViewItem::FFmpegViewItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);

    updateRenderInterval();
    limiter_.start();
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
    shutdownPlayer();
}

void FFmpegViewItem::releaseResources()
{
    {
        QMutexLocker locker(&frameMutex_);
        currentFrame_ = QImage();
    }
    QQuickItem::releaseResources();
}

void FFmpegViewItem::shutdownPlayer()
{
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;

    if (!player_) {
        return;
    }

    QObject::disconnect(player_, nullptr, this, nullptr);
    player_->stop();
    {
        QMutexLocker locker(&frameMutex_);
        currentFrame_ = QImage();
    }
    delete player_;
    player_ = nullptr;
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
}

void FFmpegViewItem::updateRenderInterval()
{
    targetFrameIntervalMs_ = std::max(1, 1000 / std::max(renderFps_, 1));
}

void FFmpegViewItem::restartPlayback()
{
    if (shuttingDown_) {
        return;
    }

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
    if (shuttingDown_ || !window()) {
        delete previous;
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
    if (shuttingDown_ || player_ == nullptr) {
        return;
    }

    const QImage frame = player_->takeLatestFrame();
    if (frame.isNull()) {
        return;
    }

    if (limiter_.elapsed() < targetFrameIntervalMs_) {
        return;
    }

    limiter_.restart();

    {
        QMutexLocker locker(&frameMutex_);
        currentFrame_ = frame;
    }

    state_ = PlayState::Playing;
    update();
}

void FFmpegViewItem::setUrl(const QString& url)
{
    if (url_ != url) {
        url_ = url;
        emit urlChanged();

        if (!shuttingDown_ && player_) {
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
    value = std::max(value, 1);
    if (decodeThreadCount_ == value) return;
    decodeThreadCount_ = value;
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
    if (shuttingDown_) {
        return;
    }

    errorMessage_ = error;
    state_ = PlayState::Failed;
    update();
}

void FFmpegViewItem::onConnectedChanged(bool connected)
{
    if (shuttingDown_) {
        return;
    }

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
