/**
  ******************************************************************************
  * @file    blackboard.c
  * @brief   Blackboard 实现 —— 全部状态迁移均为原子操作，无锁、无禁用中断
  *
  *          内存序约定：
  *            申请成功  acquire —— 临界区内对数据区域的访问不被重排到申请之前
  *            释放      release —— 临界区内的修改先于释放对其它观察者可见
  *            读释放    acq_rel —— 归零回调能看到此前所有读者的访问结果
  *            探测/失败 relaxed —— 不构成同步，避免多余屏障（DMB）开销
  ******************************************************************************
  */

#include "blackboard.h"

#include <assert.h>

void BlackboardInit(Blackboard_T *bb, void *cacheAddr, size_t cacheSize,
                    int initReaderCount, void (*onReadersEmpty)(Blackboard_T *))
{
    assert(initReaderCount >= 0);       /* 读者初始值不得为负 */
    atomic_init(&bb->state, initReaderCount);
    bb->cacheAddr = cacheAddr;
    bb->cacheSize = cacheSize;
    bb->onReadersEmpty = onReadersEmpty;
}

bool BlackboardReadRequest(Blackboard_T *bb)
{
    int s = atomic_load_explicit(&bb->state, memory_order_relaxed);
    for (;;) {
        if (s < 0)              /* 写(-1)或释放(-2)进行中，立即失败不等待 */
            return false;
        /* CAS：读者数 +1。
         * 失败分两种：
         *  - 被其他读者并发改了计数（s 仍为 >=0）：读读不互斥，更新 s 继续尝试；
         *  - 被写者/释放者改成负值：下一轮循环在上方拦截，返回 false。
         */
        if (atomic_compare_exchange_weak_explicit(
                &bb->state, &s, s + 1,
                memory_order_acquire,   /* 成功：获取语义，保护后续对数据区域的读 */
                memory_order_relaxed))  /* 失败：s 已被更新为最新值 */
            return true;
    }
}

void BlackboardReadRelease(Blackboard_T *bb)
{
    int s = atomic_load_explicit(&bb->state, memory_order_relaxed);
    for (;;) {
        if (s <= 0) {           /* 无读者（空闲/写/释放）：拒绝减 1，计数不会变负 */
            assert(s > 0);      /* Debug 下暴露误配对；Release 下静默忽略 */
            return;
        }
        /* CAS：仅当计数仍 > 0 时才减 1。
         * acq_rel：release 保证本读者的读先于减计数对他人可见；
         * acquire 保证最后一个读者能看到此前所有读者的访问结果。
         * 失败时 s 已被更新（可能被其他读者改小/改大），下一轮重新检查。
         */
        if (atomic_compare_exchange_weak_explicit(
                &bb->state, &s, s - 1,
                memory_order_acq_rel,
                memory_order_relaxed)) {
            if (s == 1 && bb->onReadersEmpty != NULL)
                bb->onReadersEmpty(bb);     /* 读者计数归零：触发回调 */
            return;
        }
    }
}

bool BlackboardWriteRequest(Blackboard_T *bb)
{
    int expected = 0;
    /* 单次 CAS：仅空闲(0)才能切换到写(-1)；其他状态立即失败 */
    return atomic_compare_exchange_strong_explicit(
        &bb->state, &expected, -1,
        memory_order_acquire,
        memory_order_relaxed);
}

void BlackboardWriteRelease(Blackboard_T *bb)
{
    /* 用 CAS 完成 -1 -> 0：只有当前确为写状态才释放，
     * 防止误配对（未持有写锁却调用释放）静默破坏状态。
     */
    int expected = -1;
    bool ok = atomic_compare_exchange_strong_explicit(
        &bb->state, &expected, 0,
        memory_order_release,   /* 临界区内的修改先于释放对其它线程可见 */
        memory_order_relaxed);
    assert(ok);                 /* 失败说明调用方未配对使用 */
    (void)ok;
}

bool BlackboardFreeRequest(Blackboard_T *bb)
{
    int expected = 0;
    /* 单次 CAS：仅空闲(0)才能进入释放状态(-2)；其他状态立即失败。
     * 释放状态为终态：进入后不再回到空闲，黑板对后续访问永久关闭。
     */
    return atomic_compare_exchange_strong_explicit(
        &bb->state, &expected, -2,
        memory_order_acquire,
        memory_order_relaxed);
}
