// Copyright (c) 2026 yasukioo
// Author: yasukioo <yasukioo@outlook.com>

#include "FFmpegPlayer.h"

#include <QDebug>
#include <QMutexLocker>

namespace
{
QString ffmpegErrorString(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}
}

std::shared_ptr<FFmpegPlayer::FrameBufferPool> FFmpegPlayer::FrameBufferPool::create()
{
    return std::shared_ptr<FrameBufferPool>(new FrameBufferPool());
}

std::shared_ptr<FFmpegPlayer::FrameBuffer> FFmpegPlayer::FrameBufferPool::acquire(int width, int height, int bytesPerLine)
{
    std::unique_ptr<FrameBuffer> buf;
    {
        QMutexLocker locker(&mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if ((*it)->width == width && (*it)->height == height && (*it)->bytesPerLine == bytesPerLine) {
                buf = std::move(*it);
                free_.erase(it);
                break;
            }
        }
        if (!buf && !free_.empty()) {
            free_.clear();
        }
    }

    if (!buf) {
        buf = std::make_unique<FrameBuffer>();
        buf->width = width;
        buf->height = height;
        buf->bytesPerLine = bytesPerLine;
        buf->data.resize(static_cast<size_t>(bytesPerLine) * static_cast<size_t>(height));
    }

    auto self = shared_from_this();
    FrameBuffer* raw = buf.release();
    return std::shared_ptr<FrameBuffer>(raw, [self](FrameBuffer* b) {
        self->recycle(b);
    });
}

void FFmpegPlayer::FrameBufferPool::recycle(FrameBuffer* buf)
{
    std::unique_ptr<FrameBuffer> owned(buf);
    QMutexLocker locker(&mutex_);
    if (free_.size() < 4) {
        free_.push_back(std::move(owned));
    }
}

FFmpegPlayer::FFmpegPlayer(QObject* parent)
    : QObject(parent)
    , thread_(new QThread())
    , running_(false)
    , fmt_ctx_(nullptr)
    , codec_ctx_(nullptr)
    , codec_(nullptr)
    , packet_(nullptr)
    , frame_(nullptr)
    , sws_ctx_(nullptr)
    , bufferPool_(FrameBufferPool::create())
{
    avformat_network_init();

    connect(thread_, &QThread::started, this, &FFmpegPlayer::run, Qt::DirectConnection);
}

FFmpegPlayer::~FFmpegPlayer()
{
    stop();
    delete thread_;
}

void FFmpegPlayer::setUrl(const QString& url)
{
    rtsp_url_ = url;
}

void FFmpegPlayer::setRtspTransport(const QString& transport)
{
    const QString normalized = transport.trimmed().toLower();
    config_.rtspTransport = (normalized == QStringLiteral("tcp")) ? QStringLiteral("tcp") : QStringLiteral("udp");
}

void FFmpegPlayer::setTimeoutUs(int value)
{
    config_.timeoutUs = std::max(value, 100000);
}

void FFmpegPlayer::setSocketBufferSize(int value)
{
    config_.socketBufferSize = std::max(value, 262144);
}

void FFmpegPlayer::setMaxDelayUs(int value)
{
    config_.maxDelayUs = std::max(value, 0);
}

void FFmpegPlayer::setProbeSize(int value)
{
    config_.probeSize = std::max(value, 32768);
}

void FFmpegPlayer::setAnalyzeDurationUs(qint64 value)
{
    config_.analyzeDurationUs = std::max<qint64>(value, 0);
}

void FFmpegPlayer::setReconnectPauseMs(int value)
{
    config_.reconnectPauseMs = std::max(value, 0);
}

void FFmpegPlayer::setTransientReadErrorLimit(int value)
{
    config_.transientReadErrorLimit = std::max(value, 1);
}

void FFmpegPlayer::setTransientDecodeErrorLimit(int value)
{
    config_.transientDecodeErrorLimit = std::max(value, 1);
}

void FFmpegPlayer::setDecodeThreadCount(int value)
{
    config_.decodeThreadCount = std::max(value, 0);
}

void FFmpegPlayer::requestKeyFrame()
{
    awaitingKeyFrame_ = true;
    if (codec_ctx_ != nullptr) {
        avcodec_flush_buffers(codec_ctx_);
    }
}

bool FFmpegPlayer::isFrameCorrupt(const AVFrame* f) const
{
    if (f == nullptr) {
        return true;
    }
    if (f->decode_error_flags != 0) {
        return true;
    }
#ifdef AV_FRAME_FLAG_CORRUPT
    if (f->flags & AV_FRAME_FLAG_CORRUPT) {
        return true;
    }
#endif
    return false;
}

void FFmpegPlayer::start()
{
    if (!thread_->isRunning()) {
        running_ = true;
        thread_->start();
    }
}

void FFmpegPlayer::stop()
{
    running_ = false;
    if (thread_->isRunning()) {
        thread_->quit();
        thread_->wait();
    }
}

QImage FFmpegPlayer::takeLatestFrame()
{
    QMutexLocker locker(&frameMutex_);
    frameNotificationPending_ = false;
    return latestFrame_;
}

void FFmpegPlayer::sleepInterruptible(int ms)
{
    int elapsed = 0;
    constexpr int step = 50;
    while (running_ && elapsed < ms) {
        QThread::msleep(step);
        elapsed += step;
    }
}

int FFmpegPlayer::interruptCallback(void* ctx)
{
    auto* self = static_cast<FFmpegPlayer*>(ctx);
    return self && !self->running_ ? 1 : 0;
}

bool FFmpegPlayer::openStream(int& videoStream)
{
    cleanupResources();

    fmt_ctx_ = avformat_alloc_context();
    if (fmt_ctx_ == nullptr) {
        emit errorOccurred(QStringLiteral("无法分配 AVFormatContext"));
        return false;
    }

    fmt_ctx_->interrupt_callback.callback = &FFmpegPlayer::interruptCallback;
    fmt_ctx_->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", config_.rtspTransport.toUtf8().constData(), 0);
    av_dict_set(&opts, "timeout", QByteArray::number(config_.timeoutUs).constData(), 0);
    av_dict_set(&opts, "stimeout", QByteArray::number(config_.timeoutUs).constData(), 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "buffer_size", QByteArray::number(config_.socketBufferSize).constData(), 0);
    av_dict_set(&opts, "max_delay", QByteArray::number(config_.maxDelayUs).constData(), 0);
    av_dict_set(&opts, "reorder_queue_size", "0", 0);

    const QByteArray urlBytes = rtsp_url_.toUtf8();
    const int openRet = avformat_open_input(&fmt_ctx_, urlBytes.constData(), nullptr, &opts);
    av_dict_free(&opts);

    if (openRet < 0) {
        emit errorOccurred(QStringLiteral("无法打开 RTSP 流: %1").arg(ffmpegErrorString(openRet)));
        cleanupResources();
        return false;
    }

    fmt_ctx_->flags |= AVFMT_FLAG_NOBUFFER;
    fmt_ctx_->probesize = config_.probeSize;
    fmt_ctx_->max_analyze_duration = config_.analyzeDurationUs;
    fmt_ctx_->max_delay = 0;

    const int infoRet = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (infoRet < 0) {
        emit errorOccurred(QStringLiteral("无法获取流信息: %1").arg(ffmpegErrorString(infoRet)));
        cleanupResources();
        return false;
    }

    videoStream = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream < 0) {
        emit errorOccurred(QStringLiteral("未找到视频流: %1").arg(ffmpegErrorString(videoStream)));
        cleanupResources();
        return false;
    }

    codec_ = avcodec_find_decoder(fmt_ctx_->streams[videoStream]->codecpar->codec_id);
    if (codec_ == nullptr) {
        emit errorOccurred(QStringLiteral("找不到视频解码器"));
        cleanupResources();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (codec_ctx_ == nullptr) {
        emit errorOccurred(QStringLiteral("无法分配 AVCodecContext"));
        cleanupResources();
        return false;
    }

    const int paramsRet = avcodec_parameters_to_context(codec_ctx_, fmt_ctx_->streams[videoStream]->codecpar);
    if (paramsRet < 0) {
        emit errorOccurred(QStringLiteral("无法复制解码参数: %1").arg(ffmpegErrorString(paramsRet)));
        cleanupResources();
        return false;
    }

    codec_ctx_->thread_count = config_.decodeThreadCount;
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    codec_ctx_->err_recognition |= AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER;
#ifdef AV_CODEC_FLAG_LOW_DELAY
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
#endif
#ifdef AV_CODEC_FLAG2_FAST
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
#endif

    const int codecOpenRet = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (codecOpenRet < 0) {
        emit errorOccurred(QStringLiteral("解码器打开失败: %1").arg(ffmpegErrorString(codecOpenRet)));
        cleanupResources();
        return false;
    }

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    sws_ctx_ = sws_getContext(codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
        codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_BGRA,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    if (packet_ == nullptr || frame_ == nullptr || sws_ctx_ == nullptr) {
        emit errorOccurred(QStringLiteral("播放器资源初始化失败"));
        cleanupResources();
        return false;
    }

    return true;
}

void FFmpegPlayer::queueFrame(QImage&& frame)
{
    bool shouldNotify = false;
    {
        QMutexLocker locker(&frameMutex_);
        latestFrame_ = std::move(frame);
        if (!frameNotificationPending_) {
            frameNotificationPending_ = true;
            shouldNotify = true;
        }
    }

    if (shouldNotify) {
        emit frameReady();
    }
}

void FFmpegPlayer::run()
{
    while (running_) {
        emit connectedChanged(false);

        int videoStream = -1;
        if (!openStream(videoStream)) {
            if (running_) {
                sleepInterruptible(config_.reconnectPauseMs);
            }
            continue;
        }

        emit connectedChanged(true);
        awaitingKeyFrame_ = true;

        int consecutiveReadErrors = 0;
        int consecutiveDecodeErrors = 0;
        bool needReconnect = false;

        while (running_ && !needReconnect) {
            const int readRet = av_read_frame(fmt_ctx_, packet_);
            if (readRet == AVERROR(EAGAIN)) {
                continue;
            }

            if (readRet < 0) {
                if (!running_) {
                    break;
                }

                ++consecutiveReadErrors;
                if (consecutiveReadErrors < config_.transientReadErrorLimit) {
                    sleepInterruptible(config_.reconnectPauseMs);
                    continue;
                }

                emit errorOccurred(QStringLiteral("读取码流失败，准备重连: %1").arg(ffmpegErrorString(readRet)));
                needReconnect = true;
                break;
            }

            consecutiveReadErrors = 0;

            if (packet_->stream_index != videoStream) {
                av_packet_unref(packet_);
                continue;
            }

            const int sendRet = avcodec_send_packet(codec_ctx_, packet_);
            if (sendRet == AVERROR(EAGAIN)) {
                av_packet_unref(packet_);
                continue;
            }

            if (sendRet < 0 && sendRet != AVERROR_EOF) {
                ++consecutiveDecodeErrors;
                qDebug() << "Drop corrupt packet:" << sendRet;
                av_packet_unref(packet_);
                requestKeyFrame();
                if (consecutiveDecodeErrors >= config_.transientDecodeErrorLimit) {
                    emit errorOccurred(QStringLiteral("连续解码失败，准备重连: %1").arg(ffmpegErrorString(sendRet)));
                    needReconnect = true;
                }
                continue;
            }

            while (running_) {
                const int receiveRet = avcodec_receive_frame(codec_ctx_, frame_);
                if (receiveRet == AVERROR(EAGAIN) || receiveRet == AVERROR_EOF) {
                    break;
                }

                if (receiveRet < 0) {
                    ++consecutiveDecodeErrors;
                    qDebug() << "Receive frame error:" << receiveRet;
                    requestKeyFrame();
                    if (consecutiveDecodeErrors >= config_.transientDecodeErrorLimit) {
                        emit errorOccurred(QStringLiteral("连续解码失败，准备重连: %1").arg(ffmpegErrorString(receiveRet)));
                        needReconnect = true;
                    }
                    break;
                }

                consecutiveDecodeErrors = 0;

                if (isFrameCorrupt(frame_)) {
                    qDebug() << "Drop corrupt frame, error_flags=" << frame_->decode_error_flags;
                    requestKeyFrame();
                    av_frame_unref(frame_);
                    continue;
                }

                if (awaitingKeyFrame_) {
                    bool isKey = frame_->pict_type == AV_PICTURE_TYPE_I;
#ifdef AV_FRAME_FLAG_KEY
                    isKey = isKey || (frame_->flags & AV_FRAME_FLAG_KEY);
#endif
                    if (!isKey) {
                        av_frame_unref(frame_);
                        continue;
                    }
                    awaitingKeyFrame_ = false;
                }

                const int width = codec_ctx_->width;
                const int height = codec_ctx_->height;
                const int bytesPerLine = width * 4;
                auto buf = bufferPool_->acquire(width, height, bytesPerLine);
                uint8_t* dst[4] = { buf->data.data(), nullptr, nullptr, nullptr };
                int dstLinesize[4] = { bytesPerLine, 0, 0, 0 };
                sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, height, dst, dstLinesize);

                auto* holder = new std::shared_ptr<FrameBuffer>(std::move(buf));
                QImage image((*holder)->data.data(), width, height, bytesPerLine, QImage::Format_ARGB32_Premultiplied,
                    [](void* p) { delete static_cast<std::shared_ptr<FrameBuffer>*>(p); },
                    holder);
                queueFrame(std::move(image));
                av_frame_unref(frame_);
            }

            av_packet_unref(packet_);
        }

        cleanupResources();

        if (running_) {
            sleepInterruptible(config_.reconnectPauseMs);
        }
    }

    cleanupResources();
}

void FFmpegPlayer::cleanupResources()
{
    if (fmt_ctx_ != nullptr) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    if (codec_ctx_ != nullptr) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (frame_ != nullptr) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (sws_ctx_ != nullptr) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
}
