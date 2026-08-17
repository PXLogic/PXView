#include "pv/core/documentregistry.h"

#include "pv/core/eventbus.h"
#include "pv/core/sessionstatecontext.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"

#include <algorithm>

namespace pv {
namespace core {

// ---------------------------------------------------------------------------
// CaptureOwnerGuard
// ---------------------------------------------------------------------------

DocumentRegistry::CaptureOwnerGuard::CaptureOwnerGuard(DocumentRegistry *reg,
                                                       size_t doc_index)
    : _registry(reg), _doc_index(doc_index) {
  std::lock_guard<std::mutex> lock(_registry->_capture_state_mutex);
  _registry->_capture_owner_index = _doc_index;
  _registry->_coord->set_is_working(true);
  _registry->_event_bus->broadcast_async<interface::CaptureOwnerChanged>(
      {SIZE_MAX, _doc_index});
}

DocumentRegistry::CaptureOwnerGuard::~CaptureOwnerGuard() {
  if (_registry) {
    release();
  }
}

DocumentRegistry::CaptureOwnerGuard::CaptureOwnerGuard(CaptureOwnerGuard &&o) noexcept
    : _registry(o._registry), _doc_index(o._doc_index) {
  o._registry = nullptr;
}

DocumentRegistry::CaptureOwnerGuard &
DocumentRegistry::CaptureOwnerGuard::operator=(CaptureOwnerGuard &&o) noexcept {
  if (this != &o) {
    if (_registry) {
      release();
    }
    _registry = o._registry;
    _doc_index = o._doc_index;
    o._registry = nullptr;
  }
  return *this;
}

// ---------------------------------------------------------------------------
// CaptureOwnerGuard::release() — Track C4: shared cleanup logic
// ---------------------------------------------------------------------------
void DocumentRegistry::CaptureOwnerGuard::release() {
// Gap 3: join_copy_thread removed — copy_data_to_document is now
// zero-copy (instant), no background thread to join.
{
std::lock_guard<std::mutex> lock(_registry->_capture_state_mutex);
_registry->_capture_owner_index = SIZE_MAX;
    _registry->_coord->set_is_working(false);
  }
  // Broadcast outside the lock to minimize critical section and avoid
  // listener callbacks re-entering the mutex. The owner index was already
  // reset to SIZE_MAX above, so the previous owner is reported via _doc_index.
  _registry->_event_bus->broadcast_async<interface::CaptureOwnerChanged>(
      {_doc_index, SIZE_MAX});
}

// ---------------------------------------------------------------------------
// DocumentRegistry
// ---------------------------------------------------------------------------

DocumentRegistry::DocumentRegistry(EventBus *bus, ISessionState *state, ISessionCoordination *coord)
    : _event_bus(bus), _state(state), _coord(coord),
      _active_document_index(SIZE_MAX), _capture_owner_index(SIZE_MAX),
      _copy_in_progress(false) {}

DocumentRegistry::~DocumentRegistry() {
// Gap 3: no copy thread to join — copy_data_to_document is zero-copy.
}

size_t DocumentRegistry::take_document(
    std::unique_ptr<data::SessionDocument> doc) {
  if (!doc)
    return SIZE_MAX;
  size_t index = _owned_documents.size();
  _owned_documents.push_back(std::move(doc));
  return index;
}

void DocumentRegistry::release_document(size_t index) {
  // Marked deletion: reset the unique_ptr (frees the document) but keep the
  // slot so all other indices stay stable. Safe to call with SIZE_MAX or an
  // already-released/out-of-range index.
  if (index == SIZE_MAX || index >= _owned_documents.size())
    return;
  _owned_documents[index].reset();
}

size_t DocumentRegistry::create_api_document(pv::SigSession *session) {
  return take_document(
      std::make_unique<pv::data::SessionDocument>(session->device()));
}

data::SessionDocument *
DocumentRegistry::get_document_by_index(size_t index) const {
  if (index == SIZE_MAX || index >= _owned_documents.size())
    return nullptr;
  return _owned_documents[index].get();
}

size_t DocumentRegistry::find_index_for_document(
    data::SessionDocument *doc) const {
  if (!doc)
    return SIZE_MAX;
  for (size_t i = 0; i < _owned_documents.size(); ++i) {
    if (_owned_documents[i].get() == doc)
      return i;
  }
  return SIZE_MAX;
}

void DocumentRegistry::set_active_document(data::SessionDocument *doc) {
  size_t new_index = doc ? find_index_for_document(doc) : SIZE_MAX;
  if (_active_document_index == new_index) // 去重，避免重复广播
    return;
  _active_document_index = new_index;
  // R1: notify listeners that the active document changed.
  _event_bus->broadcast_async<interface::ActiveDocumentChanged>(
      {SIZE_MAX, new_index});
}

std::vector<data::SessionDocument *>
DocumentRegistry::get_all_documents() const {
  std::vector<data::SessionDocument *> result;
  result.reserve(_owned_documents.size());
  for (const auto &ptr : _owned_documents) {
    if (ptr)
      result.push_back(ptr.get());
  }
  return result;
}

void DocumentRegistry::clear_all_documents_decoders() {
  for (auto &ptr : _owned_documents) {
    if (!ptr)
      continue;
    auto &stacks = ptr->get_decoder_stacks();
    for (auto stack : stacks) {
      if (stack->IsRunning()) {
        // P0-3 fix: _delete_flag removed — just stop the work, shared_ptr
        // manages the lifetime when the stacks vector is cleared below.
        stack->stop_decode_work();
      }
    }
    stacks.clear();
  }
}

void DocumentRegistry::clear_active_document_decoders() {
  // 问题2修复：设备切换（set_device）时只清活动文档的解码器栈，避免非活动
  // 文档（如 pxl 标签页的文档）的解码器被误清。所有运行中的解码任务仍会
  // 停止（解码线程可能持有指向被释放数据的指针），但只有活动文档的栈被清空。
  for (auto &ptr : _owned_documents) {
    if (!ptr || ptr.get() != get_active_document())
      continue;
    auto &stacks = ptr->get_decoder_stacks();
    for (auto stack : stacks) {
      if (stack->IsRunning())
        stack->stop_decode_work();
    }
    stacks.clear();
  }
}

void DocumentRegistry::clear_capture_owner_document(data::SessionDocument *doc) {
  // Task 4: Guard-managed — reset the guard when the caller asks to clear the
  // document that is currently the capture owner. Guard destructor handles
  // join_copy_thread() + owner clear + _is_working=false + broadcast.
// C4 fix: lock the mutex to get a consistent snapshot of
// _capture_owner_guard and _capture_owner_index.
std::unique_ptr<CaptureOwnerGuard> guard_to_reset;
{
std::lock_guard<std::mutex> lock(_capture_state_mutex);
if (_capture_owner_guard &&
get_document_by_index(_capture_owner_index.load(std::memory_order_acquire)) == doc) {
guard_to_reset = std::move(_capture_owner_guard);
}
}
// Reset outside the lock.
guard_to_reset.reset();
}


void DocumentRegistry::acquire_capture_owner(data::SessionDocument *doc) {
  size_t idx = doc ? find_index_for_document(doc) : SIZE_MAX;
  // CRITICAL FIX (repeat→next freeze): A CaptureOwnerGuard may already be held
  // (repeat mode keeps it alive across auto-stopped frames). The straightforward
  // `_capture_owner_guard = make_unique<...>(new)` would move-assign over the
  // OLD guard: move-assignment first calls the OLD guard's release(), which
  // clears `_capture_owner_index` and sets `_is_working=false` AFTER the new
  // guard's constructor set them to true/idx. The final state is therefore
  // owner=SIZE_MAX + is_working=false while a capture is genuinely running →
  // view refresh, realtime gating and stop_capture (guarded by is_working())
  // all stop → the UI freezes (data still streams underneath, PathDiag advances).
  // Fix: move the old guard OUT and reset it (releasing is_working) BEFORE
  // constructing the new one, so the assignment below targets a null member
  // and never triggers another release.
  std::unique_ptr<CaptureOwnerGuard> old_guard;
  {
    std::lock_guard<std::mutex> lock(_capture_state_mutex);
    old_guard = std::move(_capture_owner_guard);
  }
  old_guard.reset();  // release old (is_working=true→false) — outside the lock

  std::unique_ptr<CaptureOwnerGuard> new_guard =
      std::make_unique<CaptureOwnerGuard>(this, idx);
  {
    std::lock_guard<std::mutex> lock(_capture_state_mutex);
    _capture_owner_guard = std::move(new_guard);
  }
}

void DocumentRegistry::release_capture_owner() {
  // Thread-safe reset: worker thread (SR_DF_END path in datafeedparser.cpp)
  // and main thread (action_stop_capture) can both reach here. Without the
  // lock, concurrent unique_ptr::reset() on the same guard is a data race
  // (double-free → heap corruption). Move the guard out under the lock, then
  // reset outside (guard destructor joins copy thread, which must not hold
  // the mutex — see clear_capture_owner_document for the same pattern).
  std::unique_ptr<CaptureOwnerGuard> guard_to_reset;
  {
    std::lock_guard<std::mutex> lock(_capture_state_mutex);
    if (_capture_owner_guard)
      guard_to_reset = std::move(_capture_owner_guard);
  }
  guard_to_reset.reset();
}

bool DocumentRegistry::has_capture_owner() const {
  std::lock_guard<std::mutex> lock(_capture_state_mutex);
  return _capture_owner_guard != nullptr;
}

} // namespace core
} // namespace pv
