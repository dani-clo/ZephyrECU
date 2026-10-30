#ifndef ZEPHYRECU_NVM_H_
#define ZEPHYRECU_NVM_H_

#include <stddef.h>
#include <stdint.h>

/* ID policy: 0000 invalid; 0001..0FFF framework; 1000..FFFE SWCs;
 * FFFF is reserved by NVS for internal markers, never a public record ID.
 * Components allocate fixed IDs within their range; no dynamic allocator.
 */
#define ECU_NVM_FRAMEWORK_ID_MIN 0x0001U
#define ECU_NVM_FRAMEWORK_ID_MAX 0x0fffU
#define ECU_NVM_SWC_ID_MIN       0x1000U
#define ECU_NVM_SWC_ID_MAX       0xfffeU

/* Thread-context only (ISR: -EWOULDBLOCK). Serialized, synchronous operations.
 * Init is explicit/idempotent, can retry a failed mount, never clears storage.
 * Reads/writes/deletes before successful init return -EACCES.
 */
int ecu_nvm_init(void);
/* Read copies only after complete record validation. Failure leaves data intact.
 * -ENOENT missing/deleted; -EPROTONOSUPPORT version mismatch (new or old);
 * -EMSGSIZE valid record has a different payload size; -EBADMSG invalid record
 * or NVS data CRC; -EINVAL invalid ID/pointer/size; -EIO backend failure;
 * -ENODEV unavailable partition/device or unsupported geometry.
 * Versions use all 16 bits. Payload sizes must be 1..configured maximum.
 */
int ecu_nvm_read(uint16_t id, uint16_t expected_version,
		 void *data, size_t expected_size);
/* Success is 0, including unchanged values (NVS suppresses their writes).
 * Also -ENOSPC if NVS is full. Blocking flash operations: never call write or
 * delete from Task Manager, CAN/UDS callbacks, or the system workqueue.
 * Caller retains ownership of the input and must keep it stable until return.
 */
int ecu_nvm_write(uint16_t id, uint16_t version, const void *data, size_t size);
/* Idempotent: deleting an absent record succeeds. */
int ecu_nvm_delete(uint16_t id);

#endif
