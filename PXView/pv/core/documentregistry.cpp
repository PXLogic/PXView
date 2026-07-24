#include "documentregistry.h"

#include "eventbus.h"
#include "sessionstatecontext.h"
#include "../data/decoderstack.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../sigsession.h"

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
  _registry->_state->set_is_working(true);
  _registry->_event_bus->broadcast_async<interface::CaptureOwnerChanged>(
      {nullptr, _registry->get_capture_owner_document()});
}

DocumentRegistry::CaptureOwnerGuard::~CaptureOwnerGuard() {
  if (_registry) {
    // join_copy_thread() MUST be outside the lock — the copy thread may
    // need to acquire _capture_state_mutex (e.g. in copy_to_doc_done).
    _registry->join_copy_thread();
    {
      std::lock_guard<std::mutex> lock(_registry->_capture_state_mutex);
      _registry->_capture_owner_index = SIZE_MAX;
      _registry->_state->set_is_working(false);
    }
    // Broadcast outside the lock to minimize critical section and avoid
    // listener callbacks re-entering the mutex.
    _registry->_event_bus->broadcast_async<interface::CaptureOwnerChanged>(
        {nullptr, _registry->get_capture_owner_document()});
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
      // join_copy_thread() MUST be outside the lock — see destructor note.
      _registry->join_copy_thread();
      {
        std::lock_guard<std::mutex> lock(_registry->_capture_state_mutex);
        _registry->_capture_owner_index = SIZE_MAX;
        _registry->_state->set_is_working(false);
      }
      _registry->_event_bus->broadcast_async<interface::CaptureOwnerChanged>(
        {nullptr, _registry->get_capture_owner_document()});
    }
    _registry = o._registry;
    _doc_index = o._doc_index;
    o._registry = nullptr;
  }
  return *this;
}

// ---------------------------------------------------------------------------
// DocumentRegistry
// ---------------------------------------------------------------------------

DocumentRegistry::DocumentRegistry(EventBus *bus, SessionStateContext *state)
    : _event_bus(bus), _state(state),
      _active_document_index(SIZE_MAX), _capture_owner_index(SIZE_MAX),
      _copy_in_progress(false) {}

DocumentRegistry::~DocumentRegistry() {
  // Join any in-flight copy thread before destruction (a joinable std::thread
  // would otherwise std::terminate on destruction). Owned documents are freed
  // automatically by ~unique_ptr in _owned_documents.
  join_copy_thread();
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
  return take_document(std::make_unique<pv::data::SessionDocument>(session));
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
      {nullptr, get_active_document()});
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
        stack->_delete_flag = true;
      }
    }
    stacks.clear();
  }
}

void DocumentRegistry::clear_capture_owner_document(data::SessionDocument *doc) {
  // Task 4: Guard-managed — reset the guard when the caller asks to clear the
  // document that is currently the capture owner. Guard destructor handles
  // join_copy_thread() + owner clear + _is_working=false + broadcast.
  // C4 fix: lock the mutex to get a consistent snapshot of
  // _capture_owner_guard and _capture_owner_index. The guard.reset() call
  // happens OUTSIDE the lock — the guard destructor calls join_copy_thread()
  // which could block, and we must not hold the mutex during that (the copy
  // thread may need to acquire _capture_state_mutex in copy_to_doc_done).
  std::unique_ptr<CaptureOwnerGuard> guard_to_reset;
  {
    std::lock_guard<std::mutex> lock(_capture_state_mutex);
    if (_capture_owner_guard &&
        get_document_by_index(_capture_owner_index) == doc) {
      guard_to_reset = std::move(_capture_owner_guard);
    }
  }
  // Reset outside the lock — guard destructor calls join_copy_thread()
  // which could block, and we don't want to hold the mutex during that.
  guard_to_reset.reset();
}

void DocumentRegistry::join_copy_thread() {
  if (_copy_thread.joinable()) {
    _copy_thread.join();
  }
}

void DocumentRegistry::acquire_capture_owner(data::SessionDocument *doc) {
  size_t idx = doc ? find_index_for_document(doc) : SIZE_MAX;
  _capture_owner_guard = std::make_unique<CaptureOwnerGuard>(this, idx);
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

} // namespace core
} // namespace pv
