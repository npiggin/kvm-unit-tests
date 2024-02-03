#ifndef _ASMPOWERPC_SPINLOCK_H_
#define _ASMPOWERPC_SPINLOCK_H_

struct spinlock {
	unsigned int v;
};

void spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock);

#endif /* _ASMPOWERPC_SPINLOCK_H_ */
