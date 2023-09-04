#ifndef _ASMPOWERPC_MMU_H_
#define _ASMPOWERPC_MMU_H_

#include <asm/pgtable.h>

bool vm_available(void);
bool mmu_enabled(void);
void mmu_enable(pgd_t *pgtable);
void mmu_disable(void);

#endif
