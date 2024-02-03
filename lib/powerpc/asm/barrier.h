#ifndef _ASMPOWERPC_BARRIER_H_
#define _ASMPOWERPC_BARRIER_H_

#define cpu_relax() asm volatile("or 1,1,1 ; or 2,2,2" ::: "memory")
#define pause_short() asm volatile("pause_short" ::: "memory")

#define mb() asm volatile("sync":::"memory")
#define rmb() asm volatile("sync":::"memory")
#define wmb() asm volatile("sync":::"memory")

#include <asm-generic/barrier.h>
#endif
