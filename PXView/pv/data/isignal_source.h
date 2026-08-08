#ifndef PXVIEW_PV_DATA_ISIGNAL_SOURCE_H
#define PXVIEW_PV_DATA_ISIGNAL_SOURCE_H

#include <cstdint>
#include <memory>
#include <vector>

class DeviceAgent;  // global namespace (defined in deviceagent.h)

namespace pv {
namespace data {

class SignalModel;
class TriggerConfig;
class LissajousModel;

// ISignalSource — signal models + trigger config.
// Spec v2 Task 8: extracted from DataSource胖接口.
class ISignalSource {
public:
    virtual ~ISignalSource() = default;
    // Returns a reference to the live signal models vector. Callers that
    // access it from non-UI threads MUST hold a shared_lock on
    // signal_models_mutex() for the duration of their access.
    virtual std::vector<std::shared_ptr<SignalModel>>& get_signal_models() = 0;
    // TS-2 fix: thread-safe snapshot — returns a value copy of the signal
    // models vector under a shared_lock. Safe to call from any thread
    // without manually acquiring the mutex. Prefer this over
    // get_signal_models() when you only need to iterate the models.
    virtual std::vector<std::shared_ptr<SignalModel>> get_signal_models_snapshot() = 0;
    virtual const TriggerConfig& trigger_config() const = 0;
    virtual uint8_t trigd_ch() = 0;
    virtual bool trigd() = 0;
    // Spec v3 Task 3: device() and get_lissajous_model() added here because
    // Dock/Dialog classes that need signal config also need device access.
    virtual DeviceAgent* device() = 0;
    virtual LissajousModel* get_lissajous_model() = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_ISIGNAL_SOURCE_H
