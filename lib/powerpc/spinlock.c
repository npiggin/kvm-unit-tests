#include <asm/spinlock.h>
#include <asm/smp.h>

/*
 * Skip the atomic when single-threaded, which helps avoid larx/stcx. in
 * the harness when testing tricky larx/stcx. sequences (e.g., migration
 * vs reservation).
 */
void spin_lock(struct spinlock *lock)
{
	if (!multithreaded) {
		assert(lock->v == 0);
		lock->v = 1;
	} else {
		while (__sync_lock_test_and_set(&lock->v, 1))
			;
	}
}

void spin_unlock(struct spinlock *lock)
{
	assert(lock->v == 1);
	if (!multithreaded) {
		lock->v = 0;
	} else {
		__sync_lock_release(&lock->v);
	}
}
