#include "ecu_nvm.h"
#include "nvm_private.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

K_MUTEX_DEFINE(nvm_lock);
static bool initialized;
static uint8_t record[ECU_NVM_HEADER_SIZE + CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE];

static bool id_valid(uint16_t id)
{
	return id != 0 && id != UINT16_MAX;
}
static bool buffer_valid(const void *data, size_t size)
{
	return data != NULL && size != 0 && size <= CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE;
}

int ecu_nvm_init(void)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	k_mutex_lock(&nvm_lock, K_FOREVER);
	int ret = initialized ? 0 : ecu_nvm_backend_init();

	if (ret == 0) { initialized = true; }
	k_mutex_unlock(&nvm_lock);
	return ret;
}

int ecu_nvm_read(uint16_t id, uint16_t expected_version, void *data, size_t expected_size)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!id_valid(id) || !buffer_valid(data, expected_size)) { return -EINVAL; }
	k_mutex_lock(&nvm_lock, K_FOREVER);
	int ret = -EACCES;

	if (!initialized) { goto out; }
	ssize_t stored = ecu_nvm_backend_read(id, record, sizeof(record));

	if (stored < 0) { ret = (int)stored; goto out; }
	size_t length = (size_t)stored;
	/* Full record (including backend CRC) must validate before interpretation
	 * or any caller-buffer copy. Never expose partial/unchecked payloads.
	 */
	if (length < ECU_NVM_HEADER_SIZE || length > sizeof(record) ||
	    sys_get_le32(record) != ECU_NVM_MAGIC ||
	    sys_get_le16(record + 6) == 0 ||
	    sys_get_le16(record + 6) != length - ECU_NVM_HEADER_SIZE) {
		ret = -EBADMSG;
	} else if (sys_get_le16(record + 4) != expected_version) {
		ret = -EPROTONOSUPPORT;
	} else if (sys_get_le16(record + 6) != expected_size) {
		ret = -EMSGSIZE;
	} else {
		memcpy(data, record + ECU_NVM_HEADER_SIZE, expected_size);
		ret = 0;
	}
 out:
	k_mutex_unlock(&nvm_lock);
	return ret;
}

int ecu_nvm_write(uint16_t id, uint16_t version, const void *data, size_t size)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!id_valid(id) || !buffer_valid(data, size)) { return -EINVAL; }
	k_mutex_lock(&nvm_lock, K_FOREVER);
	int ret = -EACCES;

	if (initialized) {
		sys_put_le32(ECU_NVM_MAGIC, record);
		sys_put_le16(version, record + 4);
		sys_put_le16(size, record + 6);
		memcpy(record + ECU_NVM_HEADER_SIZE, data, size);
		ret = ecu_nvm_backend_write(id, record, ECU_NVM_HEADER_SIZE + size);
	}
	k_mutex_unlock(&nvm_lock);
	return ret;
}

int ecu_nvm_delete(uint16_t id)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!id_valid(id)) { return -EINVAL; }
	k_mutex_lock(&nvm_lock, K_FOREVER);
	int ret = initialized ? ecu_nvm_backend_delete(id) : -EACCES;

	k_mutex_unlock(&nvm_lock);
	return ret;
}
