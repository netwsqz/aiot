#include "timer_pure.h"

bool TimerPure_ValidMinutes(uint32_t minutes)
{
    return minutes >= TIMER_MIN_MINUTES && minutes <= TIMER_MAX_MINUTES;
}

uint32_t TimerPure_TotalSeconds(uint32_t minutes)
{
    return minutes * 60u;
}

int TimerPure_Tick(timer_instance_t *slots, int n_slots,
                   timer_instance_t *fired)
{
    int fired_count = 0;
    for (int i = 0; i < n_slots; i++) {
        if (!slots[i].active) continue;
        if (slots[i].remaining_sec > 0) slots[i].remaining_sec--;
        if (slots[i].remaining_sec == 0) {
            slots[i].active = false;        /* 先失效，再拷贝快照 */
            if (fired) fired[fired_count] = slots[i];
            fired_count++;
        }
    }
    return fired_count;
}

int64_t TimerPure_ElapsedSec(int64_t now_sec, int64_t saved_sec)
{
    int64_t elapsed = now_sec - saved_sec;
    return (elapsed < 0) ? 0 : elapsed;
}

bool TimerPure_RemainingAfter(uint32_t remaining, int64_t elapsed,
                              uint32_t *out_remaining)
{
    if ((int64_t)remaining <= elapsed) return false;   /* 睡眠期间已到期 */
    if (out_remaining) *out_remaining = remaining - (uint32_t)elapsed;
    return true;
}

uint8_t TimerPure_RecoverNextId(const timer_instance_t *slots, int n_slots)
{
    uint8_t max_id = 0;
    for (int i = 0; i < n_slots; i++) {
        if (slots[i].active && slots[i].id > max_id) max_id = slots[i].id;
    }
    uint8_t next = (uint8_t)(max_id + 1);
    if (next == 0) next = 1;      /* 255+1 回绕 */
    return next;
}
