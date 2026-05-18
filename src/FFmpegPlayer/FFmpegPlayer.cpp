#include "FFmpegPlayer.h"
#include "PlaybackTracer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMutexLocker>
#include <atomic>

namespace
{
QString ffmpegErrorString(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

std::atomic_int g_nextPlayerId{1};
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

    tracerId_ = QStringLiteral("P%1").arg(g_nextPlayerId.fetch_add(1, std::memory_order_relaxed));
    PlaybackTracer::instance().start(
        QCoreApplication::applicationDirPath() + QStringLiteral("/logs"), 300);

    moveToThread(thread_);
    connect(thread_, &QThread::started, this, &FFmpegPlayer::run);
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

void FFmpegPlayer::setUseNoBuffer(bool value)
{
    config_.useNoBuffer = value;
}

void FFmpegPlayer::setUseLowDelay(bool value)
{
    config_.useLowDelay = value;
}

void FFmpegPlayer::setDisableReorderQueue(bool value)
{
    config_.disableReorderQueue = value;
}

void FFmpegPlayer::setSkipNonRefFrames(bool value)
{
    config_.skipNonRefFrames = value;
}

void FFmpegPlayer::requestKeyFrame()
{
    if (!awaitingKeyFrame_) {
        PlaybackTracer::instance().event(tracerId_, "KEYWAIT", QStringLiteral("flushing decoder, awaiting next I-frame"));
    }
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
    QMutexLocker locker(&frameMutex_);
    frameQueue_.clear();
    frameNotificationPending_ = false;
}

QImage FFmpegPlayer::takeNextFrame(bool& moreAvailable)
{
    QMutexLocker locker(&frameMutex_);
    if (frameQueue_.empty()) {
        moreAvailable = false;
        frameNotificationPending_ = false;
        return QImage();
    }
    QImage frame = std::move(frameQueue_.front());
    frameQueue_.pop_front();
    moreAvailable = !frameQueue_.empty();
    // 队列非空时保持 pending=true，由 UI 端通过定时器继续消费；
    // 队列为空时复位，下一帧到达由解码线程重新触发 frameReady。
    frameNotificationPending_ = moreAvailable;
    return frame;
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
    av_dict_set(&opts, "buffer_size", QByteArray::number(config_.socketBufferSize).constData(), 0);
    av_dict_set(&opts, "max_delay", QByteArray::number(config_.maxDelayUs).constData(), 0);
    if (config_.useNoBuffer) {
        av_dict_set(&opts, "fflags", "nobuffer", 0);
    }
    if (config_.useLowDelay) {
        av_dict_set(&opts, "flags", "low_delay", 0);
    }
    if (config_.disableReorderQueue) {
        av_dict_set(&opts, "reorder_queue_size", "0", 0);
    }

    const QByteArray urlBytes = rtsp_url_.toUtf8();
    const int openRet = avformat_open_input(&fmt_ctx_, urlBytes.constData(), nullptr, &opts);
    av_dict_free(&opts);

    if (openRet < 0) {
        emit errorOccurred(QStringLiteral("无法打开 RTSP 流: %1").arg(ffmpegErrorString(openRet)));
        cleanupResources();
        return false;
    }

    if (config_.useNoBuffer) {
        fmt_ctx_->flags |= AVFMT_FLAG_NOBUFFER;
    }
    fmt_ctx_->probesize = config_.probeSize;
    fmt_ctx_->max_analyze_duration = config_.analyzeDurationUs;
    fmt_ctx_->max_delay = config_.maxDelayUs;

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
    if (config_.skipNonRefFrames) {
        codec_ctx_->skip_frame = AVDISCARD_NONREF;
    }
#ifdef AV_CODEC_FLAG_LOW_DELAY
    if (config_.useLowDelay) {
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
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
    int droppedOldest = 0;
    int queueDepth = 0;
    {
        QMutexLocker locker(&frameMutex_);
        frameQueue_.push_back(std::move(frame));
        while (frameQueue_.size() > static_cast<size_t>(kMaxQueueDepth)) {
            frameQueue_.pop_front();
            ++droppedOldest;
        }
        queueDepth = static_cast<int>(frameQueue_.size());
        if (!frameNotificationPending_) {
            frameNotificationPending_ = true;
            shouldNotify = true;
        }
    }

    PlaybackTracer::instance().mark(tracerId_, "QUEUE",
        droppedOldest > 0
            ? QStringLiteral("depth=%1 dropped_oldest=%2").arg(queueDepth).arg(droppedOldest)
            : QStringLiteral("depth=%1").arg(queueDepth));

    if (shouldNotify) {
        lastEmitMonoMs_.store(PlaybackTracer::monoMs(), std::memory_order_release);
        emit frameReady();
    }
}

void FFmpegPlayer::run()
{
    auto& tracer = PlaybackTracer::instance();

    while (running_) {
        emit connectedChanged(false);
        tracer.event(tracerId_, "OPEN", QStringLiteral("url=%1 transport=%2").arg(rtsp_url_, config_.rtspTransport));

        int videoStream = -1;
        if (!openStream(videoStream)) {
            tracer.event(tracerId_, "OPEN_FAIL", QStringLiteral("retry in %1 ms").arg(config_.reconnectPauseMs));
            if (running_) {
                sleepInterruptible(config_.reconnectPauseMs);
            }
            continue;
        }

        emit connectedChanged(true);
        awaitingKeyFrame_ = true;
        tracer.event(tracerId_, "OPEN_OK",
            QStringLiteral("video_stream=%1 codec=%2 size=%3x%4")
                .arg(videoStream)
                .arg(codec_ ? QString::fromUtf8(codec_->name) : QStringLiteral("?"))
                .arg(codec_ctx_ ? codec_ctx_->width : 0)
                .arg(codec_ctx_ ? codec_ctx_->height : 0));

        tracer.event(tracerId_, "CONFIG",
            QStringLiteral("transport=%1 timeout=%2us sockBuf=%3 maxDelay=%4us probe=%5 "
                           "analyze=%6us reconnPause=%7ms readErrLim=%8 decErrLim=%9 "
                           "threads=%10 noBuf=%11 lowDelay=%12 noReorder=%13 skipNonRef=%14")
                .arg(config_.rtspTransport)
                .arg(config_.timeoutUs)
                .arg(config_.socketBufferSize)
                .arg(config_.maxDelayUs)
                .arg(config_.probeSize)
                .arg(config_.analyzeDurationUs)
                .arg(config_.reconnectPauseMs)
                .arg(config_.transientReadErrorLimit)
                .arg(config_.transientDecodeErrorLimit)
                .arg(config_.decodeThreadCount)
                .arg(config_.useNoBuffer)
                .arg(config_.useLowDelay)
                .arg(config_.disableReorderQueue)
                .arg(config_.skipNonRefFrames));

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
                tracer.event(tracerId_, "READ_ERR",
                    QStringLiteral("ret=%1 (%2) seq=%3/%4")
                        .arg(readRet)
                        .arg(ffmpegErrorString(readRet))
                        .arg(consecutiveReadErrors)
                        .arg(config_.transientReadErrorLimit));
                if (consecutiveReadErrors < config_.transientReadErrorLimit) {
                    sleepInterruptible(config_.reconnectPauseMs);
                    continue;
                }

                emit errorOccurred(QStringLiteral("读取码流失败，准备重连: %1").arg(ffmpegErrorString(readRet)));
                tracer.event(tracerId_, "RECONNECT", QStringLiteral("reason=read_errors"));
                needReconnect = true;
                break;
            }

            consecutiveReadErrors = 0;

            if (packet_->stream_index != videoStream) {
                av_packet_unref(packet_);
                continue;
            }

            tracer.mark(tracerId_, "READ",
                QStringLiteral("size=%1 pts=%2 flags=0x%3")
                    .arg(packet_->size)
                    .arg(packet_->pts)
                    .arg(packet_->flags, 0, 16));

            const int sendRet = avcodec_send_packet(codec_ctx_, packet_);
            if (sendRet == AVERROR(EAGAIN)) {
                av_packet_unref(packet_);
                continue;
            }

            if (sendRet < 0 && sendRet != AVERROR_EOF) {
                ++consecutiveDecodeErrors;
                tracer.event(tracerId_, "SEND_ERR",
                    QStringLiteral("ret=%1 (%2) seq=%3/%4")
                        .arg(sendRet)
                        .arg(ffmpegErrorString(sendRet))
                        .arg(consecutiveDecodeErrors)
                        .arg(config_.transientDecodeErrorLimit));
                qDebug() << "Drop corrupt packet:" << sendRet;
                av_packet_unref(packet_);
                requestKeyFrame();
                if (consecutiveDecodeErrors >= config_.transientDecodeErrorLimit) {
                    emit errorOccurred(QStringLiteral("连续解码失败，准备重连: %1").arg(ffmpegErrorString(sendRet)));
                    tracer.event(tracerId_, "RECONNECT", QStringLiteral("reason=send_errors"));
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
                    tracer.event(tracerId_, "RECV_ERR",
                        QStringLiteral("ret=%1 (%2) seq=%3/%4")
                            .arg(receiveRet)
                            .arg(ffmpegErrorString(receiveRet))
                            .arg(consecutiveDecodeErrors)
                            .arg(config_.transientDecodeErrorLimit));
                    qDebug() << "Receive frame error:" << receiveRet;
                    requestKeyFrame();
                    if (consecutiveDecodeErrors >= config_.transientDecodeErrorLimit) {
                        emit errorOccurred(QStringLiteral("连续解码失败，准备重连: %1").arg(ffmpegErrorString(receiveRet)));
                        tracer.event(tracerId_, "RECONNECT", QStringLiteral("reason=recv_errors"));
                        needReconnect = true;
                    }
                    break;
                }

                consecutiveDecodeErrors = 0;

                if (isFrameCorrupt(frame_)) {
                    tracer.event(tracerId_, "CORRUPT",
                        QStringLiteral("decode_error_flags=0x%1 flags=0x%2")
                            .arg(frame_->decode_error_flags, 0, 16)
                            .arg(frame_->flags, 0, 16));
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
                    tracer.event(tracerId_, "KEYRESUME", QStringLiteral("got I-frame, resuming output"));
                }

                tracer.mark(tracerId_, "DECODE",
                    QStringLiteral("pict_type=%1 pts=%2").arg(QChar(av_get_picture_type_char(frame_->pict_type))).arg(frame_->pts));

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
        tracer.event(tracerId_, "CLOSED",
            needReconnect ? QStringLiteral("will reconnect") : QStringLiteral("stop requested"));

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
