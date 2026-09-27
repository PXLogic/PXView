/*
 * test_pan_decay.cpp — 拖拽释放惯性滚动积分器的契约
 *
 * 物理模型：v(t) = v0·e^(-t/τ)，位移是速度的精确积分 x(t) = v0·τ·(1-e^(-t/τ))，
 * 每帧 sample() 返回 [上一帧, 本帧] 区间的精确位移。这里锁死几条只在运行时
 * 才会被违反、破坏了就表现为"惯性滑动手感错误"的不变量：
 *
 *   1. 总位移守恒：无论按什么帧间隔采样，所有帧位移之和 + 结算尾差 必须
 *      精确等于解析值 v0·τ（积分是精确的，不是每帧 v·dt 的近似累加）。
 *   2. 减速单调：逐帧位移必须单调下降（指数衰减没有回弹）。
 *   3. 半程对称：[0, τ·ln2] 与 [τ·ln2, ∞) 的位移各占总位移的一半。
 *   4. 停止阈值：速度衰减到 StopMinVelocityPxPerMs 以下必须结束并一次性结算
 *      剩余位移，不得无限滑行，也不得丢掉剩余行程。
 *   5. 时钟跳跃：事件循环挂起后恢复（now 直接跳到很远的未来），必须一次
 *      结算全部剩余位移并结束，不得跳变丢失或永久卡住。
 *   6. 方向与零速：v0=0 不启动；负方向位移为负。
 */

#include <QtTest>

#include <cmath>
#include <cstdint>

#include "pv/view/pan_decay.h"

using pv::view::PanDecay;

class TestPanDecay : public QObject {
  Q_OBJECT
private slots:
  void ZeroVelocityDoesNotStart();
  void TotalDistanceEqualsV0Tau();
  void PerFrameDistanceMonotonicallyDecreases();
  void HalfLifeSymmetry();
  void StopsAtMinVelocityAndSettlesTail();
  void ClockSkipSettlesInOneFrame();
  void NegativeDirection();
  void CancelStopsImmediately();
};

void TestPanDecay::ZeroVelocityDoesNotStart() {
  PanDecay p;
  p.start(0.0, 1000);
  QVERIFY(!p.active());
  double dx = -1.0;
  QVERIFY(!p.sample(1016, dx));
  QCOMPARE(dx, 0.0);
}

void TestPanDecay::TotalDistanceEqualsV0Tau() {
  // 解析总位移 = v0·τ。以 16ms 帧步进采样到结束，累计位移必须与之精确一致
  // （误差只来自浮点，不允许有"每帧近似"造成的系统性偏差）。
  const double v0 = 2.0;  // px/ms
  const double tau = static_cast<double>(PanDecay::TimeConstantMs);
  const double total_expected = v0 * tau;

  PanDecay p;
  p.start(v0, 0);

  double sum = 0.0;
  int64_t now = 0;
  int frames = 0;
  double dx = 0.0;
  while (p.sample(now, dx)) {
    sum += dx;
    now += 16;
    frames++;
    QVERIFY(frames < 10000);  // 防止无限滑行的看门狗
  }
  sum += dx;  // 结束帧的位移（含结算尾差）
  QVERIFY2(std::fabs(sum - total_expected) < 1e-6,
           "帧位移之和必须精确等于 v0·τ");
  QVERIFY(!p.active());
}

void TestPanDecay::PerFrameDistanceMonotonicallyDecreases() {
  const double v0 = 3.0;
  PanDecay p;
  p.start(v0, 0);

  double prev = 1e18;
  int64_t now = 16;  // 从第一个 16ms 帧开始（t=0 帧宽度为 0，位移必为 0）
  double dx = 0.0;
  int frames = 0;
  while (p.sample(now, dx)) {
    QVERIFY2(dx > 0.0, "正方向每帧位移必须为正");
    QVERIFY2(dx < prev, "逐帧位移必须单调下降（指数衰减无回弹）");
    prev = dx;
    now += 16;
    frames++;
    QVERIFY(frames < 10000);
  }
}

void TestPanDecay::HalfLifeSymmetry() {
  // 指数衰减的半衰期 t_half = τ·ln2。接口的时钟分辨率是整毫秒，因此取
  // round(τ·ln2) 并按该整数时刻解析计算期望值，而不是按连续半程断言。
  const double v0 = 2.0;
  const double tau = static_cast<double>(PanDecay::TimeConstantMs);
  const int64_t t_half = static_cast<int64_t>(tau * std::log(2.0) + 0.5);
  const double total = v0 * tau;

  PanDecay p;
  p.start(v0, 0);
  double dx = 0.0;
  QVERIFY(p.sample(t_half, dx));  // v(t_half)≈v0/2 > 阈值，动画仍在跑
  const double first_half_expected =
      v0 * tau * (1.0 - std::exp(-static_cast<double>(t_half) / tau));
  QVERIFY(std::fabs(dx - first_half_expected) < 1e-9);

  // 剩余部分（含结算尾差）补齐到总位移 v0·τ。
  double rest = 0.0;
  p.sample(t_half + 100000, rest);  // 远超结束点
  QVERIFY(std::fabs((dx + rest) - total) < 1e-6);
}

void TestPanDecay::StopsAtMinVelocityAndSettlesTail() {
  const double v0 = 0.05;  // px/ms：很快衰减到 0.01 阈值
  const double tau = static_cast<double>(PanDecay::TimeConstantMs);
  PanDecay p;
  p.start(v0, 0);

  // v(t) ≤ vmin 的时刻：t ≥ τ·ln(v0/vmin) = 100·ln(5) ≈ 160.9ms
  const int64_t t_stop = static_cast<int64_t>(tau * std::log(v0 / PanDecay::StopMinVelocityPxPerMs));
  double dx = 0.0;
  int64_t now = 0;
  while (p.sample(now, dx)) {
    now += 16;
    QVERIFY(now <= t_stop + 32);  // 不得滑过阈值时刻太久（无界滑行）
  }
  QVERIFY(!p.active());
}

void TestPanDecay::ClockSkipSettlesInOneFrame() {
  // 事件循环挂起 10 秒后恢复：一帧结算全部剩余位移（= 全部 v0·τ）并结束。
  const double v0 = 2.0;
  PanDecay p;
  p.start(v0, 0);

  double dx = 0.0;
  QVERIFY(!p.sample(10000, dx));
  QVERIFY(std::fabs(dx - v0 * PanDecay::TimeConstantMs) < 1e-6);
  QVERIFY(!p.active());
}

void TestPanDecay::NegativeDirection() {
  const double v0 = -1.5;
  PanDecay p;
  p.start(v0, 0);

  double sum = 0.0;
  int64_t now = 0;
  double dx = 0.0;
  while (p.sample(now, dx)) {
    sum += dx;
    now += 16;
  }
  sum += dx;
  QVERIFY(sum < 0.0);
  QVERIFY(std::fabs(sum - v0 * PanDecay::TimeConstantMs) < 1e-6);
}

void TestPanDecay::CancelStopsImmediately() {
  PanDecay p;
  p.start(5.0, 0);
  QVERIFY(p.active());
  p.cancel();
  QVERIFY(!p.active());
  double dx = -1.0;
  QVERIFY(!p.sample(16, dx));
  QCOMPARE(dx, 0.0);
}

QTEST_MAIN(TestPanDecay)
#include "test_pan_decay.moc"
