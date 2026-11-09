#ifndef ZEPHYRECU_NVM_PRIVATE_H_
#define ZEPHYRECU_NVM_PRIVATE_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ECU_NVM_HEADER_SIZE 8U
#define ECU_NVM_MAGIC UINT32_C(0x314e4345) /* little endian ECN1 */
int ecu_nvm_backend_init(void);
ssize_t ecu_nvm_backend_read(uint16_t id, void *data, size_t size);
int ecu_nvm_backend_write(uint16_t id, const void *data, size_t size);
int ecu_nvm_backend_delete(uint16_t id);
#endif
