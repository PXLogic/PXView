#ifndef PXVIEW_CORE_DOCUMENTREGISTRY_H
#define PXVIEW_CORE_DOCUMENTREGISTRY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pv/data/document/sessiondocument.h"
#include "pv/core/isession_coordination.h"
#include "pv/core/isession_state.h"
#include "pv/core/isession_state.h"

namespace pv {

class SigSession;

namespace core {

class EventBus;
class SessionStateContext;

/**
 * DocumentRegistry — owns the SessionDocument list and the capture-owner
 * lifecycle (CaptureOwnerGuard + copy thread + capture_state_mutex).
 *
 * ---------------------- modernize-core-layer-radical phase 2 ----------------------
 * OWNERSHIP SEMANTICS (phase 2: ownership moved up into DocumentRegistry):
 *
 * `_owned_documents` is a vector of unique_ptr<SessionDocument>. All
 * SessionDocument instances (per-tab documents created by MainWindow, and
 * MCP-dedicated documents created by SessionService/AppService) are now owned
 * by this registry. External code holds weak pointers obtained via
 * get_document_by_index() and a stable size_t index.
 *
 * Release model: release_document(index) uses MARKED DELETION — it resets the
 * unique_ptr at the given index (freeing the document) but does NOT erase the
 * vector element, so all remaining indices stay stable. Released slots become
 * nullptr and are skipped by get_all_documents() / clear_all_documents_decoders().
 *
 * Active / capture-owner state is tracked by index (size_t, SIZE_MAX == none)
 * rather than by raw pointer. Accessors return weak pointers via
 * get_document_by_index() for backward compatibility with callers that still
 * pass SessionDocument* around (View layer, SigSession facade).
 * --------------------------------------------------------------------------- */
class DocumentRegistry {
public:
// RAII guard for _capture_owner_index + _is_working lifecycle.
// Constructed in start_capture on success; destructed in stop_capture /
// clear_capture_owner_document (tab close). Manages owner index + _is_working
// flag + CaptureOwnerChanged broadcast as a single unit,
// eliminating manual clear_capture_owner_document() calls and use-after-free
// risk on the background copy thread.
//
// NOTE: copy_to_doc_done does NOT reset this guard — in repeat mode the owner
// field is cleared per-frame but _is_working must stay true across frames.
  // The guard persists for the whole capture session.
  class CaptureOwnerGuard {
  public:
    CaptureOwnerGuard(DocumentRegistry *reg, size_t doc_index);
    ~CaptureOwnerGuard();
    // Disable copy
    CaptureOwnerGuard(const CaptureOwnerGuard &) = delete;
    CaptureOwnerGuard &operator=(const CaptureOwnerGuard &) = delete;
    // Allow move
    CaptureOwnerGuard(CaptureOwnerGuard &&o) noexcept;
    CaptureOwnerGuard &operator=(CaptureOwnerGuard &&o) noexcept;
    inline data::SessionDocument *doc() const {
      return _registry ? _registry->get_document_by_index(_doc_index) : nullptr;
    }
    inline size_t doc_index() const { return _doc_index; }

  private:
    // Track C4: Extracted release() method to de-duplicate the cleanup
    // logic shared between destructor and move-assignment operator.
    void release();
    DocumentRegistry *_registry;
    size_t _doc_index;
  };

public:
  DocumentRegistry(EventBus *bus, ISessionState *state, ISessionCoordination *coord);
  ~DocumentRegistry();

  // --- Document list management (ownership) ---
  // Takes ownership of doc; returns the assigned stable index, or SIZE_MAX on
  // nullptr input.
  size_t take_document(std::unique_ptr<data::SessionDocument> doc);
  // Marked deletion: resets the unique_ptr at index (frees the document), keeps
  // the slot (index stays stable, vector does not shrink). Safe to call with
  // SIZE_MAX or an already-released/out-of-range index (no-op).
  void release_document(size_t index);
  // Factory: creates a SessionDocument owned by this registry and returns its
  // index. Used by the API layer (SessionService/AppService).
  size_t create_api_document(pv::SigSession *session);

  // Weak-pointer accessor by index. Returns nullptr if index is out of range
  // or the slot has been released (nullptr unique_ptr).
  data::SessionDocument *get_document_by_index(size_t index) const;

  void set_active_document(data::SessionDocument *doc);
  inline data::SessionDocument *get_active_document() const {
    return get_document_by_index(_active_document_index);
  }
  inline size_t get_active_document_index() const {
    return _active_document_index;
  }
  // Returns a snapshot vector of non-null raw pointers (weak references) for
  // iteration. Returned by value because ownership is internal to the registry.
  std::vector<data::SessionDocument *> get_all_documents() const;
  void clear_all_documents_decoders();
  // 问题2修复：设备切换时只清活动文档的解码器，非活动文档的解码器保留。
  void clear_active_document_decoders();

  // --- Capture owner / copy thread ---
  inline data::SessionDocument *get_capture_owner_document() const {
    return get_document_by_index(_capture_owner_index.load(std::memory_order_acquire));
  }
  inline size_t get_capture_owner_index() const { return _capture_owner_index.load(std::memory_order_acquire); }
inline bool is_copy_in_progress() const { return _copy_in_progress; }
void clear_capture_owner_document(data::SessionDocument *doc);

  // Called by start_capture to acquire the capture owner guard.
  void acquire_capture_owner(data::SessionDocument *doc);

  // Called by stop_capture / action_stop_capture to release the guard.
  void release_capture_owner();

  // Returns true if the capture owner guard is still held (capture active,
  // not yet cleaned up). Used by on_event(SessionStopped) to distinguish
  // the auto-stop path (guard still held → cleanup needed) from the manual
  // stop path (guard already released by action_stop_capture → skip).
  bool has_capture_owner() const;

// Mutex + copy_in_progress accessors for the copy_to_doc_done flow in
// SigSession. SigSession is a friend of DocumentRegistry.
inline std::mutex &capture_state_mutex() { return _capture_state_mutex; }
inline std::atomic<bool> &copy_in_progress() { return _copy_in_progress; }
  // Direct index setter used by the copy_to_doc_done flow when the capture
  // owner must be cleared without going through CaptureOwnerGuard (e.g. when
  // there is no active document in headless mode). Caller MUST hold
  // capture_state_mutex().
  inline void set_capture_owner_index_locked(size_t index) {
    _capture_owner_index.store(index, std::memory_order_release);
  }

private:
  // Look up the owning index for a weak pointer held by this registry. Returns
  // SIZE_MAX if not found (including nullptr input or a released slot whose
  // stored pointer is nullptr).
  size_t find_index_for_document(data::SessionDocument *doc) const;

  EventBus *_event_bus;
  ISessionState *_state;
  ISessionCoordination *_coord;

  // Document list (owned). Released slots become nullptr but keep their index
  // (marked deletion) so all other indices remain stable.
  std::vector<std::unique_ptr<data::SessionDocument>> _owned_documents;
  size_t _active_document_index;
  std::atomic<size_t> _capture_owner_index{SIZE_MAX};

  // Capture owner / copy thread state
mutable std::mutex _capture_state_mutex;
std::atomic<bool> _copy_in_progress;
std::unique_ptr<CaptureOwnerGuard> _capture_owner_guard;

  // CaptureOwnerGuard is a nested class of DocumentRegistry, so under C++11+
  // rules it has implicit access to private members without an explicit
  // friend declaration. SigSession uses only public accessors.
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_DOCUMENTREGISTRY_H
