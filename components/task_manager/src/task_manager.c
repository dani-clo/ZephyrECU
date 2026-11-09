#include "task_manager_private.h"

#include <errno.h>

K_SEM_DEFINE(task_wake, 0, 1);
static atomic_t started;
static struct k_spinlock stats_lock;

static bool known(ecu_task_id_t task)
{
	STRUCT_SECTION_FOREACH(ecu_task_descriptor, entry) {
		if (task == entry) { return true; }
	}
	return false;
}

static void add(uint64_t *counter, uint64_t value)
{
	*counter = UINT64_MAX - *counter < value ? UINT64_MAX : *counter + value;
}

static int64_t period_ticks(ecu_task_id_t task)
{
	return k_ms_to_ticks_ceil64(task->period_ms);
}

bool ecu_task_prepare(ecu_task_id_t task, int64_t now, uint32_t *events)
{
	struct ecu_task_state *s = task->state;
	int64_t period = period_ticks(task);
	bool due = period != 0 && now >= s->next;

	/* Atomic exchange BEFORE user code: notifications during dispatch survive. */
	*events = (uint32_t)atomic_set(&s->pending, 0);
	if (!due && *events == 0) { return false; }
	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	if (due) {
		int64_t skipped = (now - s->next) / period;

		add(&s->stats.missed_periods, skipped);
		/* Lateness refers to the latest represented release, not skipped ones. */
		if ((now - s->next) % period != 0) {
			add(&s->stats.late_invocations, 1);
		}
		s->next += (skipped + 1) * period;
	}
	add(&s->stats.invocations, 1);
	k_spin_unlock(&stats_lock, key);
	return true;
}

void ecu_task_complete(ecu_task_id_t task, int64_t now)
{
	struct ecu_task_state *s = task->state;
	int64_t period = period_ticks(task);

	/* Releases through completion (inclusive) are skipped, also for an
	 * event-only invocation of a periodic task. Never replay slow callbacks.
	 */
	if (period != 0 && now >= s->next) {
		int64_t skipped = (now - s->next) / period + 1;
		k_spinlock_key_t key = k_spin_lock(&stats_lock);

		add(&s->stats.missed_periods, skipped);
		s->next += skipped * period;
		k_spin_unlock(&stats_lock, key);
	}
}

static void worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		STRUCT_SECTION_FOREACH(ecu_task_descriptor, task) {
			uint32_t events;

			if (ecu_task_prepare(task, k_uptime_ticks(), &events)) {
				task->callback(events);
				ecu_task_complete(task, k_uptime_ticks());
			}
		}
		int64_t next = INT64_MAX;

		STRUCT_SECTION_FOREACH(ecu_task_descriptor, task) {
			if (task->period_ms != 0 && task->state->next < next) {
				next = task->state->next;
			}
		}
		/* A racing post leaves a semaphore token, so cannot be lost between
		 * the scan and wait. Stale tokens only cause one harmless extra scan.
		 */
		(void)k_sem_take(&task_wake, next == INT64_MAX ? K_FOREVER :
				 K_TIMEOUT_ABS_TICKS(next));
	}
}

K_THREAD_DEFINE(ecu_task_thread, CONFIG_ECU_TASK_MANAGER_STACK_SIZE, worker,
		NULL, NULL, NULL, CONFIG_ECU_TASK_MANAGER_PRIORITY, 0, SYS_FOREVER_MS);

int ecu_task_manager_start(void)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!atomic_cas(&started, 0, 1)) { return -EALREADY; }
	int64_t now = k_uptime_ticks();

	STRUCT_SECTION_FOREACH(ecu_task_descriptor, task) {
		task->state->next = now + period_ticks(task);
	}
	k_thread_start(ecu_task_thread);
	return 0;
}

int ecu_task_notify(ecu_task_id_t task, uint32_t event_mask)
{
	if (!known(task) || event_mask == 0) { return -EINVAL; }
	atomic_or(&task->state->pending, (atomic_val_t)event_mask);
	k_sem_give(&task_wake);
	return 0;
}

int ecu_task_get_stats(ecu_task_id_t task, ecu_task_stats_t *stats)
{
	if (!known(task) || stats == NULL) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	*stats = task->state->stats;
	k_spin_unlock(&stats_lock, key);
	return 0;
}
