#ifndef ZEPHYRECU_APP_TASKS_H_
#define ZEPHYRECU_APP_TASKS_H_

/* After LED/can_comm init, before can_comm/task manager start. */
int app_tasks_init(void);
/* Post a configuration change to the LED application task. */
int app_tasks_config_changed(void);
#endif
