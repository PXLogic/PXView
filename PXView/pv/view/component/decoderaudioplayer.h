#ifndef PXVIEW_PV_VIEW_COMPONENT_DECODERAUDIOPLAYER_H
#define PXVIEW_PV_VIEW_COMPONENT_DECODERAUDIOPLAYER_H

#include <QObject>
#include <QString>
#include <memory>
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <array>

namespace pv {
namespace data { class DecoderAnalogData; }
namespace view {

class DecodeTrace;

/**
 * DecoderAudioPlayerWorker — QObject worker that runs on a worker thread
 * and manages Qt Multimedia playback of decoder-generated analog audio.
 *
 * The worker receives sample data from the UI thread via queued signals
 * and manages the QAudioSink lifecycle (format / feed buffer / stop).
 */
class DecoderAudioPlayerWorker : public QObject {
    Q_OBJECT

public:
    explicit DecoderAudioPlayerWorker(QObject *parent = nullptr);
    ~DecoderAudioPlayerWorker();

public slots:
    void doPlay(const std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> &channels,
                int sampleRate, bool loop);
    void doStop();

signals:
    void playbackStarted();
    void playbackStopped();
    void playbackError(const QString &msg);

private:
    std::atomic<bool> _playing{false};
};

/**
 * DecoderAudioPlayer — singleton managing audio playback of decoder
 * analog data (TDM audio, PWM waveform, etc.).
 *
 * The player creates a worker thread + worker QObject to keep QAudioSink
 * calls off the GUI thread. The GUI thread interacts via start() / stop().
 */
class DecoderAudioPlayer : public QObject {
    Q_OBJECT

public:
    /** Audio output device descriptor. */
    struct OutputDevice {
        int id = 0;
        QString name;
        int max_channels = 2;
    };

    /** Playback configuration with optional multi-channel mix matrix. */
    struct PlayConfig {
        struct MixRow {
            int channel = 0;
            bool enabled = false;
            std::array<float, 8> outputs{};
        };
        uint32_t sample_rate = 44100;
        int bits = 16;
        int channels = 2;
        int device_id = 0;
        bool repeat = false;
        std::vector<MixRow> mix;
        std::vector<int> channel_indices;
    };

    static DecoderAudioPlayer& instance();

    // Start playback. Returns false if no data or device open fails.
    bool start(const std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> &channels,
               int sampleRate, bool loop = false);

    // Stop playback (non-blocking: signals the worker thread).
    void stop();

    // Is currently playing?
    bool is_playing() const { return _playing.load(); }
    bool isPlaying() const { return _playing.load(); }

    // Play decoder audio with full config (mix matrix, device, etc.).
    bool playDecoder(DecodeTrace *dt, const PlayConfig &cfg, QString &message);

    // List available audio output devices.
    static std::vector<OutputDevice> listOutputDevices();

    // Persist / restore play config via QSettings.
    static void loadPlayConfig(PlayConfig &cfg);
    static void savePlayConfig(const PlayConfig &cfg);

signals:
    void playbackStarted();
    void playbackStopped();
    void playbackError(const QString &msg);

private:
    DecoderAudioPlayer();
    ~DecoderAudioPlayer();
    DecoderAudioPlayer(const DecoderAudioPlayer&) = delete;
    DecoderAudioPlayer& operator=(const DecoderAudioPlayer&) = delete;

    class QThread *_thread;
    DecoderAudioPlayerWorker *_worker;
    std::atomic<bool> _playing{false};
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_COMPONENT_DECODERAUDIOPLAYER_H
