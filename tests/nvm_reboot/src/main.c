#include "ecu_nvm.h"
#include <errno.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

ZTEST(nvm_reboot, test_records_survive_reboot)
{
	uint32_t value = 0x10203040, output = 0;

	uint32_t cause;

	zassert_ok(hwinfo_get_reset_cause(&cause));
	if (!(cause & RESET_SOFTWARE)) {
		const struct device *flash = PARTITION_NODE_DEVICE(DT_ALIAS(ecu_nvm));

		zassert_ok(flash_erase(flash, DT_REG_ADDR(DT_ALIAS(ecu_nvm)),
				       DT_REG_SIZE(DT_ALIAS(ecu_nvm))));
		zassert_ok(ecu_nvm_init());
		zassert_ok(ecu_nvm_write(0x1010, 1, &value, sizeof(value)));
		zassert_ok(ecu_nvm_write(0x1011, 1, &value, sizeof(value)));
		zassert_ok(ecu_nvm_write(0x1012, 2, &value, sizeof(value)));
		zassert_ok(ecu_nvm_delete(0x1011));
		sys_reboot(SYS_REBOOT_WARM);
		zassert_unreachable();
	}
	zassert_true(cause & RESET_SOFTWARE);
	zassert_ok(ecu_nvm_init()); /* static manager state reset; remount from flash */
	zassert_ok(ecu_nvm_read(0x1010, 1, &output, sizeof(output)));
	zassert_equal(output, value);
	zassert_equal(ecu_nvm_read(0x1011, 1, &output, sizeof(output)), -ENOENT);
	zassert_equal(ecu_nvm_read(0x1012, 1, &output, sizeof(output)), -EPROTONOSUPPORT);
	zassert_equal(ecu_nvm_read(0x1012, 2, &output, 1), -EMSGSIZE);
	zassert_ok(ecu_nvm_read(0x1012, 2, &output, sizeof(output)));
	zassert_equal(output, value);
}
ZTEST_SUITE(nvm_reboot, NULL, NULL, NULL, NULL, NULL);
