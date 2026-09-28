/*
 * test_pan_animation.cpp — 水平平移动画状态机的契约
 *
 * 语义：把 offset 从 from 在 DurationMs 内缓动到 to，方向由 from/to 的相对大小
 * 决定。这里锁死几条"破坏了就表现为跳转生硬 / 回弹 / 卡死"的不变量：
 *
 *   1. 终点精确：到达/超过时长的那一帧必须硬写目标值（无插值残差）。
 *   2. 单调收敛：全程 offset 单向逼近目标，不越过、不回弹。
 *   3. 缓出：前段快、末段减速滑入 —— 半程时刻的进度明显大于线性 0.5。
 *   4. 时钟跳跃：事件循环挂起后恢复（now 直接跳到很远的未来），一帧补到终点。
 *   5. 双向对称：from>to（向左平移）与 from<to（向右平移）行为对称。
 *   6. 重定目标：动画中再次 start 以"当前实际值"为起点平滑接管（不回跳）。
 *   7. 取消：cancel 后立即结束，不再产出中间值。
 */

#include <QtTest>

#include <cstdint>

#include "pv/view/pan_animation.h"

using pv::view::PanAnimation;

class TestPanAnimation : public QObject {
  Q_OBJECT
private slots:
  void InactiveByDefault();
  void ReachesTargetExactlyAtEnd();
  void MonotonicTowardsTarget();
  void EaseOutIsFrontLoaded();
  void ClockJumpLandsInOneFrame();
  void NegativeDirection();
  void SameFromToStaysPut();
  void RetargetFromCurrentPosition();
  void CancelStopsImmediately();
};

void TestPanAnimation::InactiveByDefault() {
  PanAnimation a;
  QVERIFY(!a.active());
  int64_t out = 123;
  QVERIFY(!a.sample(16, out));
}

void TestPanAnimation::ReachesTargetExactlyAtEnd() {
  PanAnimation a;
  a.start(0, 1000, 500);
  QVERIFY(a.active());

  int64_t out = 0;
  QVERIFY(a.sample(650, out));  // t = 150/300 = 0.5
  QVERIFY(out > 0);
  QVERIFY(out < 1000);

  QVERIFY(!a.sample(800, out));  // t = 300/300 = 1.0 → 收尾
  QCOMPARE(out, static_cast<int64_t>(1000));
  QVERIFY(!a.active());
}

void TestPanAnimation::MonotonicTowardsTarget() {
  PanAnimation a;
  a.start(0, 1000, 0);

  int64_t out = 0;
  int64_t prev = -1;
  int64_t now = 16;
  bool more = true;
  while ((more = a.sample(now, out))) {
    QVERIFY2(out >= prev, "offset 必须单调不减地向目标收敛");
    QVERIFY2(out <= 1000, "不得越过目标（末尾帧取整即可先到达目标值）");
    prev = out;
    now += 16;
  }
  QCOMPARE(out, static_cast<int64_t>(1000));
}

void TestPanAnimation::EaseOutIsFrontLoaded() {
  PanAnimation a;
  a.start(0, 1000, 0);
  // ease-out cubic：半程时刻 t=0.5 的进度应为 1-0.5^3 = 0.875，明显大于线性 0.5。
  QVERIFY(a.progress(150) > 0.8);
  QVERIFY(a.progress(150) <= 1.0);
  QVERIFY(a.progress(-5) == 0.0);     // 时钟回退保护
  QVERIFY(a.progress(100000) == 1.0); // 结束后仍可调用
}

void TestPanAnimation::ClockJumpLandsInOneFrame() {
  PanAnimation a;
  a.start(0, -750, 0);

  int64_t out = 0;
  QVERIFY(!a.sample(100000, out));  // 远超结束点：一帧补到终点
  QCOMPARE(out, static_cast<int64_t>(-750));
  QVERIFY(!a.active());
}

void TestPanAnimation::NegativeDirection() {
  PanAnimation a;
  a.start(1000, -500, 0);

  int64_t out = 0;
  int64_t prev = 2000;  // 大于起点
  int64_t now = 16;
  bool more = true;
  while ((more = a.sample(now, out))) {
    QVERIFY2(out <= prev, "向左平移时 offset 必须单调不增");
    QVERIFY2(out >= -500, "不得越过目标（末尾帧取整即可先到达目标值）");
    prev = out;
    now += 16;
  }
  QCOMPARE(out, static_cast<int64_t>(-500));
}

void TestPanAnimation::SameFromToStaysPut() {
  PanAnimation a;
  a.start(42, 42, 0);
  int64_t out = 0;
  QVERIFY(a.sample(100, out));
  QCOMPARE(out, static_cast<int64_t>(42));
  QVERIFY(!a.sample(400, out));
  QCOMPARE(out, static_cast<int64_t>(42));
}

void TestPanAnimation::RetargetFromCurrentPosition() {
  PanAnimation a;
  a.start(0, 1000, 0);

  int64_t out = 0;
  QVERIFY(a.sample(150, out));  // 走到中途
  const int64_t mid = out;
  QVERIFY(mid > 0 && mid < 1000);

  // 动画中重定目标：以当前实际值为起点，不回跳。
  a.start(mid, 500, 150);
  QCOMPARE(a.from_offset(), mid);
  QCOMPARE(a.target_offset(), static_cast<int64_t>(500));
  QVERIFY(a.active());

  QVERIFY(!a.sample(150 + 300, out));
  QCOMPARE(out, static_cast<int64_t>(500));
}

void TestPanAnimation::CancelStopsImmediately() {
  PanAnimation a;
  a.start(0, 1000, 0);
  QVERIFY(a.active());
  a.cancel();
  QVERIFY(!a.active());

  int64_t out = -1;
  QVERIFY(!a.sample(16, out));
}

QTEST_MAIN(TestPanAnimation)
#include "test_pan_animation.moc"
