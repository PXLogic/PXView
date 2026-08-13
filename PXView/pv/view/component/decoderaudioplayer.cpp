#include "pv/view/component/decoderaudioplayer.h"
#include "pv/base/log.h"
#include "pv/data/decoderanalogdata.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/data/stack/decoderstack.h"

#include <QThread>
#include <QMetaObject>
#include <QAudioSink>
#include <QAudioFormat>
#include <QBuffer>
#include <QEventLoop>
#include <QTimer>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QSettings>

namespace pv {
namespace view {

// =========================================================================
// DecoderAudioPlayerWorker
// =========================================================================

DecoderAudioPlayerWorker::DecoderAudioPlayerWorker(QObject *parent)
    : QObject(parent)
{
}

DecoderAudioPlayerWorker::~DecoderAudioPlayerWorker()
{
    if (_playing.load())
        doStop();
}

void DecoderAudioPlayerWorker::doPlay(
    const std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> &channels,
    int sampleRate, bool loop)
{
    if (channels.empty() || sampleRate <= 0) {
        emit playbackError("No channels or invalid sample rate");
        return;
    }

    // Collect samples from all channels and mix to mono (or keep stereo if 2 ch).
    const size_t numChannels = channels.size();
    size_t totalSamples = 0;
    for (const auto &ch : channels) {
        if (!ch) continue;
        totalSamples = std::max(totalSamples, ch->get_sample_count());
    }
    if (totalSamples == 0) {
        emit playbackError("No samples to play");
        return;
    }

    // Clamp channel count to 2 for audio output.
    const int outChannels = static_cast<int>(std::min<size_t>(numChannels, 2));

    // Convert float [-1,1] to interleaved int16 PCM.
    QByteArray audioData;
    audioData.reserve(static_cast<int>(totalSamples * outChannels * sizeof(int16_t)));

    for (size_t i = 0; i < totalSamples; ++i) {
        for (int c = 0; c < outChannels; ++c) {
            float val = 0.0f;
            if (static_cast<size_t>(c) < numChannels && channels[c])
                val = channels[c]->get_value_at(static_cast<uint64_t>(i));
            val = std::max(-1.0f, std::min(1.0f, val));
            int16_t sample = static_cast<int16_t>(val * 32767.0f);
            audioData.append(reinterpret_cast<const char *>(&sample),
                             static_cast<int>(sizeof(int16_t)));
        }
    }

    // Set up Qt Multimedia audio format.
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(outChannels);
    format.setSampleFormat(QAudioFormat::Int16);

    // Create audio sink (uses default audio output device).
    QAudioSink sink(format);
    if (sink.error() != QAudio::NoError) {
        emit playbackError("QAudioSink initialization failed");
        return;
    }

    // Wrap PCM data in a QBuffer for sequential reading by the sink.
    QBuffer buffer(&audioData);
    buffer.open(QIODevice::ReadOnly);

    sink.start(&buffer);

    _playing.store(true);
    emit playbackStarted();

    // Local event loop to handle state changes and stop requests.
    QEventLoop eventLoop;

    // When the sink goes Idle, data is exhausted.
    QObject::connect(&sink, &QAudioSink::stateChanged,
        &eventLoop, [&eventLoop, &sink, &buffer, this, loop](QAudio::State state) {
            if (state == QAudio::IdleState) {
                if (loop && _playing.load()) {
                    // Rewind buffer and restart for loop playback.
                    buffer.seek(0);
                    sink.start(&buffer);
                } else {
                    eventLoop.quit();
                }
            }
        });

    // Periodic stop-request check.
    QTimer stopCheckTimer;
    stopCheckTimer.setInterval(100);
    QObject::connect(&stopCheckTimer, &QTimer::timeout,
        &eventLoop, [this, &sink, &eventLoop]() {
            if (!_playing.load()) {
                sink.stop();
                eventLoop.quit();
            }
        });
    stopCheckTimer.start();

    eventLoop.exec();

    sink.stop();
    _playing.store(false);
    emit playbackStopped();
}

void DecoderAudioPlayerWorker::doStop()
{
    _playing.store(false);
}

// =========================================================================
// DecoderAudioPlayer (singleton)
// =========================================================================

DecoderAudioPlayer::DecoderAudioPlayer()
    : _thread(new QThread())
    , _worker(new DecoderAudioPlayerWorker())
{
    _worker->moveToThread(_thread);
    connect(_worker, &DecoderAudioPlayerWorker::playbackStarted,
            this, &DecoderAudioPlayer::playbackStarted);
    connect(_worker, &DecoderAudioPlayerWorker::playbackStopped,
            this, &DecoderAudioPlayer::playbackStopped);
    connect(_worker, &DecoderAudioPlayerWorker::playbackError,
            this, &DecoderAudioPlayer::playbackError);
    connect(_worker, &DecoderAudioPlayerWorker::playbackStopped,
            this, [this]() { _playing.store(false); });
    connect(_worker, &DecoderAudioPlayerWorker::playbackStarted,
            this, [this]() { _playing.store(true); });
    _thread->start();
}

DecoderAudioPlayer::~DecoderAudioPlayer()
{
    stop();
    _worker->deleteLater();
    _thread->quit();
    _thread->wait(3000);
    delete _thread;
}

DecoderAudioPlayer& DecoderAudioPlayer::instance()
{
    static DecoderAudioPlayer s_instance;
    return s_instance;
}

bool DecoderAudioPlayer::start(
    const std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> &channels,
    int sampleRate, bool loop)
{
    if (_playing.load())
        stop();

    QMetaObject::invokeMethod(_worker, "doPlay",
        Qt::QueuedConnection,
        Q_ARG(const std::vector<std::shared_ptr<pv::data::DecoderAnalogData>>&, channels),
        Q_ARG(int, sampleRate),
        Q_ARG(bool, loop));
    return true;
}

void DecoderAudioPlayer::stop()
{
    QMetaObject::invokeMethod(_worker, "doStop", Qt::QueuedConnection);
}

bool DecoderAudioPlayer::playDecoder(DecodeTrace *dt, const PlayConfig &cfg,
                                     QString &message)
{
    if (!dt) {
        message = QStringLiteral("No decode trace");
        return false;
    }

    auto stack = dt->decoder();
    if (!stack) {
        message = QStringLiteral("No decoder stack");
        return false;
    }

    auto analog_data = stack->analog_data_copy();
    if (analog_data.empty()) {
        message = QStringLiteral("No analog data");
        return false;
    }

    // Determine total sample count.
    size_t totalSamples = 0;
    for (const auto &ch : analog_data) {
        if (!ch) continue;
        totalSamples = std::max(totalSamples, ch->get_sample_count());
    }
    if (totalSamples == 0) {
        message = QStringLiteral("No samples");
        return false;
    }

    if (!cfg.mix.empty()) {
        // Mix-matrix mode: create mixed output channels.
        const int outChannels = std::max(1, std::min(cfg.channels, 8));
        std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> mixed;

        for (int out = 0; out < outChannels; ++out) {
            auto out_data = std::make_shared<pv::data::DecoderAnalogData>(
                out, outChannels, "mixed_" + std::to_string(out));

            for (size_t i = 0; i < totalSamples; ++i) {
                float val = 0.0f;
                for (const auto &row : cfg.mix) {
                    if (!row.enabled) continue;
                    for (const auto &ad : analog_data) {
                        if (ad && ad->channel() == row.channel) {
                            val += ad->get_value_at(static_cast<uint64_t>(i))
                                   * row.outputs[(size_t)out];
                            break;
                        }
                    }
                }
                out_data->append_sample(static_cast<uint64_t>(i),
                                        static_cast<uint64_t>(i) + 1, val);
            }
            mixed.push_back(out_data);
        }

        return start(mixed, static_cast<int>(cfg.sample_rate), cfg.repeat);
    } else {
        // Simple selection mode.
        std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> selected;

        if (!cfg.channel_indices.empty()) {
            for (int idx : cfg.channel_indices) {
                for (const auto &ad : analog_data) {
                    if (ad && ad->channel() == idx) {
                        selected.push_back(ad);
                        break;
                    }
                }
            }
        } else {
            // Select all visible channels.
            for (const auto &ad : analog_data) {
                if (ad && ad->visible())
                    selected.push_back(ad);
            }
        }

        if (selected.empty()) {
            message = QStringLiteral("No channels selected");
            return false;
        }

        return start(selected, static_cast<int>(cfg.sample_rate), cfg.repeat);
    }
}

std::vector<DecoderAudioPlayer::OutputDevice> DecoderAudioPlayer::listOutputDevices()
{
    std::vector<OutputDevice> result;
    const auto devices = QMediaDevices::audioOutputs();
    for (int i = 0; i < devices.size(); ++i) {
        OutputDevice dev;
        dev.id = i;
        dev.name = devices[i].description();
        int ch = devices[i].preferredFormat().channelCount();
        dev.max_channels = std::max(1, std::min(ch, 8));
        result.push_back(dev);
    }
    if (result.empty()) {
        result.push_back({0, QStringLiteral("Default Audio Output"), 2});
    }
    return result;
}

void DecoderAudioPlayer::loadPlayConfig(PlayConfig &cfg)
{
    QSettings settings;
    settings.beginGroup("DecoderAudioPlayer");
    cfg.sample_rate = settings.value("sample_rate", 44100).toUInt();
    cfg.bits = settings.value("bits", 16).toInt();
    cfg.channels = settings.value("channels", 2).toInt();
    cfg.device_id = settings.value("device_id", 0).toInt();
    cfg.repeat = settings.value("repeat", false).toBool();
    settings.endGroup();
}

void DecoderAudioPlayer::savePlayConfig(const PlayConfig &cfg)
{
    QSettings settings;
    settings.beginGroup("DecoderAudioPlayer");
    settings.setValue("sample_rate", cfg.sample_rate);
    settings.setValue("bits", cfg.bits);
    settings.setValue("channels", cfg.channels);
    settings.setValue("device_id", cfg.device_id);
    settings.setValue("repeat", cfg.repeat);
    settings.endGroup();
}

} // namespace view
} // namespace pv
