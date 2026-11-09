#include "nvm_private.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

static const struct flash_parameters parameter_sets[] = {
	{.write_block_size = 4, .erase_value = 0xff},
	{.write_block_size = 0, .erase_value = 0xff},
	{.write_block_size = 64, .erase_value = 0xff},
	{.write_block_size = 3, .erase_value = 0xff},
};
static const struct flash_parameters *params;
static struct flash_area area;
static struct nvs_fs *mounted;
static int open_result, page_calls, bad_page_call, page_error, mount_result;
static int io_result, nvs_result;
static size_t page_size, bad_page_size;
static off_t page_shift;
static bool driver_error;
static unsigned int reads, writes, erases;
static struct device_state unready_state;
static const struct device unready = {.state = &unready_state};

static int read_flash(const struct device *dev, off_t offset, void *data, size_t size)
{
	ARG_UNUSED(dev);
	zassert_equal(offset, 0x1000);
	zassert_equal(size, 4);
	memset(data, 0x5a, size);
	reads++;
	return io_result;
}
static int write_flash(const struct device *dev, off_t offset, const void *data, size_t size)
{
	ARG_UNUSED(dev);
	zassert_equal(offset, 0x1000);
	zassert_equal(size, 4);
	zassert_not_null(data);
	writes++;
	return io_result;
}
static int erase_flash(const struct device *dev, off_t offset, size_t size)
{
	ARG_UNUSED(dev);
	zassert_equal(offset, 0x1000);
	zassert_equal(size, 4096);
	erases++;
	return io_result;
}
static const struct flash_parameters *get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return params;
}
static const struct flash_pages_layout pages = {.pages_count = 4, .pages_size = 4096};
static void get_layout(const struct device *dev, const struct flash_pages_layout **layout,
		       size_t *count)
{
	ARG_UNUSED(dev);
	*layout = &pages;
	*count = 1;
}
static DEVICE_API(flash, api) = {
	.read = read_flash, .write = write_flash, .erase = erase_flash,
	.get_parameters = get_parameters, .page_layout = get_layout,
};
DEVICE_DEFINE(backend_test_flash, "backend_test_flash", NULL, NULL, NULL, NULL,
	      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &api);

int __wrap_flash_area_open(uint8_t id, const struct flash_area **out)
{
	zassert_equal(id, DT_FIXED_PARTITION_ID(DT_ALIAS(ecu_nvm)));
	*out = &area;
	return open_result;
}
int __wrap_z_impl_flash_get_page_info_by_offs(const struct device *dev, off_t offset,
					    struct flash_pages_info *info)
{
	zassert_equal_ptr(dev, area.fa_dev);
	page_calls++;
	bool bad = page_calls == bad_page_call;

	*info = (struct flash_pages_info){.start_offset = offset + (bad ? page_shift : 0),
		.size = bad ? bad_page_size : page_size};
	return bad ? page_error : 0;
}
int __wrap_nvs_mount(struct nvs_fs *fs)
{
	mounted = fs;
	zassert_equal(fs->offset, area.fa_off);
	zassert_equal(fs->sector_size, page_size);
	zassert_equal(fs->sector_count, area.fa_size / page_size);
	/* NVS must receive the forwarding device, not bypass driver observation. */
	zassert_not_equal(fs->flash_device, area.fa_dev);
	zassert_equal_ptr(flash_get_parameters(fs->flash_device), params);
	const struct flash_driver_api *proxy = fs->flash_device->api;
	const struct flash_pages_layout *layout;
	size_t count;

	proxy->page_layout(fs->flash_device, &layout, &count);
	zassert_equal_ptr(layout, &pages);
	zassert_equal(count, 1);
	if (driver_error) {
		uint8_t data[4];

		zassert_equal(flash_read(fs->flash_device, 0x1000, data, sizeof(data)), io_result);
	}
	return mount_result;
}
ssize_t __wrap_nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t size)
{
	zassert_equal_ptr(fs, mounted);
	zassert_equal(id, 0x1000);
	zassert_equal(size, 4);
	if (driver_error) {
		zassert_equal(flash_read(fs->flash_device, 0x1000, data, size), io_result);
	}
	return nvs_result;
}
ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t size)
{
	zassert_equal_ptr(fs, mounted);
	zassert_equal(id, 0x1000);
	zassert_equal(size, 4);
	if (driver_error) {
		zassert_equal(flash_write(fs->flash_device, 0x1000, data, size), io_result);
	}
	return nvs_result;
}
int __wrap_nvs_delete(struct nvs_fs *fs, uint16_t id)
{
	zassert_equal_ptr(fs, mounted);
	zassert_equal(id, 0x1000);
	if (driver_error) {
		zassert_equal(flash_erase(fs->flash_device, 0x1000, 4096), io_result);
	}
	return nvs_result;
}

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	params = &parameter_sets[0];
	area = (struct flash_area){.fa_off = 0x1000, .fa_size = 4 * 4096,
		.fa_dev = DEVICE_GET(backend_test_flash)};
	mounted = NULL;
	open_result = page_calls = bad_page_call = page_error = mount_result = 0;
	io_result = nvs_result = 0;
	page_size = bad_page_size = 4096;
	page_shift = 0;
	driver_error = false;
	reads = writes = erases = 0;
}

ZTEST(nvm_backend, test_partition_readiness)
{
	open_result = -ENOENT;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	zassert_equal(page_calls, 0);
	open_result = 0;
	area.fa_dev = &unready;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	zassert_equal(page_calls, 0);
}

ZTEST(nvm_backend, test_first_page_geometry)
{
	bad_page_call = 1;
	page_error = -EINVAL;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	page_error = 0;
	page_calls = 0;
	page_shift = 1;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	page_shift = 0;
	for (size_t size = 0; size < 3; ++size) {
		page_calls = 0;
		bad_page_size = (size_t[]){0, 3000, 131072}[size];
		zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	}
	bad_page_call = 0;
	for (size_t i = 0; i < 3; ++i) {
		area.fa_size = (size_t[]){4097, 4096, (size_t)4096 * 65536}[i];
		zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	}
	zassert_is_null(mounted);
}

ZTEST(nvm_backend, test_nonuniform_partition)
{
	bad_page_call = 3; /* first page was valid; reject a later physical page */
	page_error = -EINVAL;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	page_calls = page_error = 0;
	page_shift = 1;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	page_calls = 0;
	page_shift = 0;
	bad_page_size = 2048;
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	zassert_is_null(mounted);
}

ZTEST(nvm_backend, test_write_alignment_and_capacity)
{
	for (size_t i = 0; i < 3; ++i) {
		params = &parameter_sets[i + 1];
		zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	}
	params = &parameter_sets[0];
	page_size = 256; /* power-of-two pages, but the maximum record cannot fit */
	zassert_equal(ecu_nvm_backend_init(), -ENODEV);
	zassert_is_null(mounted);
	page_size = 4096;
	zassert_ok(ecu_nvm_backend_init()); /* retry after correcting the device geometry */
}

ZTEST(nvm_backend, test_nvs_result_classification)
{
	uint8_t data[4] = {0};
	const int results[] = {-EIO, -EDEADLK, -ESPIPE, -ENOENT, -ENOSPC, -EINVAL, 0, 4};
	const int expected[] = {-EBADMSG, -EBADMSG, -EBADMSG, -ENOENT, -ENOSPC, -EIO, 0, 4};

	for (size_t i = 0; i < ARRAY_SIZE(results); ++i) {
		mount_result = MIN(results[i], 0);
		zassert_equal(ecu_nvm_backend_init(), results[i] < 0 ? expected[i] : 0);
		nvs_result = results[i];
		zassert_equal(ecu_nvm_backend_read(0x1000, data, sizeof(data)), expected[i]);
		zassert_equal(ecu_nvm_backend_write(0x1000, data, sizeof(data)),
			results[i] < 0 ? expected[i] : 0);
		nvs_result = MIN(results[i], 0);
		zassert_equal(ecu_nvm_backend_delete(0x1000), results[i] < 0 ? expected[i] : 0);
	}
	nvs_result = 2; /* A positive short write must never be reported as success. */
	zassert_equal(ecu_nvm_backend_write(0x1000, data, sizeof(data)), -EIO);
}

ZTEST(nvm_backend, test_observed_driver_failures_and_reset)
{
	uint8_t data[4] = {0};

	driver_error = true;
	io_result = -ETIMEDOUT;
	mount_result = -EIO;
	zassert_equal(ecu_nvm_backend_init(), -EIO); /* not an NVS CRC error */
	mount_result = 0;
	io_result = 0;
	zassert_ok(ecu_nvm_backend_init());
	nvs_result = -EIO;
	io_result = -ETIMEDOUT;
	zassert_equal(ecu_nvm_backend_read(0x1000, data, sizeof(data)), -EIO);
	zassert_equal(ecu_nvm_backend_write(0x1000, data, sizeof(data)), -EIO);
	zassert_equal(ecu_nvm_backend_delete(0x1000), -EIO);
	zassert_true(reads > 0 && writes > 0 && erases > 0);
	/* A clean driver operation must clear the previous observation. */
	io_result = 0;
	nvs_result = 4;
	zassert_equal(ecu_nvm_backend_read(0x1000, data, sizeof(data)), 4);
	zassert_mem_equal(data, ((uint8_t[]){0x5a, 0x5a, 0x5a, 0x5a}), 4);
	zassert_ok(ecu_nvm_backend_write(0x1000, data, sizeof(data)));
	nvs_result = 0;
	zassert_ok(ecu_nvm_backend_delete(0x1000));
}

ZTEST_SUITE(nvm_backend, NULL, NULL, reset, NULL, NULL);
