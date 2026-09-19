/* timer_pure 主机自检 —— 一次性检查，不进固件。
 * 编译： gcc -I E:/aiot/tools/pc_sim/include -I E:/aiot/components/timer_engine \
 *            E:/aiot/components/timer_engine/timer_pure.c <这个文件> -o timerpurecheck.exe
 * 跑：   ./timerpurecheck.exe
 *
 * 覆盖的是出过 bug 的三段算术：倒计时归零、睡眠后 elapsed 折算、id 回绕。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "timer_pure.h"

static timer_instance_t mk(uint8_t id, uint32_t total, uint32_t remaining, bool active)
{
    timer_instance_t t = {0};
    t.id = id;
    t.kind = TIMER_KIND_REMINDER;
    t.total_seconds = total;
    t.remaining_sec = remaining;
    t.active = active;
    return t;
}

static void test_valid_minutes(void)
{
    assert(!TimerPure_ValidMinutes(0));
    assert( TimerPure_ValidMinutes(1));
    assert( TimerPure_ValidMinutes(180));
    assert(!TimerPure_ValidMinutes(181));
    assert(TimerPure_TotalSeconds(25) == 1500);
    assert(TimerPure_TotalSeconds(180) == 10800);
    printf("  valid_minutes / total_seconds OK\n");
}

static void test_tick(void)
{
    timer_instance_t slots[TIMER_MAX_SLOTS];
    timer_instance_t fired[TIMER_MAX_SLOTS];

    /* slot0 还剩 2 秒、slot1 还剩 1 秒、slot2/3 未启用 */
    slots[0] = mk(1, 120, 2, true);
    slots[1] = mk(2, 60,  1, true);
    slots[2] = mk(0, 0,   0, false);
    slots[3] = mk(0, 0,   0, false);

    /* 第 1 次 tick：先递减再判 0 —— 剩 1 秒的那个这一拍就到点；
     * 剩 2 秒的减到 1，仍在跑。 */
    int n = TimerPure_Tick(slots, TIMER_MAX_SLOTS, fired);
    assert(n == 1);
    assert(slots[0].remaining_sec == 1 && slots[0].active);
    assert(!slots[1].active && slots[1].remaining_sec == 0);
    assert(fired[0].id == 2 && !fired[0].active && fired[0].total_seconds == 60);

    /* 第 2 次 tick：slot0 减到 0，进 fired */
    n = TimerPure_Tick(slots, TIMER_MAX_SLOTS, fired);
    assert(n == 1);
    assert(!slots[0].active && slots[0].remaining_sec == 0);

    /* fired[] 拿到的是「已失效」的快照 —— 回调读 active 依赖这一点 */
    assert(fired[0].id == 1 && !fired[0].active && fired[0].total_seconds == 120);

    /* 已失效的槽位再 tick 不应重复触发 */
    assert(TimerPure_Tick(slots, TIMER_MAX_SLOTS, fired) == 0);
    printf("  tick: 递减 / 归零 / 死槽不重触发 OK\n");
}

static void test_tick_zero_remaining_is_not_double_decremented(void)
{
    /* 边界：remaining 已经是 0 但 active 仍为 true（异常态）不应下溢 */
    timer_instance_t slots[TIMER_MAX_SLOTS];
    timer_instance_t fired[TIMER_MAX_SLOTS];
    slots[0] = mk(7, 60, 0, true);
    for (int i = 1; i < TIMER_MAX_SLOTS; i++) slots[i] = mk(0, 0, 0, false);

    int n = TimerPure_Tick(slots, TIMER_MAX_SLOTS, fired);
    assert(n == 1);
    assert(fired[0].id == 7);
    assert(slots[0].remaining_sec == 0);   /* 没有回绕成 0xFFFFFFFF */
    printf("  tick: remaining==0 不回绕 OK\n");
}

static void test_elapsed(void)
{
    assert(TimerPure_ElapsedSec(1000, 400) == 600);
    assert(TimerPure_ElapsedSec(400, 1000) == 0);   /* RTC 调回 → 钳 0 */
    assert(TimerPure_ElapsedSec(500, 500) == 0);
    printf("  elapsed: 正常 / RTC 调回钳 0 OK\n");
}

static void test_remaining_after(void)
{
    uint32_t out = 0;

    /* 还剩 100 秒，睡了 60 秒 → 剩 40 */
    assert(TimerPure_RemainingAfter(100, 60, &out));
    assert(out == 40);

    /* 还剩 60 秒，正好睡了 60 秒 → 到期丢弃（<= 是刻意的） */
    assert(!TimerPure_RemainingAfter(60, 60, &out));

    /* 睡过头 → 丢弃 */
    assert(!TimerPure_RemainingAfter(60, 9999, &out));

    /* elapsed = 0 → 原样返回 */
    assert(TimerPure_RemainingAfter(1, 0, &out));
    assert(out == 1);

    /* 1 秒的计时器睡 0 秒仍存活，睡 1 秒就死 —— 边界 */
    assert(TimerPure_RemainingAfter(1, 1, &out) == false);
    printf("  remaining_after: 折算 / 到期 / 边界 OK\n");
}

static void test_recover_next_id(void)
{
    timer_instance_t slots[TIMER_MAX_SLOTS];

    /* 全空 → 从 1 开始 */
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) slots[i] = mk(0, 0, 0, false);
    assert(TimerPure_RecoverNextId(slots, TIMER_MAX_SLOTS) == 1);

    /* 活跃 id = 5 → 下一个 6 */
    slots[0] = mk(5, 60, 60, true);
    assert(TimerPure_RecoverNextId(slots, TIMER_MAX_SLOTS) == 6);

    /* 失效槽位不参与取最大值 */
    slots[1] = mk(200, 60, 60, false);
    assert(TimerPure_RecoverNextId(slots, TIMER_MAX_SLOTS) == 6);

    /* 回绕：id=255 且活跃 → 0 被钳回 1 */
    slots[0] = mk(255, 60, 60, true);
    assert(TimerPure_RecoverNextId(slots, TIMER_MAX_SLOTS) == 1);
    printf("  recover_next_id: 空 / 取最大 / 失效不计 / 回绕钳 1 OK\n");
}

int main(void)
{
    printf("timer_pure self-test:\n");
    test_valid_minutes();
    test_tick();
    test_tick_zero_remaining_is_not_double_decremented();
    test_elapsed();
    test_remaining_after();
    test_recover_next_id();
    printf("OK: all timer_pure checks pass\n");
    return 0;
}
