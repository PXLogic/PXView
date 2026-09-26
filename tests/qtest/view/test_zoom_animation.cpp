/*
 * test_zoom_animation.cpp — 滚轮缩放动画状态机的契约
 *
 * 背景
 * ----
 * 滚轮缩放动画把"滚轮离散 tick"变成"100ms 线性插值的连续滑动"。这个状态机里
 * 有几条**只在运行时才会被违反**、且破坏了就表现为"缩放交付量不足 / 手感迟钝"
 * 的不变量：
 *
 *   1. 端点：t=0 必须落在 initial，t>=duration 必须落在 target 且置为 inactive
 *      （收尾帧硬写终值，避免插值残差）。
 *   2. 锚点不动：整段动画中，鼠标所在的那个像素处的时间必须恒定
 *      ——否则缩放时光标下的波形会漂移。
 *   3. 复合累计（**本测试的核心**）：连续快速滚动 N 格，交付的缩放量必须恰为
 *      zoom_factor^N，与滚动速度无关。
 *      实测缺陷（29 个手势 / 104 格滚轮）：以"当前显示值"为基准 retarget 会把
 *      上一格尚未走完的行程整段丢弃，交付率只有 26%（应 205.3× / 实得 52.8×）。
 *      日志实证：手势 #1 第 1 格目标 354.55ms，第 2 格到来时显示值才走到
 *      475.10ms，新目标被算成 475.10/1.5 = 316.73ms，余量 475→354 被扔掉。
 *      修复 = retarget_compound() 以**旧目标**为基准复合。
 *   4. 丢帧恢复：用绝对墙钟求进度，事件循环被挂起（模态框/最小化）后恢复必须
 *      一帧补到终点，不得永久卡在中途。
 *
 * 纯逻辑无 Qt 依赖（zoom_animation.h 只依赖 <cstdint>），因此本测试直接
 * 编入头文件即可，不需要任何 View / Widget / 数据源。
 */

#include <QtTest>

#include <cmath>
#include <cstdint>

#include "pv/view/zoom_animation.h"

using pv::view::ZoomAnimation;

// sqrt(2)：物理滚轮每格的缩放比（zoom_factor = sqrt(2)^steps，
// 对应可见跨度按 2^(k/2) 成阶梯变化）。
static const double kPerNotch = 1.4142135623730951;

class TestZoomAnimation : public QObject {
  Q_OBJECT
private slots:
  // ---- 1. 端点与夹取 ----
  void SampleHitsInitialAtT0();
  void SampleHitsTargetAndGoesInactive();
  void ProgressIsClampedAndMonotone();

  // ---- 2. 锚点不动 ----
  void AnchorStaysFixedWithinOnePixel();
  void ReanchorKeepsAnchorStableAfterClamp();

  // ---- 3. 复合累计（回归测试：修复前交付率 26%）----
  void CompoundDeliversFullZoomOverRapidNotches();
  void CompoundTargetCompoundsNotCurrentValue();
  void CompoundClampsToBounds();
  void FirstNotchStartsFromCurrentValue();

  // ---- 4. 丢帧恢复 ----
  void ClockSkipSettlesInOneFrame();
};

// ---------------------------------------------------------------------------
// 1. 端点与夹取
// ---------------------------------------------------------------------------

void TestZoomAnimation::SampleHitsInitialAtT0() {
  ZoomAnimation a;
  a.retarget(1e-3, 1e-3 / kPerNotch, /*fixpoint=*/400.0, /*anchor_offset=*/0,
             /*now_ms=*/1000, ZoomAnimation::DurationMs);

  double s = 0.0;
  QVERIFY(a.sample(1000, s));  // t=0，动画仍在跑
  QCOMPARE(s, 1e-3);
  QVERIFY(a.active());
}

void TestZoomAnimation::SampleHitsTargetAndGoesInactive() {
  ZoomAnimation a;
  const double target = 1e-3 / kPerNotch;
  a.retarget(1e-3, target, 400.0, 0, 0, ZoomAnimation::DurationMs);

  double s = 0.0;
  // 恰好走完 duration：必须落在 target 且置 inactive（收尾帧硬写终值）。
  QVERIFY(!a.sample(ZoomAnimation::DurationMs, s));
  QCOMPARE(s, target);
  QVERIFY(!a.active());

  // 超过 duration 仍是 target。
  QVERIFY(!a.sample(ZoomAnimation::DurationMs * 3, s));
  QCOMPARE(s, target);
}

void TestZoomAnimation::ProgressIsClampedAndMonotone() {
  ZoomAnimation a;
  a.retarget(1.0, 2.0, 0.0, 0, 5000, 100);

  QCOMPARE(a.progress(5000), 0.0);   // t=0
  QCOMPARE(a.progress(5050), 0.5);   // 中点
  QCOMPARE(a.progress(5100), 1.0);   // 终点
  QCOMPARE(a.progress(9999), 1.0);   // 上夹取
  QCOMPARE(a.progress(4000), 0.0);   // 下夹取（时钟回退保护）

  // sample() 结束后 progress() 仍可用（返回 1.0），供诊断日志收尾。
  double s = 0.0;
  a.sample(5100, s);
  QCOMPARE(a.progress(5100), 1.0);
}

// ---------------------------------------------------------------------------
// 2. 锚点不动
// ---------------------------------------------------------------------------

void TestZoomAnimation::AnchorStaysFixedWithinOnePixel() {
  // 视口 800px、初始 scale 1e-3 秒/像素 → 可见 800ms，与实测量级一致。
  const double s0 = 1e-3;
  const double anchor = 400.0;  // 鼠标位于视口中央
  const int64_t o0 = 0;

  ZoomAnimation a;
  a.retarget(s0, s0 / kPerNotch, anchor, o0, 0, ZoomAnimation::DurationMs);

  // 锚点处的时间（秒）—— 整段动画中必须恒定。
  const double invariant = (static_cast<double>(o0) + anchor) * s0;

  double scale = s0;
  int frames = 0;
  int64_t now = 0;
  while (frames < 6) {
    now += 16;
    double next = scale;
    a.sample(now, next);
    if (next == scale)
      continue;  // t=0 那一帧没有变化
    scale = next;

    const int64_t offset = a.offset_for(scale);
    const double drift = std::fabs((offset + anchor) * scale - invariant);

    // 规格：锚点漂移必须 < 1 像素。scale 就是"一像素值多少秒"，故以它为单位。
    // 这里锁死的是"offset 必须以固定参照系算、不能逐帧递推"——逐帧递推时
    // floor 的同向误差会累积，6 帧后漂移可达数像素（鼠标下的波形会滑动）。
    QVERIFY2(drift <= scale * (1.0 + 1e-9), "锚点漂移超过 1 像素");
    frames++;
  }
  QCOMPARE(frames, 6);
}

// 触边再锚定：offset 被有效范围夹住后，参照系必须被重设，且重设后仍满足
// "锚点抖动 < 1 像素"（不能与夹取结果较劲产生回弹）。
void TestZoomAnimation::ReanchorKeepsAnchorStableAfterClamp() {
  const double s0 = 1e-3;
  const double anchor = 400.0;
  ZoomAnimation a;
  a.retarget(s0, s0 / kPerNotch, anchor, 0, 0, ZoomAnimation::DurationMs);

  // 模拟 ViewLayout 的夹取：把 offset 人为夹到一个边界值并 reanchor。
  const int64_t boundary = -5000;
  a.reanchor(s0, boundary);
  QCOMPARE(a.anchor_offset(), boundary);
  QCOMPARE(a.initial_scale(), s0);

  double next = s0;
  a.sample(100, next);
  const int64_t offset = a.offset_for(next);
  const double invariant = (static_cast<double>(boundary) + anchor) * s0;
  const double drift = std::fabs((offset + anchor) * next - invariant);
  QVERIFY2(drift <= next * (1.0 + 1e-9), "reanchor 后锚点漂移超过 1 像素");
}

// ---------------------------------------------------------------------------
// 3. 复合累计
// ---------------------------------------------------------------------------

void TestZoomAnimation::CompoundDeliversFullZoomOverRapidNotches() {
  // 复现实测缺陷：5 格快速连滚（每 16ms 一格，即实测的连滚节奏）。
  // 交付的缩放倍率必须恰为 kPerNotch^5，与滚动速度无关。
  // 修复前（基准取"当前显示值"）实测交付率仅 26%。
  const double s0 = 1e-3;
  const double min_s = 1e-9;
  const double max_s = 1e9;

  ZoomAnimation a;
  double scale = s0;

  for (int i = 0; i < 5; i++) {
    const int64_t now = static_cast<int64_t>(i) * 16;
    a.retarget_compound(scale, 400.0, 0, 1.0 / kPerNotch, min_s, max_s, now,
                        ZoomAnimation::DurationMs);
    // 下一格到来前推进一帧
    double next = scale;
    a.sample(now + 16, next);
    scale = next;
  }

  // 跑完剩余动画（末格之后 100ms 内收敛）。
  int64_t now = 5 * 16;
  double next = scale;
  while (a.sample(now, next))
    now += 16;
  scale = next;

  const double delivered = s0 / scale;              // 实得倍率
  const double expected = std::pow(kPerNotch, 5.0); // 应得倍率 = 2^(5/2) = 5.657
  QVERIFY2(std::fabs(delivered - expected) < expected * 1e-12,
           "复合累计交付量必须 = zoom_factor^N，与滚动速度无关");
}

void TestZoomAnimation::CompoundTargetCompoundsNotCurrentValue() {
  // 直接锁死"基准是旧目标、不是当前显示值"这条语义：
  // 连滚两格但**中间一帧都不推进**（显示值仍是 s0），
  // 目标仍必须是 s0/kPerNotch^2，而不是 s0/kPerNotch。
  const double s0 = 1e-3;
  ZoomAnimation a;
  a.retarget_compound(s0, 0.0, 0, 1.0 / kPerNotch, 1e-9, 1e9, 0, 100);
  a.retarget_compound(s0, 0.0, 0, 1.0 / kPerNotch, 1e-9, 1e9, 16, 100);

  const double expected = s0 / (kPerNotch * kPerNotch);
  QVERIFY(std::fabs(a.target_scale() - expected) < expected * 1e-12);
}

void TestZoomAnimation::CompoundClampsToBounds() {
  // 目标越过 _minscale/_maxscale 时必须被夹住（否则会缩放到非法尺度过远）。
  const double min_s = 1e-4;
  const double max_s = 1e-2;
  ZoomAnimation a;

  // 连续放大 64 格：s0/√2^64 远小于 min_s → 必须夹到 min_s。
  double v = max_s;
  for (int i = 0; i < 64; i++)
    a.retarget_compound(v, 0.0, 0, 1.0 / kPerNotch, min_s, max_s,
                        static_cast<int64_t>(i) * 16, 100);
  QCOMPARE(a.target_scale(), min_s);

  // 反向：连续缩小 → 夹到 max_s。
  a.cancel();
  for (int i = 0; i < 64; i++)
    a.retarget_compound(v, 0.0, 0, kPerNotch, min_s, max_s,
                        static_cast<int64_t>(i) * 16, 100);
  QCOMPARE(a.target_scale(), max_s);
}

void TestZoomAnimation::FirstNotchStartsFromCurrentValue() {
  // 手势的第一格（动画未在跑）必须从**当前值**起算，而不是从旧目标
  // ——否则第一格会以上一次手势遗留的目标为基准，产生跳变。
  const double s0 = 1e-3;
  ZoomAnimation a;
  a.retarget_compound(s0, 0.0, 0, 1.0 / kPerNotch, 1e-9, 1e9, 0, 100);
  QCOMPARE(a.target_scale(), s0 / kPerNotch);

  // 动画结束后再来一格 = 新手势，仍从当前值起算。
  double s = 0.0;
  a.sample(1000, s);  // 远超 duration → inactive
  QVERIFY(!a.active());
  a.retarget_compound(1e-2, 0.0, 0, 1.0 / kPerNotch, 1e-9, 1e9, 2000, 100);
  QCOMPARE(a.target_scale(), 1e-2 / kPerNotch);
}

// ---------------------------------------------------------------------------
// 4. 丢帧恢复
// ---------------------------------------------------------------------------

void TestZoomAnimation::ClockSkipSettlesInOneFrame() {
  // 事件循环被挂起（模态框/窗口最小化）后恢复：绝对墙钟求进度必须让它
  // 一帧到达终点，而不是继续跑满 100ms 或永久停在中途。
  ZoomAnimation a;
  const double target = 1e-3 / kPerNotch;
  a.retarget(1e-3, target, 0.0, 0, 0, ZoomAnimation::DurationMs);

  double s = 0.0;
  const int64_t far_future = ZoomAnimation::DurationMs * 100;  // 挂起 10 秒
  QVERIFY(!a.sample(far_future, s));
  QCOMPARE(s, target);
  QVERIFY(!a.active());
}

QTEST_MAIN(TestZoomAnimation)
#include "test_zoom_animation.moc"
