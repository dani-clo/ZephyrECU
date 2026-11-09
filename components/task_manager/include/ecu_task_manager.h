#ifndef ZEPHYRECU_TASK_MANAGER_H_
#define ZEPHYRECU_TASK_MANAGER_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>

struct ecu_task_descriptor;
typedef const struct ecu_task_descriptor *ecu_task_id_t;
typedef void (*ecu_task_callback_t)(uint32_t events);

typedef struct {
	uint64_t invocations;
	uint64_t missed_periods;
	uint64_t late_invocations;
} ecu_task_stats_t;

/* Macro implementation storage; applications must not access these fields. */
struct ecu_task_state {
	atomic_t pending;
	int64_t next;
	ecu_task_stats_t stats;
};
struct ecu_task_descriptor {
	ecu_task_callback_t callback;
	uint32_t period_ms;
	struct ecu_task_state *state;
};

/* Define once at file scope. period 0 = event-only; first release is start +
 * period. Symbols are globally unique; dispatch order is lexical symbol order.
 * name is the public handle; ECU_TASK_DECLARE(name) exposes it in other files.
 */
#define ECU_TASK_DEFINE(name, cb, ms) \
	BUILD_ASSERT((uint64_t)(ms) < (UINT64_C(1) << 32), "Invalid task period"); \
	static struct ecu_task_state name##_state; \
	const STRUCT_SECTION_ITERABLE(ecu_task_descriptor, name##_descriptor) = { \
		.callback = (cb), .period_ms = (ms), .state = &name##_state, \
	}; \
	const ecu_task_id_t name = &name##_descriptor
#define ECU_TASK_DECLARE(name) extern const ecu_task_id_t name

/* Thread-only, explicit one-time start; duplicate returns -EALREADY.
 * No callback runs before start. ISR returns -EWOULDBLOCK.
 */
int ecu_task_manager_start(void);
/* ISR/thread-safe, nonblocking, never invokes callbacks. Nonzero bits coalesce
 * and remain pending before start. Unknown/NULL handle or zero mask: -EINVAL.
 */
int ecu_task_notify(ecu_task_id_t task, uint32_t event_mask);
/* Coherent read-only snapshot, also ISR-safe. Invalid ID/NULL output: -EINVAL.
 * Counters saturate at UINT64_MAX. See docs/task-manager.md for definitions.
 */
int ecu_task_get_stats(ecu_task_id_t task, ecu_task_stats_t *stats);

#endif
