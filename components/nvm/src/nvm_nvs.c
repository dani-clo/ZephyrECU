#include "nvm_private.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#define NVM_PARTITION DT_ALIAS(ecu_nvm)
#if !DT_NODE_EXISTS(NVM_PARTITION)
#error "ECU_NVM requires an ecu-nvm alias to an exclusive fixed flash partition"
#endif
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PARENT(NVM_PARTITION), fixed_partitions),
	     "ECU_NVM requires a fixed partition");
/* With XIP on the same flash, merely declaring fixed partitions does not
 * reserve them in Zephyr's linker. Require an explicit nonoverlapping image
 * region (or USE_DT_CODE_PARTITION, which derives LOAD_SIZE automatically).
 */
#if defined(CONFIG_XIP) && DT_SAME_NODE(DT_GPARENT(NVM_PARTITION), DT_CHOSEN(zephyr_flash))
BUILD_ASSERT(CONFIG_FLASH_LOAD_SIZE > 0 &&
	     (CONFIG_FLASH_LOAD_OFFSET + CONFIG_FLASH_LOAD_SIZE <= DT_REG_ADDR(NVM_PARTITION) ||
	      CONFIG_FLASH_LOAD_OFFSET >= DT_REG_ADDR(NVM_PARTITION) + DT_REG_SIZE(NVM_PARTITION)),
	     "ECU_NVM: bound the linked flash image away from the NVM partition");
#endif
BUILD_ASSERT(!IS_ENABLED(CONFIG_NVS_INIT_BAD_MEMORY_REGION),
	     "ECU_NVM must not automatically format invalid storage");

static const struct flash_area *area;
static struct nvs_fs fs;
/* Serialized by ecu_nvm's mutex. Zephyr NVS uses -EIO for both data CRC
 * mismatch and driver I/O failure. Observe forwarded driver errors to avoid
 * conflating them. No flash commands, record traversal or recovery here.
 */
static bool driver_failed;

static int observed(int ret)
{
	if (ret < 0) { driver_failed = true; }
	return ret;
}
static int read_flash(const struct device *dev, off_t offset, void *data, size_t size)
{
	ARG_UNUSED(dev);
	return observed(flash_read(area->fa_dev, offset, data, size));
}
static int write_flash(const struct device *dev, off_t offset, const void *data, size_t size)
{
	ARG_UNUSED(dev);
	return observed(flash_write(area->fa_dev, offset, data, size));
}
static int erase_flash(const struct device *dev, off_t offset, size_t size)
{
	ARG_UNUSED(dev);
	return observed(flash_erase(area->fa_dev, offset, size));
}
static const struct flash_parameters *parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return flash_get_parameters(area->fa_dev);
}
static void layout(const struct device *dev, const struct flash_pages_layout **pages,
		   size_t *count)
{
	ARG_UNUSED(dev);
	const struct flash_driver_api *api = area->fa_dev->api;

	api->page_layout(area->fa_dev, pages, count);
}
static DEVICE_API(flash, nvm_flash_api) = {
	.read = read_flash, .write = write_flash, .erase = erase_flash,
	.get_parameters = parameters, .page_layout = layout,
};
/* Private forwarding device for NVS error classification, not a new backend
 * or physical driver. Underlying device readiness is checked explicitly.
 */
DEVICE_DEFINE(ecu_nvm_flash, "ecu_nvm_flash", NULL, NULL, NULL, NULL,
	      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &nvm_flash_api);

static int result(int ret)
{
	if (driver_failed) { return -EIO; }
	if (ret == -EIO || ret == -EDEADLK || ret == -ESPIPE) { return -EBADMSG; } /* NVS CRC failure, no driver error */
	if (ret == -ENOENT || ret == -ENOSPC || ret >= 0) { return ret; }
	return -EIO;
}

int ecu_nvm_backend_init(void)
{
	int ret = flash_area_open(DT_FIXED_PARTITION_ID(NVM_PARTITION), &area);

	if (ret != 0 || !device_is_ready(area->fa_dev)) { return -ENODEV; }
	struct flash_pages_info page;

	if (flash_get_page_info_by_offs(area->fa_dev, area->fa_off, &page) != 0 ||
	    page.start_offset != area->fa_off || !is_power_of_two(page.size) ||
	    page.size > 65536 || area->fa_size % page.size != 0 ||
	    area->fa_size / page.size < 2 || area->fa_size / page.size > UINT16_MAX) {
		return -ENODEV;
	}
	/* NVS sectors use one uniform physical erase page each; validate the
	 * entire area, not just its first page. No custom sector aggregation.
	 */
	size_t page_size = page.size;

	for (size_t consumed = 0; consumed < area->fa_size; consumed += page_size) {
		off_t offset = area->fa_off + (off_t)consumed;

		if (flash_get_page_info_by_offs(area->fa_dev, offset, &page) != 0 ||
		    page.start_offset != offset || page.size != page_size) { return -ENODEV; }
	}
	size_t alignment = flash_get_write_block_size(area->fa_dev);

	if (alignment == 0 || alignment > 32 || !is_power_of_two(alignment) ||
	    ROUND_UP(ECU_NVM_HEADER_SIZE + CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE + 4,
		     alignment) + 4 * ROUND_UP(8, alignment) > page_size) {
		return -ENODEV;
	}
	fs = (struct nvs_fs){
		.flash_device = DEVICE_GET(ecu_nvm_flash), .offset = area->fa_off,
		.sector_size = page_size, .sector_count = area->fa_size / page_size,
	};
	driver_failed = false;
	ret = nvs_mount(&fs);
	return result(ret);
}

ssize_t ecu_nvm_backend_read(uint16_t id, void *data, size_t size)
{
	driver_failed = false;
	return result(nvs_read(&fs, id, data, size));
}
int ecu_nvm_backend_write(uint16_t id, const void *data, size_t size)
{
	driver_failed = false;
	ssize_t ret = nvs_write(&fs, id, data, size);

	if (ret < 0) { return result(ret); }
	return ret == 0 || (size_t)ret == size ? 0 : -EIO;
}
int ecu_nvm_backend_delete(uint16_t id)
{
	driver_failed = false;
	return result(nvs_delete(&fs, id));
}
