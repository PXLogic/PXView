#ifndef PXVIEW_CORE_DATAFEEDPARSER_H
#define PXVIEW_CORE_DATAFEEDPARSER_H

#include <libsigrok/libsigrok.h>

namespace pv {

namespace core {

class EventBus;
class SessionStateContext;

/**
 * DataFeedParser — owns the data feed callback trampoline and the feed_in_*
 * packet dispatch methods. Extracted from SigSession (SubTask 10.4) as a
 * mechanical refactoring: no behavior change.
 *
 * The parser holds an injected EventBus* (for typed event dispatch via
 * broadcast_async<T>/broadcast_sync<T>) and a SessionStateContext* (for
 * accessing capture_data / view_data / device_agent / is_triged / data_lock
 * / etc.). modernize-core-layer-radical phase 1 replaced the previous
 * SigSession* + friend-declaration coupling.
 */
class DataFeedParser {
public:
  DataFeedParser(EventBus *bus, SessionStateContext *state);
  ~DataFeedParser();

  // Static trampoline registered with libsigrok. user_data is a DataFeedParser*.
  static void data_feed_callback_ex(const struct sr_dev_inst *sdi,
                                    const struct sr_datafeed_packet *packet,
                                    void *user_data);

  void data_feed_in(const struct sr_dev_inst *sdi,
                    const struct sr_datafeed_packet *packet);

private:
  void feed_in_header(const sr_dev_inst *sdi);
  void feed_in_meta(const sr_dev_inst *sdi, const sr_datafeed_meta &meta);
  void feed_in_trigger();
  void feed_in_logic(const sr_datafeed_logic &o);
  void feed_in_analog(const sr_datafeed_analog &o);
  void feed_in_dso(const sr_datafeed_dso &o);

  EventBus *_event_bus;
  // Shared session state (capture_data / view_data / device_agent /
  // is_triged / trig_time / trigger_flag / trigger_ch / hw_replied /
  // error / data_mutex / decode_task_manager /
  // capture_manager / spectrum_stacks / math_stack / receive_header() /
  // receive_trigger() / frame_began() / frame_ended() / session_error() /
  // set_receive_data_len() / set_cur_snap_samplerate() / set_session_time() /
  // data_lock() / get_ch_num()) accessed via SessionStateContext accessors.
  // modernize-core-layer-radical phase 1 replaced the previous SigSession* +
  // friend-declaration coupling.
  SessionStateContext *_state;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_DATAFEEDPARSER_H
