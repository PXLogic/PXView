#ifndef PXVIEW_PV_DATA_ICAPTURE_CONTROL_H
#define PXVIEW_PV_DATA_ICAPTURE_CONTROL_H

#include <cstdint>

namespace pv {
namespace data {

class SessionDocument;

// ICaptureControl — capture lifecycle control.
// Spec v2 Task 8: extracted from DataSource胖接口.
class ICaptureControl {
public:
    virtual ~ICaptureControl() = default;
    virtual bool start_capture(bool instant = false, SessionDocument *owner = nullptr) = 0;
    virtual bool stop_capture() = 0;
    virtual bool is_working() = 0;
    virtual bool is_instant() = 0;
    virtual bool is_repeating() = 0;
    virtual bool is_stopped_status() = 0;
    virtual bool is_running_status() = 0;
    virtual void refresh(int holdtime) = 0;
    virtual void auto_end() = 0;
    // Spec v3 Task 3: additional capture-related methods needed by toolbars
    virtual bool is_realtime_refresh() = 0;
    virtual bool is_repeat_mode() = 0;
    virtual void session_save() = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_ICAPTURE_CONTROL_H
