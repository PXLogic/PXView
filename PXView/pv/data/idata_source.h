#ifndef PXVIEW_PV_DATA_IDATA_SOURCE_H
#define PXVIEW_PV_DATA_IDATA_SOURCE_H

#include <cstdint>

namespace pv {
namespace data {

class LogicSnapshot;
class AnalogSnapshot;
class DsoSnapshot;
class Snapshot;

// IDataSource — data snapshots + sample rate (Signal rendering required subset).
// Spec v2 Task 8: extracted from DataSource胖接口 to satisfy ISP.
// Signal渲染类只需要数据快照+采样率,不需要看到解码器/采集/游标方法。
class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual uint64_t cur_snap_samplerate() = 0;
    virtual uint64_t cur_samplelimits() = 0;
    virtual double cur_snap_sampletime() = 0;
    virtual data::LogicSnapshot* get_logic_snapshot() = 0;
    virtual data::AnalogSnapshot* get_analog_snapshot() = 0;
    virtual data::DsoSnapshot* get_dso_snapshot() = 0;
    virtual data::Snapshot* get_snapshot(int type) = 0;
    virtual bool have_view_data() = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_IDATA_SOURCE_H
