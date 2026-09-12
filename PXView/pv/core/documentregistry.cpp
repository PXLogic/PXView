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

std::shared_ptr<data::SessionDocument>
DocumentRegistry::get_shared_by_index(size_t index) const {
  if (index == SIZE_MAX || index >= _owned_documents.size())
    return nullptr;
  return _owned_documents[index];
}

std::shared_ptr<data::SessionDocument>
DocumentRegistry::find_file_device_document_shared(
    ds_device_handle handle) const {
  if (handle == NULL_HANDLE)
    return nullptr;
  for (const auto &ptr : _owned_documents) {
    if (ptr && ptr->is_file_device_slot() && ptr->device_handle() == handle)
      return ptr;
  }
  return nullptr;
}

data::SessionDocument *
DocumentRegistry::find_file_device_document(ds_device_handle handle) const {
  if (handle == NULL_HANDLE)
    return nullptr;
  for (const auto &ptr : _owned_documents) {
    if (ptr && ptr->is_file_device_slot() && ptr->device_handle() == handle)
      return ptr.get();
  }
  return nullptr;
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
    if (ptr)
      ptr->clear_decoder_stacks();  // 加锁、幂等、并发安全
  }
}

void DocumentRegistry::clear_active_document_decoders() {
  // 问题2修复：设备切换（set_device）时只清活动文档的解码器栈，避免非活动
  // 文档（如 pxl 标签页的文档）的解码器被误清。所有运行中的解码任务仍会
  // 停止（解码线程可能持有指向被释放数据的指针），但只有活动文档的栈被清空。
  //
  // 并发安全修复：此函数可能被 MCP worker 线程（load_capture→set_device）
  // 与 GUI 主线程（CurrentDeviceChangePrev→del_all_protocol→clear_all_decoder）
  // 同时调用并各自清同一份文档栈 → 双线程 .clear() 数据竞争/双释放。统一走
  // SessionDocument::clear_decoder_stacks()（内部加锁 + 幂等）：先清空的销毁
  // 全部栈，后到的看到空向量即无操作。
  auto *doc = get_active_document();
  if (doc)
    doc->clear_decoder_stacks();
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
  // 数据模型重构步骤7：数据代 → Live。owner = 本次采集归属文档。
  // 这是 Live 转移的唯一入口。
  if (doc) {
    if (auto shared = get_shared_by_index(idx))
      mark_generation_live(std::move(shared));
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
  size_t owner_idx = SIZE_MAX;
  {
    std::lock_guard<std::mutex> lock(_capture_state_mutex);
    if (_capture_owner_guard) {
      guard_to_reset = std::move(_capture_owner_guard);
      owner_idx = _capture_owner_index.load(std::memory_order_acquire);
    }
  }
  // 阶段3a：采集结束（正常/中止）——owner ctx 进入 Stopped（持有完整快照，
  // 可显示/解码）。这是所有停止路径（SessionStopped / action_stop_capture /
  // exit_capture）的收敛点，per-tab 状态在此统一落定。
  if (owner_idx != SIZE_MAX) {
    if (auto *doc = get_document_by_index(owner_idx)) {
      if (doc->is_collecting())
        doc->set_state(data::SessionDocument::SessionState::Stopped);
    }
  }
  // 数据模型重构步骤7（澄清后）：采集结束（正常/中止）的 Live 出口。
  // 零拷贝下 doc 在 Live 期从不引用不完整执行缓冲（copy_data_to_document
  // 只发生在 RevEndPacket 后的 share），Live 期 doc 只可能：
  //   - 有数据（repeat≥2 帧，持有上一帧完整共享快照）→ Frozen，数据完整
  //     归属 owner doc，保持可显示/可认领；
  //   - 无数据（首帧中止）→ Empty，空白 tab 恒定空白。
  if (data_generation().phase == DataGeneration::Phase::Live) {
    data::SessionDocument *gen_owner = data_generation().owner_doc.lock().get();
    if (gen_owner && gen_owner->has_data())
      mark_generation_frozen();
    else
      reset_generation();
  }
  guard_to_reset.reset();
}

void DocumentRegistry::on_capture_frame_started(data::SessionDocument *owner_doc) {
  if (!owner_doc)
    return;
  // Frozen→Live 回边：仅当数据代归属文档就是本次采集 owner（repeat 连续帧
  // 的同一 doc）时回边；别的文档发起的采集不能把自己的 Live 标到他人头上。
  std::lock_guard<std::mutex> lock(_capture_state_mutex);
  if (_generation.phase != DataGeneration::Phase::Frozen)
    return;
  if (_generation.owner_doc.lock().get() != owner_doc)
    return;
  if (get_document_by_index(_capture_owner_index.load(std::memory_order_acquire)) !=
      owner_doc)
    return;
  _generation.phase = DataGeneration::Phase::Live;
}

bool DocumentRegistry::has_capture_owner() const {
  std::lock_guard<std::mutex> lock(_capture_state_mutex);
  return _capture_owner_guard != nullptr;
}

} // namespace core
} // namespace pv
