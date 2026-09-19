#ifndef TIMER_PURE_H
#define TIMER_PURE_H

#include <stdint.h>
#include <stdbool.h>
#include "timer_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 计时器的纯算术 —— 无锁、无 I/O、不碰 FreeRTOS，因此能在主机上测。
 * timer_engine.c 负责把这些函数接到互斥量和 NVS 上。
 *
 * 抽出来的动因：改动最频繁、出过最多 bug 的正是这几段算术，而它们原本和
 * xSemaphoreTake / nvs_get_blob / PortRtc_GetTime 挤在同一个函数体里，
 * 主机上一次也跑不了。 */

/* 分钟数合法区间（含端点） */
#define TIMER_MIN_MINUTES  1
#define TIMER_MAX_MINUTES  180

bool     TimerPure_ValidMinutes(uint32_t minutes);
uint32_t TimerPure_TotalSeconds(uint32_t minutes);

/* 递减所有活跃槽位 1 秒；本次刚到 0 的槽位置 active=false 后拷进 fired[]。
 * 返回 fired 数量。注意：拷进 fired[] 的是「已失效」的快照。
 * fired 须能容纳 TIMER_MAX_SLOTS 个元素。 */
int TimerPure_Tick(timer_instance_t *slots, int n_slots,
                   timer_instance_t *fired);

/* 睡眠/断电经过的秒数；负值（RTC 被调回）钳到 0。 */
int64_t TimerPure_ElapsedSec(int64_t now_sec, int64_t saved_sec);

/* 扣掉 elapsed 后的剩余秒数。返回 false 表示睡眠期间已到期，应丢弃。
 * remaining 必须 > 0（调用方只在 active 槽位上调用）。 */
bool TimerPure_RemainingAfter(uint32_t remaining, int64_t elapsed,
                              uint32_t *out_remaining);

/* Restore 时从已恢复的槽位反推下一个 id：最大 id + 1；uint8_t 回绕到 0 时
 * 钳到 1（TimerEngine_Cancel 把 0 当作 INVALID_ARG）。
 * 注意：正常启动路径用单调递增计数器，不用这个——否则取消掉高 id 计时器后
 * 会把同一个 id 发第二次。 */
uint8_t TimerPure_RecoverNextId(const timer_instance_t *slots, int n_slots);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_PURE_H */
