#ifndef OEM_DATA
#define OEM_DATA

#include <stdint.h>

#define OEM_DATA_MAGIC_NUMBER 0xA5B6C7D8
#define OEM_STRUCT_VERSION 1

#define OEM_BOARD_REVISION_MAX_LEN 3
#define OEM_FACTORY_FIRMWARE_VERSION_MAX_LEN 5
#define OEM_SERIAL_NUMBER_MAX_LEN 15

#define OEM_BOARD_REVISION_BUFFER_LEN OEM_BOARD_REVISION_MAX_LEN + 1
#define OEM_FACTORY_FIRMWARE_VERSION_BUFFER_LEN OEM_FACTORY_FIRMWARE_VERSION_MAX_LEN + 1
#define OEM_SERIAL_NUMBER_BUFFER_LEN OEM_SERIAL_NUMBER_MAX_LEN + 1

#pragma pack(push, 1)
typedef struct
{
    uint32_t magic_number;   // To verify data integrity/presence
    uint16_t struct_version; // For future compatibility
    uint16_t crc16;          // CRC or checksum of the data (excluding crc16 itself)

    char board_revision[OEM_BOARD_REVISION_BUFFER_LEN];
    char factory_firmware_version[OEM_FACTORY_FIRMWARE_VERSION_BUFFER_LEN];
    char serial_number[OEM_SERIAL_NUMBER_BUFFER_LEN];
} ada_oem_data_t;
#pragma pack(pop)

#endif /* OEM_DATA */
