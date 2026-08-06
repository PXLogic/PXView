#ifndef PXVIEW_PV_DATA_IMEASURE_SOURCE_H
#define PXVIEW_PV_DATA_IMEASURE_SOURCE_H

#include <cstdint>
#include <vector>

#include "../api/types.h"  // api::MeasurementValue
#include "../core/cursorregistry.h"  // core::CursorEntry

namespace pv {
namespace data {

// IMeasureSource — measurement + cursors (MCP / View measurement required subset).
// Spec v2 Task 8: extracted from DataSource胖接口.
class IMeasureSource {
public:
    virtual ~IMeasureSource() = default;
    virtual std::vector<api::MeasurementValue> get_measurements(
        int channel_index = -1,
        int view_rect_height = 0) = 0;
    virtual std::vector<core::CursorEntry> get_cursors() const = 0;
    virtual int add_cursor(uint64_t sample_position) = 0;
    virtual bool remove_cursor(int index) = 0;
    virtual bool set_cursor_position(int index, uint64_t sample_position) = 0;
    virtual void clear_cursors() = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_IMEASURE_SOURCE_H
