#ifndef PXVIEW_PV_BASE_COPY_AUDIT_H
#define PXVIEW_PV_BASE_COPY_AUDIT_H

#include <cstdint>

// ----------------------------------------------------------------------------
// P1-e 拷贝审计 —— 统计"真实采集 / 导出路径上剩余的整载荷拷贝"。
//
// 为什么需要：零拷贝调查与 P2 槽位池化给出的都是**基准测试**数字。本计数器把
// 同一件事放到真实运行路径上度量，用于确认结论成立（或发现新出现的拷贝点）。
//
// 两个站点：
//   staging : LogicSnapshotDiskCacheWriter::enqueue() 的入队 memcpy
//             —— libsigrok 借用式 payload 契约导致，PXView 内部无法消除。
//             P2 池化后单核占用已降到 ~1.8%（1000 MB/s），此计数器用于复核。
//   export  : StoreSession / SessionService 导出时的交叉重打包与 zip 写入
//             —— 导出路径，拷贝本就必要。
//
// 门控：`PXVIEW_COPY_AUDIT`（由 CMake 的 ENABLE_DECODE_PERF 一并定义，
// 见 CMake/flags.cmake）。关闭时宏展开为 `(void)(bytes)`，零运行时开销。
// 汇总行 COPY_AUDIT 由 pv/base/perflog.h 的窗口 flush 一并写入
// %TEMP%/pxv_decode_perf.log。
//
// 本头**刻意保持轻量**（只含 <cstdint>）：它会进入 logicsnapshot_diskcache_writer.cpp
// 与 storesession.cpp 这两个已包含 windows/Qt/libsigrok 头的翻译单元，
// 不能像 perflog.h 那样拉入 windows.h / glib.h。
// ----------------------------------------------------------------------------

#ifdef PXVIEW_COPY_AUDIT

namespace pv {
namespace base {
namespace perf {

// 诊断用途；relaxed 竞争可接受（与 perflog.h 中的其它聚合一致）。
inline uint64_t g_copy_staging_calls = 0;
inline uint64_t g_copy_staging_bytes = 0;
inline uint64_t g_copy_export_calls  = 0;
inline uint64_t g_copy_export_bytes  = 0;

inline void record_copy_staging(uint64_t bytes) {
  g_copy_staging_calls++;
  g_copy_staging_bytes += bytes;
}
inline void record_copy_export(uint64_t bytes) {
  g_copy_export_calls++;
  g_copy_export_bytes += bytes;
}

}  // namespace perf
}  // namespace base
}  // namespace pv

#define PXV_PERF_COPY_STAGING(bytes) \
  pv::base::perf::record_copy_staging(static_cast<uint64_t>(bytes))
#define PXV_PERF_COPY_EXPORT(bytes) \
  pv::base::perf::record_copy_export(static_cast<uint64_t>(bytes))

#else  // !PXVIEW_COPY_AUDIT

#define PXV_PERF_COPY_STAGING(bytes) \
  do {                               \
    (void)(bytes);                   \
  } while (0)
#define PXV_PERF_COPY_EXPORT(bytes) \
  do {                              \
    (void)(bytes);                  \
  } while (0)

#endif  // PXVIEW_COPY_AUDIT

#endif  // PXVIEW_PV_BASE_COPY_AUDIT_H
