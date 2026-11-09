#include "ecu_nvm.h"
#include "nvm_private.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#define PART DT_ALIAS(ecu_nvm)
static const struct device *const flash = PARTITION_NODE_DEVICE(PART);
static struct nvs_fs raw;
static uint8_t *memory;
static size_t memory_size;

static void irq_calls(const void *unused)
{
	ARG_UNUSED(unused);
	uint32_t value;

	zassert_equal(ecu_nvm_init(), -EWOULDBLOCK);
	zassert_equal(ecu_nvm_read(0x1000, 1, &value, sizeof(value)), -EWOULDBLOCK);
	zassert_equal(ecu_nvm_write(0x1000, 1, &value, sizeof(value)), -EWOULDBLOCK);
	zassert_equal(ecu_nvm_delete(0x1000), -EWOULDBLOCK);
}
static int fail_write(const struct device *dev, off_t offset, uint8_t data)
{
	ARG_UNUSED(dev); ARG_UNUSED(offset); ARG_UNUSED(data);
	return -EIO;
}
static const struct flash_simulator_cb write_failure = {.write_byte = fail_write};

static void raw_mount(void)
{
	struct flash_pages_info page;

	zassert_ok(flash_get_page_info_by_offs(flash, DT_REG_ADDR(PART), &page));
	raw = (struct nvs_fs){.flash_device = flash, .offset = DT_REG_ADDR(PART),
		.sector_size = page.size, .sector_count = DT_REG_SIZE(PART) / page.size};
	zassert_ok(nvs_mount(&raw));
}
static void inject(uint16_t id, const uint8_t *bytes, size_t size)
{
	/* Valid NVS ATE and NVS data CRC, invalid manager envelope. */
	zassert_ok(ecu_nvm_backend_write(id, bytes, size));
}

static void concurrency_worker(void *id_arg, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1); ARG_UNUSED(unused2);
	uint16_t id = (uintptr_t)id_arg;

	for (uint32_t i = 0; i < 24; ++i) {
		uint32_t values[2] = {i, ~i}, output[2];

		zassert_ok(ecu_nvm_write(id, 1, values, sizeof(values)));
		zassert_ok(ecu_nvm_read(id, 1, output, sizeof(output)));
		zassert_mem_equal(values, output, sizeof(values));
	}
}
K_THREAD_STACK_DEFINE(thread1_stack, 2048);
K_THREAD_STACK_DEFINE(thread2_stack, 2048);
static struct k_thread thread1, thread2;

ZTEST(nvm, test_contract)
{
	uint32_t value = 0x12345678, output = 0xa5a5a5a5;

	zassert_ok(flash_erase(flash, DT_REG_ADDR(PART), DT_REG_SIZE(PART)));

	zassert_equal(ecu_nvm_read(0x1000, 1, &output, sizeof(output)), -EACCES);
	zassert_equal(ecu_nvm_write(0x1000, 1, &value, sizeof(value)), -EACCES);
	zassert_equal(ecu_nvm_delete(0x1000), -EACCES);
	irq_offload(irq_calls, NULL);
	/* Mount must reject invalid storage, preserve it, and permit retry after
	 * deliberate external repair. It must never autoformat a bad region.
	 */
	memory = flash_simulator_get_memory(flash, &memory_size);
	memset(memory + DT_REG_ADDR(PART), 0, DT_REG_SIZE(PART));
	zassert_equal(ecu_nvm_init(), -EBADMSG);
	zassert_equal(memory[DT_REG_ADDR(PART)], 0);
	zassert_ok(flash_erase(flash, DT_REG_ADDR(PART), DT_REG_SIZE(PART)));
	zassert_ok(ecu_nvm_init());
	zassert_ok(ecu_nvm_init());
	zassert_equal(ecu_nvm_read(0x1000, 1, &output, sizeof(output)), -ENOENT);
	zassert_equal(output, 0xa5a5a5a5);
	zassert_equal(ecu_nvm_write(0, 1, &value, sizeof(value)), -EINVAL);
	zassert_equal(ecu_nvm_write(0xffff, 1, &value, sizeof(value)), -EINVAL);
	zassert_equal(ecu_nvm_write(0x1000, 1, NULL, sizeof(value)), -EINVAL);
	zassert_equal(ecu_nvm_write(0x1000, 1, &value, 0), -EINVAL);
	zassert_equal(ecu_nvm_write(0x1000, 1, &value, CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE + 1), -EINVAL);
	zassert_equal(ecu_nvm_read(0, 1, &output, sizeof(output)), -EINVAL);
	zassert_equal(ecu_nvm_read(0xffff, 1, &output, sizeof(output)), -EINVAL);
	zassert_equal(ecu_nvm_read(0x1000, 1, NULL, sizeof(output)), -EINVAL);
	zassert_equal(ecu_nvm_read(0x1000, 1, &output, 0), -EINVAL);
	zassert_equal(ecu_nvm_read(0x1000, 1, &output, CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE + 1), -EINVAL);
	zassert_equal(ecu_nvm_delete(0), -EINVAL);
	zassert_equal(ecu_nvm_delete(0xffff), -EINVAL);
	zassert_ok(ecu_nvm_write(0x1000, 1, &value, sizeof(value)));
	zassert_ok(ecu_nvm_write(1, 0xffff, &value, sizeof(value))); /* framework ID */
	zassert_ok(ecu_nvm_write(0xfffe, 0, &value, sizeof(value))); /* legal boundary */
	zassert_ok(ecu_nvm_read(0xfffe, 0, &output, sizeof(output)));
	zassert_equal(output, value);
	zassert_ok(ecu_nvm_read(1, 0xffff, &output, sizeof(output)));
	zassert_ok(ecu_nvm_read(0x1000, 1, &output, sizeof(output)));
	zassert_equal(output, value);

	/* NVS suppresses an identical envelope+payload; no allocation advances. */
	raw_mount();
	uint32_t before = raw.data_wra;

	zassert_ok(ecu_nvm_write(0x1000, 1, &value, sizeof(value)));
	raw_mount();
	zassert_equal(raw.data_wra, before);
	output = 0xa5a5a5a5;
	zassert_equal(ecu_nvm_read(0x1000, 2, &output, sizeof(output)), -EPROTONOSUPPORT);
	zassert_equal(output, 0xa5a5a5a5);
	zassert_equal(ecu_nvm_read(0x1000, 1, &output, 2), -EMSGSIZE);
	zassert_equal(output, 0xa5a5a5a5);
	value++;
	zassert_ok(ecu_nvm_write(0x1000, 3, &value, sizeof(value)));
	zassert_equal(ecu_nvm_read(0x1000, 1, &output, sizeof(output)), -EPROTONOSUPPORT);
	zassert_ok(ecu_nvm_read(0x1000, 3, &output, sizeof(output)));
	zassert_equal(output, value);
	zassert_ok(ecu_nvm_delete(0x1000));
	zassert_ok(ecu_nvm_delete(0x1000));
	zassert_equal(ecu_nvm_read(0x1000, 3, &output, sizeof(output)), -ENOENT);

	uint8_t malformed[12] = {0};

	inject(0x1001, malformed, sizeof(malformed));
	zassert_equal(ecu_nvm_read(0x1001, 1, &output, sizeof(output)), -EBADMSG);
	sys_put_le32(ECU_NVM_MAGIC, malformed);
	sys_put_le16(1, malformed + 4);
	sys_put_le16(3, malformed + 6); /* actual payload is four bytes */
	inject(0x1002, malformed, sizeof(malformed));
	zassert_equal(ecu_nvm_read(0x1002, 1, &output, sizeof(output)), -EBADMSG);
	inject(0x1003, malformed, 3); /* truncated manager header, valid NVS CRC */
	zassert_equal(ecu_nvm_read(0x1003, 1, &output, sizeof(output)), -EBADMSG);
	zassert_equal(output, value); /* every failed read left caller data intact */
	sys_put_le16(0, malformed + 6);
	inject(0x1006, malformed, ECU_NVM_HEADER_SIZE); /* illegal empty payload */
	zassert_equal(ecu_nvm_read(0x1006, 1, &output, sizeof(output)), -EBADMSG);
	uint8_t oversized[ECU_NVM_HEADER_SIZE + CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE + 1] = {0};

	inject(0x1007, oversized, sizeof(oversized));
	zassert_equal(ecu_nvm_read(0x1007, 1, &output, sizeof(output)), -EBADMSG);

	/* Inject one damaged payload byte at the just-written record data offset.
	 * NVS ATE is untouched and CRC-8-valid; its CRC-32 now disagrees. Locate
	 * via NVS's actual data_wra (high16 sector, low16 data offset), not a
	 * guessed flash record layout. No writes/GC occur before the mutation.
	 */
	raw_mount();
	uint32_t address = raw.data_wra;
	off_t at = raw.offset + (address >> 16) * raw.sector_size + (address & 0xffff);

	zassert_ok(ecu_nvm_write(0x1004, 1, &value, sizeof(value)));
	memory = flash_simulator_get_memory(flash, &memory_size);
	zassert_true((size_t)at + 12 < memory_size);
	memory[at + ECU_NVM_HEADER_SIZE] ^= 1;
	zassert_equal(ecu_nvm_read(0x1004, 1, &output, sizeof(output)), -EBADMSG);
	zassert_equal(output, value);
	memory[at + ECU_NVM_HEADER_SIZE] ^= 1;
	zassert_ok(ecu_nvm_read(0x1004, 1, &output, sizeof(output)));

	/* Driver failures remain I/O errors, distinct from the same native NVS
	 * -EIO CRC outcome. A subsequent successful write/read remains possible.
	 */
	flash_simulator_set_callbacks(flash, &write_failure);
	zassert_equal(ecu_nvm_write(0x1005, 1, &value, sizeof(value)), -EIO);
	flash_simulator_set_callbacks(flash, NULL);
	zassert_ok(ecu_nvm_write(0x1005, 1, &value, sizeof(value)));

	/* Concurrent independent IDs, coherent pairs, real NVS writes and reads. */
	k_tid_t t1 = k_thread_create(&thread1, thread1_stack, K_THREAD_STACK_SIZEOF(thread1_stack),
		concurrency_worker, (void *)0x1010, NULL, NULL, 5, 0, K_NO_WAIT);
	k_tid_t t2 = k_thread_create(&thread2, thread2_stack, K_THREAD_STACK_SIZEOF(thread2_stack),
		concurrency_worker, (void *)0x1011, NULL, NULL, 5, 0, K_NO_WAIT);

	zassert_ok(k_thread_join(t1, K_SECONDS(5)));
	zassert_ok(k_thread_join(t2, K_SECONDS(5)));

	/* Force several sector rotations/GC with changing max-size payloads. */
	uint8_t payload[CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE], readback[sizeof(payload)];

	for (unsigned int i = 0; i < 160; ++i) {
		memset(payload, i, sizeof(payload));
		zassert_ok(ecu_nvm_write(0x1020, 1, payload, sizeof(payload)));
	}
	zassert_ok(ecu_nvm_read(0x1020, 1, readback, sizeof(readback)));
	zassert_mem_equal(payload, readback, sizeof(payload));
	/* Remount independently without erase: valid records and tombstones
	 * survive NVS mount; full manager startup over reboot has its own test.
	 */
	raw_mount();
	zassert_equal(nvs_read(&raw, 0x1000, payload, sizeof(payload)), -ENOENT);
	zassert_true(nvs_read(&raw, 0x1005, payload, sizeof(payload)) > 0);
	bool full = false;

	for (uint16_t id = 0x1100; id < 0x1180; ++id) {
		int ret = ecu_nvm_write(id, 1, payload, sizeof(payload));

		if (ret == -ENOSPC) { full = true; break; }
		zassert_ok(ret);
	}
	zassert_true(full); /* bounded full-storage error, not endless GC */
}
ZTEST_SUITE(nvm, NULL, NULL, NULL, NULL, NULL);
