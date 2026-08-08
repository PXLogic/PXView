#ifndef PXVIEW_CORE_DATAFEEDPARSER_H
#define PXVIEW_CORE_DATAFEEDPARSER_H

#include <libsigrok/libsigrok.h>
#include "pv/core/isession_coordination.h"
#include "pv/core/isession_state.h"
#include "pv/core/isession_state.h"

namespace pv {

namespace core {

class EventBus;
class SessionStateContext;
class CaptureManager;
class DecodeTaskManager;

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
 *
 * Cross-manager coordination (EventBus dispatch + state mutation) goes
 * through ISessionCoordination* — which is a pure abstraction with no
 * dependency on concrete manager types. Typed access to CaptureManager /
 * DecodeTaskManager state is injected directly via set_managers() by
 * SigSession (CaptureManager is constructed after DataFeedParser), keeping
 * ISessionCoordination free of any concrete-manager coupling.
 */
class DataFeedParser {
public:
  DataFeedParser(EventBus *bus, ISessionState *state, ISessionCoordination *coord);
  ~DataFeedParser();

  // Static trampoline registered with libsigrok. user_data is a DataFeedParser*.
  static void data_feed_callback_ex(const struct sr_dev_inst *sdi,
                                    const struct sr_datafeed_packet *packet,
                                    void *user_data);

  void data_feed_in(const struct sr_dev_inst *sdi,
                    const sr_datafeed_packet *packet);

  // SigSession injects the concrete manager pointers after construction
  // (CaptureManager is built after DataFeedParser). DataFeedParser needs
  // typed access to these for capture/decode state queries, but this is
  // NOT routed through ISessionCoordination (which stays concrete-free).
  void set_managers(CaptureManager *capture_mgr,
                    DecodeTaskManager *decode_mgr) {
    _capture_mgr = capture_mgr;
    _decode_mgr = decode_mgr;
  }

private:
  void feed_in_header(const sr_dev_inst *sdi);
  void feed_in_meta(const sr_dev_inst *sdi, const sr_datafeed_meta &meta);
  void feed_in_trigger();
  void feed_in_logic(const sr_datafeed_logic &o);
  void feed_in_analog(const sr_datafeed_analog &o);
  void feed_in_dso(const sr_datafeed_dso &o);

  EventBus *_event_bus;
  ISessionState *_state;
  ISessionCoordination *_coord;
  CaptureManager *_capture_mgr = nullptr;
  DecodeTaskManager *_decode_mgr = nullptr;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_DATAFEEDPARSER_H
