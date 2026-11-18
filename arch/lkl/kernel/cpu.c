#include <linux/kernel.h>
#include <linux/sched/stat.h>
#include <asm/host_ops.h>
#include <asm/cpu.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/sched.h>
#include <asm/syscalls.h>
#include <linux/cpu.h>

/*
 * This structure is used to get access to the "LKL CPU" that allows us to run
 * Linux code. Because we have to deal with various synchronization requirements
 * between idle thread, system calls, interrupts, "reentrancy", CPU shutdown,
 * imbalance wake up (i.e. acquire the CPU from one thread and release it from
 * another), we can't use a simple synchronization mechanism such as (recursive)
 * mutex or semaphore. Instead, we use a mutex and a bunch of status data plus a
 * semaphore.
 */
struct lkl_cpu {
	/* lock that protects the CPU status data */
	struct lkl_mutex *lock;
	/*
	 * Since we must free the cpu lock during shutdown we need a
	 * synchronization algorithm between lkl_cpu_shutdown() and the CPU
	 * access functions since lkl_cpu_get() gets called from thread
	 * destructor callback functions which may be scheduled after
	 * lkl_cpu_shutdown() has freed the cpu lock.
	 *
	 * An atomic counter is used to keep track of the number of running
	 * CPU access functions and allow the shutdown function to wait for
	 * them.
	 *
	 * The shutdown functions adds MAX_THREADS to this counter which allows
	 * the CPU access functions to check if the shutdown process has
	 * started.
	 *
	 * This algorithm assumes that we never have more the MAX_THREADS
	 * requesting CPU access.
	 */
	#define MAX_THREADS 1000000
	unsigned int shutdown_gate;
	bool irqs_pending;
	/* no of threads waiting the CPU */
	unsigned int sleepers;
	/* no of times the current thread got the CPU */
	unsigned int count;
	/* current thread that owns the CPU */
	lkl_thread_t owner;
	/* semaphore for threads waiting the CPU */
	struct lkl_sem *sem;
	/* semaphore used for shutdown */
	struct lkl_sem *shutdown_sem;
	/* idle waits here for an interrupt until the idle host task exists */
	struct lkl_sem *idle_sem;
	bool idle_waiting;
	/*
	 * halt waits here for the threads still in the gate (lkl_cpu_wait_gate);
	 * gate_waiting says it does, and the thread that clears it posts
	 */
	struct lkl_sem *gate_sem;
	bool gate_waiting;
} cpu;

static int __cpu_try_get_lock(int n)
{
	lkl_thread_t self;

	if (__sync_fetch_and_add(&cpu.shutdown_gate, n) >= MAX_THREADS)
		return -2;

	lkl_ops->mutex_lock(cpu.lock);

	if (cpu.shutdown_gate >= MAX_THREADS)
		return -1;

	self = lkl_ops->thread_self();

	if (cpu.owner && !lkl_ops->thread_equal(cpu.owner, self))
		return 0;

	cpu.owner = self;
	cpu.count++;

	return 1;
}

static void __cpu_try_get_unlock(int lock_ret, int n)
{
	if (lock_ret >= -1)
		lkl_ops->mutex_unlock(cpu.lock);
	/* the last thread out of a shut down gate wakes the shutdown's wait */
	if (__sync_sub_and_fetch(&cpu.shutdown_gate, n) == MAX_THREADS && n &&
	    __atomic_exchange_n(&cpu.gate_waiting, false, __ATOMIC_SEQ_CST))
		lkl_ops->sem_up(cpu.gate_sem);
}

void lkl_cpu_change_owner(lkl_thread_t owner)
{
	lkl_ops->mutex_lock(cpu.lock);
	if (cpu.count > 1)
		lkl_bug("bad count while changing owner\n");
	cpu.owner = owner;
	lkl_ops->mutex_unlock(cpu.lock);
}

int lkl_cpu_get(void)
{
	int ret;

	ret = __cpu_try_get_lock(1);

	while (ret == 0) {
		cpu.sleepers++;
		__cpu_try_get_unlock(ret, 0);
		lkl_ops->sem_down(cpu.sem);
		ret = __cpu_try_get_lock(0);
	}

	__cpu_try_get_unlock(ret, 1);

	if (ret > 0)
		idle_host_task_busy();

	return ret;
}

void lkl_cpu_put(void)
{
	/* the owner's last put, the idle host task current: the kernel idles */
	if (cpu.count == 1)
		idle_host_task_idle();

	lkl_ops->mutex_lock(cpu.lock);

	if (!cpu.count || !cpu.owner ||
	    !lkl_ops->thread_equal(cpu.owner, lkl_ops->thread_self()))
		lkl_bug("%s: unbalanced put\n", __func__);

	while (cpu.irqs_pending && !irqs_disabled()) {
		cpu.irqs_pending = false;
		lkl_ops->mutex_unlock(cpu.lock);
		run_irqs();
		lkl_ops->mutex_lock(cpu.lock);
	}

	if (test_ti_thread_flag(current_thread_info(), TIF_HOST_THREAD) &&
	    !single_task_running() && cpu.count == 1) {
		if (in_interrupt())
			lkl_bug("%s: in interrupt\n", __func__);
		lkl_ops->mutex_unlock(cpu.lock);
		idle_host_task_busy();
		thread_sched_jb();
		return;
	}

	if (--cpu.count > 0) {
		lkl_ops->mutex_unlock(cpu.lock);
		return;
	}

	if (cpu.sleepers) {
		cpu.sleepers--;
		lkl_ops->sem_up(cpu.sem);
	}

	cpu.owner = 0;

	lkl_ops->mutex_unlock(cpu.lock);
}

int lkl_cpu_try_run_irq(int irq)
{
	int ret;

	ret = __cpu_try_get_lock(1);
	if (!ret) {
		set_irq_pending(irq);
		cpu.irqs_pending = true;
	}
	if (ret >= -1 && cpu.idle_waiting) {
		cpu.idle_waiting = false;
		lkl_ops->sem_up(cpu.idle_sem);
	}
	__cpu_try_get_unlock(ret, 1);

	return ret;
}

void lkl_cpu_shutdown(void)
{
	__sync_fetch_and_add(&cpu.shutdown_gate, MAX_THREADS);
}

void lkl_cpu_wait_shutdown(void)
{
	lkl_ops->sem_down(cpu.shutdown_sem);
	lkl_ops->sem_free(cpu.shutdown_sem);
}

/*
 * Wait for the threads still in the gate to leave it. A thread woken from
 * lkl_cpu_get()'s sem_down(cpu.sem) at shutdown still counts until it runs
 * again; spinning here kept it from running on a host that runs its
 * threads cooperatively (one at a time), and the halt hung whenever another
 * thread was waiting for the CPU (a program thread entering a syscall).
 * So this sleeps on gate_sem, posted by the thread whose exit from the gate
 * empties it (__cpu_try_get_unlock) if it finds gate_waiting set. Clearing
 * gate_waiting before the semaphore is freed means a late thread (a TLS
 * destructor after the halt) never posts it.
 */
static void lkl_cpu_wait_gate(void)
{
	for (;;) {
		__atomic_store_n(&cpu.gate_waiting, true, __ATOMIC_SEQ_CST);
		if (__sync_fetch_and_add(&cpu.shutdown_gate, 0) <= MAX_THREADS)
			break;
		lkl_ops->sem_down(cpu.gate_sem);
	}
	/* a thread that cleared it is posting: take that before the free */
	if (!__atomic_exchange_n(&cpu.gate_waiting, false, __ATOMIC_SEQ_CST))
		lkl_ops->sem_down(cpu.gate_sem);
}

static void lkl_cpu_cleanup(bool shutdown)
{
	lkl_cpu_wait_gate();

	if (shutdown)
		lkl_ops->sem_up(cpu.shutdown_sem);
	else if (cpu.shutdown_sem)
		lkl_ops->sem_free(cpu.shutdown_sem);
	if (cpu.sem)
		lkl_ops->sem_free(cpu.sem);
	if (cpu.idle_sem)
		lkl_ops->sem_free(cpu.idle_sem);
	if (cpu.gate_sem)
		lkl_ops->sem_free(cpu.gate_sem);
	if (cpu.lock)
		lkl_ops->mutex_free(cpu.lock);
}

/*
 * Idle before the idle host task exists: release the CPU and sleep until an
 * interrupt is triggered. Returning at once would have the idle loop spin
 * holding the CPU, and the host threads that deliver interrupts (timers,
 * devices) could only queue them; a host that runs its threads
 * cooperatively, one at a time, never runs them at all and hangs at boot.
 * Only an interrupt can make work before boot is done: no host thread
 * makes syscalls until lkl_start_kernel() returns.
 */
static void lkl_cpu_wait_irq(void)
{
	lkl_ops->mutex_lock(cpu.lock);
	if (cpu.irqs_pending) {
		/* run them instead, as lkl_cpu_put() would, then idle again */
		cpu.irqs_pending = false;
		lkl_ops->mutex_unlock(cpu.lock);
		run_irqs();
		return;
	}
	/* an interrupt from here on posts idle_sem (lkl_cpu_try_run_irq) */
	cpu.idle_waiting = true;
	lkl_ops->mutex_unlock(cpu.lock);

	lkl_cpu_put();
	lkl_ops->sem_down(cpu.idle_sem);
	lkl_cpu_get();
}

void arch_cpu_idle(void)
{
	if (cpu.shutdown_gate >= MAX_THREADS) {

		lkl_ops->mutex_lock(cpu.lock);
		while (cpu.sleepers--)
			lkl_ops->sem_up(cpu.sem);
		lkl_ops->mutex_unlock(cpu.lock);

		lkl_cpu_cleanup(true);

		lkl_ops->thread_exit();
	}
	/* enable irqs now to allow direct irqs to run */
	local_irq_enable();

	/* switch to idle_host_task, or wait for an interrupt until it exists */
	if (!wakeup_idle_host_task())
		lkl_cpu_wait_irq();
}

int lkl_cpu_init(void)
{
	cpu.lock = lkl_ops->mutex_alloc(0);
	cpu.sem = lkl_ops->sem_alloc(0);
	cpu.shutdown_sem = lkl_ops->sem_alloc(0);
	cpu.idle_sem = lkl_ops->sem_alloc(0);
	cpu.gate_sem = lkl_ops->sem_alloc(0);

	if (!cpu.lock || !cpu.sem || !cpu.shutdown_sem || !cpu.idle_sem ||
	    !cpu.gate_sem) {
		lkl_cpu_cleanup(false);
		return -ENOMEM;
	}

	return 0;
}
