/**
  ******************************************************************************
  * @file    blackboard.h
  * @brief   Blackboard —— 单原子整数保护共享数据区域（两步法，申请不等待）
  *
  *          state 编码：
  *            state == 0   : 空闲
  *            state == -1  : 正在被写（独占）
  *            state == -2  : 释放状态（资源回收，独占；仅空闲时可进入）
  *            state >  0   : 正在被读，数值 = 读者数量
  *
  *          合法迁移：空闲<->写、空闲<->读、空闲->释放（单向终态）；
  *          读/写/释放两两不可直接切换（由 CAS 目标值天然保证，无竞态窗口）。
  *
  *          使用范式（两步法）：
  *            if (BlackboardReadRequest(&bb)) {    // 阶段1：申请（遇写者/释放不等待）
  *                ... 通过 bb.cacheAddr 读数据 ... // 阶段2：仅申请成功方可操作
  *                BlackboardReadRelease(&bb);      // 配对释放（计数归零时触发回调）
  *            } else {
  *                ... 失败处理：放弃/重试/退避，策略由调用方决定 ...
  *            }
  *
  *          依赖 C11 <stdatomic.h>；在 Cortex-M 上 CAS 由编译器自动
  *          降为 LDREXW/STREXW 排他指令（ARMv7-M 及以上），无需手写汇编。
  ******************************************************************************
  */

#ifndef BLACKBOARD_H
#define BLACKBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/**
  * @brief  黑板对象：原子状态 + 被保护数据区域信息
  * @note   onReadersEmpty 为读者计数归零回调：最后一个读者释放（计数 -> 0）时
  *         被调用。典型用途：预期读者数目固定的场景（initReaderCount > 0），
  *         全部固定读者归还计数后统一回收/处理数据区域。可为 NULL。
  */
typedef struct blackboard_t {
    atomic_int state;                              /* 保护状态：0=空闲 / -1=写 / -2=释放 / >0=读者数 */
    void      *cacheAddr;                          /* 被保护数据区域（缓存）起始地址 */
    size_t     cacheSize;                          /* 被保护数据区域大小（字节） */
    void     (*onReadersEmpty)(struct blackboard_t *); /* 读者计数归零回调，可为 NULL */
} Blackboard_T;

/**
  * @brief  初始化黑板：登记被保护数据区域，并设定读者初始值与归零回调。
  *         黑板对象使用前必须经本函数初始化（atomic_init 的要求）。
  * @param  bb             黑板对象
  * @param  cacheAddr      被保护数据区域起始地址
  * @param  cacheSize      被保护数据区域大小（字节）
  * @param  initReaderCount 读者初始值，必须 >= 0：
  *            = 0 -> 初始为空闲态，常规用法；
  *            > 0 -> 初始即带"预登记读者"，适用于预期读者数目固定的场景：
  *                   各固定读者完成后直接 BlackboardReadRelease 归还计数，
  *                   归零时触发 onReadersEmpty。
  * @param  onReadersEmpty 读者计数归零回调，可为 NULL
  */
void BlackboardInit(Blackboard_T *bb, void *cacheAddr, size_t cacheSize,
                    int initReaderCount, void (*onReadersEmpty)(Blackboard_T *));

/*
 * 阶段1：申请状态更改；成功返回 true，失败返回 false。
 *
 * BlackboardReadRequest：仅当被其他读者并发改计数导致 CAS 失败时继续尝试；
 *                        遇到写者（-1）或释放状态（-2）立即失败，不循环等待。
 * BlackboardWriteRequest：单次 CAS(0->-1)，任何非空闲状态立即失败。
 * BlackboardFreeRequest：单次 CAS(0->-2)，仅空闲可进入释放状态。
 */
bool BlackboardReadRequest(Blackboard_T *bb);
bool BlackboardWriteRequest(Blackboard_T *bb);
bool BlackboardFreeRequest(Blackboard_T *bb);

/* 阶段2之后：释放（仅申请成功者——或固定读者场景归还预登记计数——方可调用，
 * 必须严格配对；
 * 释放状态 -2 为终态，进入后不再回到空闲，无需也不存在 Finish 操作） */
void BlackboardReadRelease(Blackboard_T *bb);
void BlackboardWriteRelease(Blackboard_T *bb);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOARD_H */
