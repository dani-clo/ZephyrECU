#ifndef ZEPHYRECU_TASK_MANAGER_PRIVATE_H_
#define ZEPHYRECU_TASK_MANAGER_PRIVATE_H_
#include "ecu_task_manager.h"

/* Production scheduling transitions, separately testable with absolute ticks.
 * Only the worker may mutate scheduling state once it has started.
 */
bool ecu_task_prepare(ecu_task_id_t task, int64_t now, uint32_t *events);
void ecu_task_complete(ecu_task_id_t task, int64_t now);
#endif
