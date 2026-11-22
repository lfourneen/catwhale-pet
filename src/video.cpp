#include "video.hpp"

#include <QDebug>
#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace pet {
namespace {

using Clock = std::chrono::steady_clock;

template <typename T, void (*Free)(T **)>
struct PointerDeleter
{
    void operator()(T *pointer) const { Free(&pointer); }
};

using Frame = std::unique_ptr<AVFrame, PointerDeleter<AVFrame, av_frame_free>>;
using Codec = std::unique_ptr<AVCodecContext, PointerDeleter<AVCodecContext, avcodec_free_context>>;
using Format = std::unique_ptr<AVFormatContext, PointerDeleter<AVFormatContext, avformat_close_input>>;
using Packet = std::unique_ptr<AVPacket, PointerDeleter<AVPacket, av_packet_free>>;
using Device = std::unique_ptr<AVBufferRef, PointerDeleter<AVBufferRef, av_buffer_unref>>;

QString ffmpegError(int code)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return QString::fromUtf8(message);
}

AVPixelFormat hardwareFormat(AVCodecContext *codec, const AVPixelFormat *formats)
{
    if (!codec->hw_device_ctx)
        return AV_PIX_FMT_NONE;
    const auto *device = reinterpret_cast<const AVHWDeviceContext *>(codec->hw_device_ctx->data);
    const auto required = device->type == AV_HWDEVICE_TYPE_CUDA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
    for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == required)
            return *format;
    }
    return AV_PIX_FMT_NONE;
}

struct HardwareDevice
{
    HardwareDevice(AVHWDeviceType type, AVPixelFormat format, const char *label)
        : type(type), format(format), label(label)
    {
    }

    bool supports(const AVCodec *codec) const
    {
        if (!codec)
            return false;
        for (int index = 0; const auto *config = avcodec_get_hw_config(codec, index); ++index) {
            if (config->device_type == type && config->pix_fmt == format
                && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                return true;
        }
        return false;
    }

    AVBufferRef *get()
    {
        if (!available)
            return nullptr;
        if (!device) {
            AVBufferRef *ref = nullptr;
            AVDictionary *options = nullptr;
            if (type == AV_HWDEVICE_TYPE_VAAPI)
                av_dict_set(&options, "connection_type", "drm", 0);
            const int result = av_hwdevice_ctx_create(&ref, type, nullptr, options, 0);
            av_dict_free(&options);
            if (result < 0) {
                available = false;
                qInfo().noquote() << label << "unavailable:" << ffmpegError(result);
                return nullptr;
            }
            device.reset(ref);
        }
        return device.get();
    }

    const AVHWDeviceType type;
    const AVPixelFormat format;
    const char *label;
    Device device;
    bool available = true;
};

using HardwareDevices = std::array<HardwareDevice, 2>;

Codec openCodec(const AVCodec *codec, const AVStream &stream, AVBufferRef *device, int &error)
{
    if (!codec) {
        error = AVERROR_DECODER_NOT_FOUND;
        return {};
    }
    Codec context(avcodec_alloc_context3(codec));
    if (!context) {
        error = AVERROR(ENOMEM);
        return {};
    }
    error = avcodec_parameters_to_context(context.get(), stream.codecpar);
    if (error < 0)
        return {};
    context->pkt_timebase = stream.time_base;
    context->thread_count = device ? 1 : 2;
    if (device) {
        context->hw_device_ctx = av_buffer_ref(device);
        if (!context->hw_device_ctx) {
            error = AVERROR(ENOMEM);
            return {};
        }
        context->get_format = hardwareFormat;
        context->extra_hw_frames = 4;
    }
    error = avcodec_open2(context.get(), codec, nullptr);
    return error < 0 ? Codec{} : std::move(context);
}

struct DecodedFrame
{
    Frame color;
    Frame alpha;
    double seconds = 0.0;
};

class Decoder
{
public:
    ~Decoder()
    {
        sws_freeContext(colorScaler_);
        sws_freeContext(alphaScaler_);
    }

    int open(const QString &path, HardwareDevice *hardware)
    {
        const QByteArray fileName = QFile::encodeName(path);
        AVFormatContext *input = nullptr;
        AVDictionary *options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        int result = avformat_open_input(&input, fileName.constData(), nullptr, &options);
        av_dict_free(&options);
        if (result < 0)
            return result;
        input_.reset(input);
        if ((result = avformat_find_stream_info(input, nullptr)) < 0)
            return result;
        streamIndex_ = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex_ < 0)
            return streamIndex_;
        stream_ = input->streams[streamIndex_];
        const auto codecId = stream_->codecpar->codec_id;
        const auto *alphaTag = av_dict_get(stream_->metadata, "alpha_mode", nullptr, 0);
        expectsAlpha_ = alphaTag && QByteArrayView(alphaTag->value) == "1";

        const AVCodec *codec = avcodec_find_decoder(codecId);
        AVBufferRef *device = nullptr;
        if (hardware) {
            if (!hardware->supports(codec))
                return AVERROR(ENOSYS);
            device = hardware->get();
            if (!device)
                return AVERROR(ENODEV);
        } else if (codecId == AV_CODEC_ID_VP9) {
            codec = avcodec_find_decoder_by_name("libvpx-vp9");
        } else if (codecId == AV_CODEC_ID_VP8) {
            codec = avcodec_find_decoder_by_name("libvpx");
        }

        color_ = openCodec(codec, *stream_, device, result);
        if (!color_)
            return result;
        hardware_ = hardware;
        if (hardware_ && expectsAlpha_) {
            alpha_ = openCodec(codec, *stream_, device, result);
            if (!alpha_)
                return result;
        }
        packet_.reset(av_packet_alloc());
        alphaPacket_.reset(av_packet_alloc());
        if (!packet_ || !alphaPacket_)
            return AVERROR(ENOMEM);
        const double rate = av_q2d(av_guess_frame_rate(input, stream_, nullptr));
        if (rate > 0 && std::isfinite(rate))
            frameInterval_ = 1.0 / rate;
        origin_ = stream_->start_time == AV_NOPTS_VALUE ? 0 : stream_->start_time;
        return 0;
    }

    int seek(double seconds)
    {
        if (seconds > std::numeric_limits<int64_t>::max() / AV_TIME_BASE)
            return AVERROR(EINVAL);
        const int64_t timestamp = av_sat_add64(origin_, av_rescale_q(
            static_cast<int64_t>(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, stream_->time_base));
        const int result = av_seek_frame(input_.get(), streamIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
        if (result < 0)
            return result;
        colorFrames_.clear();
        alphaFrames_.clear();
        avcodec_flush_buffers(color_.get());
        if (alpha_)
            avcodec_flush_buffers(alpha_.get());
        drained_ = false;
        nextTimestamp_ = seconds;
        return 0;
    }

    double interval() const { return frameInterval_; }
    int next(DecodedFrame &output)
    {
        while (colorFrames_.empty() || (alpha_ && alphaFrames_.empty())) {
            if (drained_)
                return colorFrames_.empty() && alphaFrames_.empty() ? AVERROR_EOF : AVERROR_INVALIDDATA;

            int result = av_read_frame(input_.get(), packet_.get());
            if (result == AVERROR_EOF) {
                drained_ = true;
                result = drain(color_.get(), nullptr, colorFrames_);
                if (result >= 0 && alpha_)
                    result = drain(alpha_.get(), nullptr, alphaFrames_);
                if (result < 0)
                    return result;
                continue;
            }
            if (result < 0)
                return result;
            if (packet_->stream_index == streamIndex_)
                result = decodePacket();
            av_packet_unref(packet_.get());
            if (result < 0)
                return result;
        }

        output.color = std::move(colorFrames_.front());
        colorFrames_.pop_front();
        if (alpha_) {
            if (output.color->best_effort_timestamp != alphaFrames_.front()->best_effort_timestamp)
                return AVERROR_INVALIDDATA;
            output.alpha = std::move(alphaFrames_.front());
            alphaFrames_.pop_front();
        }
        const auto timestamp = output.color->best_effort_timestamp;
        output.seconds = timestamp == AV_NOPTS_VALUE
            ? nextTimestamp_ : (static_cast<double>(timestamp) - origin_) * av_q2d(stream_->time_base);
        nextTimestamp_ = output.seconds + frameInterval_;
        return 0;
    }

    int image(DecodedFrame &decoded, QSize target, QImage &image)
    {
        if (hardware_) {
            int result = download(decoded.color);
            if (result >= 0 && decoded.alpha)
                result = download(decoded.alpha);
            if (result < 0)
                return result;
        }
        const QSize original(decoded.color->width, decoded.color->height);
        if (original.isEmpty())
            return AVERROR_INVALIDDATA;
        if (target.isValid() && !target.isEmpty())
            target = original.scaled(target, Qt::KeepAspectRatio);
        else
            target = original;
        const auto format = static_cast<AVPixelFormat>(decoded.color->format);
        const auto *description = av_pix_fmt_desc_get(format);
        if (!description || (expectsAlpha_ && !decoded.alpha && !(description->flags & AV_PIX_FMT_FLAG_ALPHA)))
            return AVERROR_INVALIDDATA;

        QImage straight(target, QImage::Format_RGBA8888);
        if (straight.isNull())
            return AVERROR(ENOMEM);
        colorScaler_ = sws_getCachedContext(colorScaler_, original.width(), original.height(), format,
                                          target.width(), target.height(), AV_PIX_FMT_RGBA,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!colorScaler_)
            return AVERROR(ENOMEM);
        uint8_t *pixels[] = {straight.bits(), nullptr, nullptr, nullptr};
        int stride[] = {static_cast<int>(straight.bytesPerLine()), 0, 0, 0};
        const auto *coefficients = sws_getCoefficients(decoded.color->colorspace == AVCOL_SPC_BT709
            ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        sws_setColorspaceDetails(colorScaler_, coefficients, decoded.color->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
        if (sws_scale(colorScaler_, decoded.color->data, decoded.color->linesize, 0, original.height(),
                      pixels, stride) != target.height())
            return AVERROR_INVALIDDATA;

        if (decoded.alpha) {
            const int result = addAlpha(*decoded.alpha, original, straight);
            if (result < 0)
                return result;
        }
        image = straight.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        return image.isNull() ? AVERROR(ENOMEM) : 0;
    }

private:
    int decodePacket()
    {
        if (hardware_) {
            size_t size = 0;
            const auto *data = av_packet_get_side_data(packet_.get(), AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
            const bool alphaBlock = data && size > 8 && AV_RB64(data) == 1;
            if (alphaBlock != static_cast<bool>(alpha_))
                return AVERROR_INVALIDDATA;
            if (alphaBlock) {
                if (size - 8 > INT_MAX)
                    return AVERROR_INVALIDDATA;
                int result = av_new_packet(alphaPacket_.get(), static_cast<int>(size - 8));
                if (result < 0)
                    return result;
                std::memcpy(alphaPacket_->data, data + 8, size - 8);
                alphaPacket_->pts = packet_->pts;
                alphaPacket_->dts = packet_->dts;
                alphaPacket_->duration = packet_->duration;
                alphaPacket_->flags = packet_->flags;
                result = drain(alpha_.get(), alphaPacket_.get(), alphaFrames_);
                av_packet_unref(alphaPacket_.get());
                if (result < 0)
                    return result;
            }
        }
        return drain(color_.get(), packet_.get(), colorFrames_);
    }

    static int drain(AVCodecContext *codec, AVPacket *packet, std::deque<Frame> &frames)
    {
        int result = avcodec_send_packet(codec, packet);
        if (result < 0 && result != AVERROR_EOF)
            return result;
        for (;;) {
            Frame frame(av_frame_alloc());
            if (!frame)
                return AVERROR(ENOMEM);
            result = avcodec_receive_frame(codec, frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return 0;
            if (result < 0)
                return result;
            // Bound memory if the color and alpha queues move out of sync.
            if (frames.size() == 12)
                return AVERROR_INVALIDDATA;
            frames.push_back(std::move(frame));
        }
    }

    int download(Frame &frame) const
    {
        if (frame->format != hardware_->format || !frame->hw_frames_ctx)
            return AVERROR_INVALIDDATA;
        Frame copy(av_frame_alloc());
        if (!copy)
            return AVERROR(ENOMEM);
        int result = av_hwframe_transfer_data(copy.get(), frame.get(), 0);
        if (result < 0)
            return result;
        result = av_frame_copy_props(copy.get(), frame.get());
        if (result >= 0)
            frame = std::move(copy);
        return result;
    }

    int addAlpha(const AVFrame &alpha, QSize original, QImage &image)
    {
        if (alpha.width != original.width() || alpha.height != original.height())
            return AVERROR_INVALIDDATA;
        const auto *description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(alpha.format));
        if (!description || !alpha.data[0] || description->comp[0].plane != 0)
            return AVERROR_INVALIDDATA;

        // Decode the alpha stream's luma as full-range coverage.
        AVPixelFormat gray;
        switch (description->comp[0].depth) {
        case 8: gray = AV_PIX_FMT_GRAY8; break;
        case 10: gray = description->comp[0].shift == 0 ? AV_PIX_FMT_GRAY10LE : AV_PIX_FMT_GRAY16LE; break;
        case 12: gray = description->comp[0].shift == 0 ? AV_PIX_FMT_GRAY12LE : AV_PIX_FMT_GRAY16LE; break;
        case 16: gray = AV_PIX_FMT_GRAY16LE; break;
        default: return AVERROR_INVALIDDATA;
        }
        alphaScaler_ = sws_getCachedContext(alphaScaler_, original.width(), original.height(), gray,
                                          image.width(), image.height(), AV_PIX_FMT_GRAY8,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!alphaScaler_)
            return AVERROR(ENOMEM);
        QImage mask(image.size(), QImage::Format_Grayscale8);
        if (mask.isNull())
            return AVERROR(ENOMEM);
        const uint8_t *source[] = {alpha.data[0], nullptr, nullptr, nullptr};
        int sourceStride[] = {alpha.linesize[0], 0, 0, 0};
        uint8_t *target[] = {mask.bits(), nullptr, nullptr, nullptr};
        int targetStride[] = {static_cast<int>(mask.bytesPerLine()), 0, 0, 0};
        if (sws_scale(alphaScaler_, source, sourceStride, 0, original.height(), target, targetStride) != image.height())
            return AVERROR_INVALIDDATA;
        for (int y = 0; y < image.height(); ++y) {
            auto *row = image.scanLine(y);
            const auto *coverage = mask.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                row[4 * x + 3] = coverage[x];
        }
        return 0;
    }

    Format input_;
    Codec color_;
    Codec alpha_;
    Packet packet_;
    Packet alphaPacket_;
    std::deque<Frame> colorFrames_;
    std::deque<Frame> alphaFrames_;
    SwsContext *colorScaler_ = nullptr;
    SwsContext *alphaScaler_ = nullptr;
    AVStream *stream_ = nullptr;
    int streamIndex_ = -1;
    int64_t origin_ = 0;
    double frameInterval_ = 1.0 / 24.0;
    double nextTimestamp_ = 0.0;
    bool drained_ = false;
    bool expectsAlpha_ = false;
    const HardwareDevice *hardware_ = nullptr;
};

struct Playback
{
    QString path;
    bool loop = true;
    double start = 0.0;
    double end = 0.0;
};

double sanitizeSpeed(double speed)
{
    return std::isfinite(speed) ? std::clamp(speed, 0.05, 8.0) : 1.0;
}

} // namespace

struct VideoPlayer::Private
{
    Private(VideoPlayer *owner, bool hardware)
        : owner(owner), allowHardware(hardware), worker([this] { run(); })
    {
    }

    ~Private()
    {
        {
            std::lock_guard lock(mutex);
            shuttingDown = true;
        }
        updated.notify_one();
        worker.join();
    }

    void play(Playback playback, double speed)
    {
        {
            std::lock_guard lock(mutex);
            request = std::move(playback);
            ++generation;
            rate = sanitizeSpeed(speed);
            latestFrame = {};
        }
        updated.notify_one();
    }

    bool current(quint64 id) const
    {
        return !shuttingDown && generation == id;
    }

    bool waitUntil(quint64 id, Clock::time_point previous, double seconds)
    {
        std::unique_lock lock(mutex);
        while (current(id)) {
            const auto duration = std::chrono::duration<double>(seconds / rate);
            const auto deadline = previous + std::chrono::duration_cast<Clock::duration>(duration);
            if (Clock::now() >= deadline)
                return true;
            updated.wait_until(lock, deadline);
        }
        return false;
    }

    void postFrame(quint64 id, QImage frame)
    {
        {
            std::lock_guard lock(mutex);
            if (!current(id))
                return;
            latestFrame = std::move(frame);
            frameGeneration = id;
            if (framePosted)
                return;
            framePosted = true;
        }
        QMetaObject::invokeMethod(owner, [this] {
            QImage frame;
            {
                std::lock_guard lock(mutex);
                framePosted = false;
                if (current(frameGeneration))
                    frame = std::move(latestFrame);
            }
            if (!frame.isNull())
                emit owner->frameReady(frame);
        }, Qt::QueuedConnection);
    }

    template <typename Function>
    void postSignal(quint64 id, Function callback)
    {
        QMetaObject::invokeMethod(owner, [this, id, callback] {
            {
                std::lock_guard lock(mutex);
                if (!current(id))
                    return;
            }
            callback();
        }, Qt::QueuedConnection);
    }

    void run()
    {
        HardwareDevices devices{{
            {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA, "NVIDIA NVDEC"},
            {AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI, "VA-API"},
        }};
        quint64 handled = 0;
        for (;;) {
            Playback playback;
            quint64 id;
            {
                std::unique_lock lock(mutex);
                updated.wait(lock, [&] { return shuttingDown || generation != handled; });
                if (shuttingDown)
                    return;
                id = handled = generation;
                playback = request;
            }
            if (playback.path.isEmpty()) {
                for (auto &device : devices)
                    device.device.reset();
                continue;
            }
            playRequest(id, playback, devices);
        }
    }

    void playRequest(quint64 id, const Playback &playback, HardwareDevices &devices)
    {
        bool haveFrame = false;
        double position = playback.start;
        double lastTimestamp = position;
        auto lastFrameAt = Clock::now();
        if (!waitUntil(id, lastFrameAt, 0))
            return;
        auto nextDevice = devices.begin();
        for (;;) {
            HardwareDevice *hardware = nullptr;
            while (allowHardware && nextDevice != devices.end()) {
                auto &candidate = *nextDevice++;
                if (candidate.get()) {
                    hardware = &candidate;
                    break;
                }
            }
            Decoder decoder;
            int result = decoder.open(playback.path, hardware);
            if (result >= 0 && position > 0)
                result = decoder.seek(position);
            if (result >= 0) {
                int resultOfPlayback = 0;
                bool presented = false;
                bool notified = false;
                for (;;) {
                    if (!waitUntil(id, Clock::now(), 0))
                        return;
                    DecodedFrame frame;
                    resultOfPlayback = decoder.next(frame);
                    const bool reachedEnd = resultOfPlayback == AVERROR_EOF
                        || (resultOfPlayback == 0 && playback.end > 0 && frame.seconds >= playback.end);
                    if (reachedEnd) {
                        if (!presented) {
                            resultOfPlayback = AVERROR_INVALIDDATA;
                            break;
                        }
                        double tail = decoder.interval();
                        if (playback.end > 0)
                            tail = std::clamp(playback.end - lastTimestamp, 0.0, tail);
                        if (!waitUntil(id, lastFrameAt, tail))
                            return;
                        if (!playback.loop) {
                            postSignal(id, [this] { emit owner->finished(); });
                            return;
                        }
                        resultOfPlayback = decoder.seek(playback.start);
                        if (resultOfPlayback < 0)
                            break;
                        haveFrame = false;
                        presented = false;
                        position = playback.start;
                        continue;
                    }
                    if (resultOfPlayback < 0)
                        break;
                    if (frame.seconds < position)
                        continue;

                    QSize pixels;
                    {
                        std::lock_guard lock(mutex);
                        if (!current(id))
                            return;
                        pixels = target;
                    }
                    QImage image;
                    resultOfPlayback = decoder.image(frame, pixels, image);
                    if (resultOfPlayback < 0)
                        break;
                    const double delay = haveFrame
                        ? std::clamp(frame.seconds - lastTimestamp, 0.0, 1.0) : 0.0;
                    if (!waitUntil(id, lastFrameAt, delay))
                        return;
                    if (!notified) {
                        const QString backend = hardware ? QString::fromLatin1(hardware->label) : QStringLiteral("CPU");
                        postSignal(id, [this, backend] {
                            if (backend == lastBackend)
                                return;
                            lastBackend = backend;
                            emit owner->backendChanged(backend);
                        });
                        notified = true;
                    }
                    presented = true;
                    haveFrame = true;
                    lastTimestamp = frame.seconds;
                    lastFrameAt = Clock::now();
                    postFrame(id, std::move(image));
                }
                result = resultOfPlayback;
            }

            if (hardware) {
                if (result != AVERROR(ENOSYS) && result != AVERROR(ENODEV)) {
                    qInfo().noquote() << hardware->label << "playback failed, trying another decoder for"
                                      << playback.path << ffmpegError(result);
                }
                hardware->device.reset();
                if (haveFrame)
                    position = lastTimestamp;
                continue;
            }
            const QString error = ffmpegError(result);
            postSignal(id, [this, error] { emit owner->failed(error); });
            return;
        }
    }

    VideoPlayer *owner;
    bool allowHardware;
    std::mutex mutex;
    std::condition_variable updated;
    Playback request;
    QSize target;
    QImage latestFrame;
    QString lastBackend;
    quint64 generation = 0;
    quint64 frameGeneration = 0;
    double rate = 1.0;
    bool framePosted = false;
    bool shuttingDown = false;
    std::thread worker;
};

VideoPlayer::VideoPlayer(bool allowHardware, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, allowHardware))
{
}

VideoPlayer::~VideoPlayer() = default;

void VideoPlayer::play(const QString &path, bool loop, double speed, double start, double end)
{
    const double lower = std::isfinite(start) ? std::max(0.0, start) : 0.0;
    const double upper = std::isfinite(end) && end > lower ? end : 0.0;
    d->play({path, loop, lower, upper}, speed);
}

void VideoPlayer::stop()
{
    d->play({}, 1.0);
}

void VideoPlayer::setSpeed(double speed)
{
    {
        std::lock_guard lock(d->mutex);
        d->rate = sanitizeSpeed(speed);
    }
    d->updated.notify_one();
}

void VideoPlayer::setTargetSize(QSize physicalPixels)
{
    std::lock_guard lock(d->mutex);
    d->target = physicalPixels;
}

} // namespace pet
