import json
import struct
import argparse
import crcmod

OEM_DATA_MAGIC_NUMBER = 0xA5B6C7D8
OEM_STRUCT_VERSION = 1

# These should match your C/C++ struct definition precisely
OEM_BOARD_REVISION_MAX_LEN = 3
OEM_FACTORY_FIRMWARE_VERSION_MAX_LEN = 5
OEM_SERIAL_NUMBER_MAX_LEN = 15

# The maximum lengths of the strings in bytes, including null terminators
OEM_BOARD_REVISION_BUFFER_LEN = OEM_BOARD_REVISION_MAX_LEN + 1
OEM_FACTORY_FIRMWARE_VERSION_BUFFER_LEN = OEM_FACTORY_FIRMWARE_VERSION_MAX_LEN + 1
OEM_SERIAL_NUMBER_BUFFER_LEN = OEM_SERIAL_NUMBER_MAX_LEN + 1

# Struct format for Python's struct module
# '<' for little-endian
# 'I' for uint32_t (magic_number)
# 'H' for uint16_t (struct_version, crc16)
# 's' for bytes (char array)
OEM_STRUCT_FORMAT = (
    "<I"  # magic_number
    "H"  # struct_version
    "H"  # crc16
    f"{OEM_BOARD_REVISION_BUFFER_LEN}s"  # board_revision
    f"{OEM_FACTORY_FIRMWARE_VERSION_BUFFER_LEN}s"  # factory_firmware_version
    f"{OEM_SERIAL_NUMBER_BUFFER_LEN}s"  # serial_number
)

OEM_STRUCT_SIZE = struct.calcsize(OEM_STRUCT_FORMAT)
print(f"Expected OEM struct size: {OEM_STRUCT_SIZE} bytes")

def calculate_esp_rom_crc16_le_equivalent(data_bytes):
    """
    Calculates the CRC value as expected by esp_rom_crc16_le(0, data, len)
    based on esp-hal documentation.

    esp_rom_crc16_le(initial_val, data, len) behavior:
    1. Actual seed for calculation = !initial_val
    2. CRC calculation uses:
       - Poly: 0x11021
       - Seed: Result from step 1
       - Input Reflected: True
       - Output Reflected: True
       - XOR Out (for this internal step): 0x0000
       Let the result of this be R_internal.
    3. The ROM function returns !R_internal.

    For our case, initial_val is 0.
    1. Actual seed = !0 = 0xFFFF.
    2. R_internal = crcmod.mkCrcFun(poly=0x11021, initCrc=0xFFFF, rev=True, xorOut=0x0000)(data_bytes)
    3. Stored CRC (and what ROM returns) = !R_internal = R_internal ^ 0xFFFF
    """

    # Step 2: Calculate R_internal
    # Parameters for CRC-16/AUG-CCITT (or SPI-FUJITSU)
    crc_func_internal = crcmod.mkCrcFun(poly=0x11021, initCrc=0xFFFF, rev=True, xorOut=0x0000)
    r_internal = crc_func_internal(data_bytes)
    print(f"Intermediate R_internal (poly=0x11021, init=0xFFFF, rev=True): 0x{r_internal:04X}")

    # Step 3: The value returned by esp_rom_crc16_le(0,...) and to be stored is !R_internal
    final_crc_to_store = r_internal ^ 0xFFFF  # One's complement for 16-bit

    return final_crc_to_store

def main():
    parser = argparse.ArgumentParser(description="Generate OEM data binary.")
    parser.add_argument("input_json", help="Input JSON file path") # Changed name for clarity
    parser.add_argument("--output_bin", default="oem_data.bin", help="Output binary file name") # Changed name
    args = parser.parse_args()

    try:
        with open(args.input_json, "r") as f:
            contents = json.load(f)
            print(f"Loaded JSON data: {contents}")
    except FileNotFoundError:
        print(f"Error: Input JSON file not found at {args.input_json}")
        return
    except json.JSONDecodeError:
        print(f"Error: Could not decode JSON from {args.input_json}")
        return

    # TODO Better names
    board_revision_str = contents.get("boardRevision")
    if not board_revision_str:
        print("Error: Missing 'board_revision' in input JSON.")
        return

    factory_firmware_version_str = contents.get("factoryVersion")
    if not factory_firmware_version_str:
        print("Error: Missing 'factory_firmware_version' in input JSON.")
        return

    serial_number_str = contents.get("serialNumber") # Consistent naming with struct
    if not serial_number_str:
        print("Error: Missing 'serial_number' in input JSON.") # Consistent naming
        return

    # --- Prepare byte strings for packing ---
    # Ensure UTF-8 encoding, truncate if necessary, and null-terminate
    # The -1 for max length is to ensure space for the null terminator
    board_revision_bytes = (board_revision_str.encode('utf-8')[:OEM_BOARD_REVISION_BUFFER_LEN-1]).ljust(OEM_BOARD_REVISION_BUFFER_LEN, b'\0')
    factory_firmware_version_bytes = (factory_firmware_version_str.encode('utf-8')[:OEM_FACTORY_FIRMWARE_VERSION_BUFFER_LEN-1]).ljust(OEM_FACTORY_FIRMWARE_VERSION_BUFFER_LEN, b'\0')
    serial_number_bytes = (serial_number_str.encode('utf-8')[:OEM_SERIAL_NUMBER_BUFFER_LEN-1]).ljust(OEM_SERIAL_NUMBER_BUFFER_LEN, b'\0')

    # --- Pack data with CRC field initially zeroed for CRC calculation ---
    temp_packed_data = struct.pack(
        OEM_STRUCT_FORMAT,
        OEM_DATA_MAGIC_NUMBER,
        OEM_STRUCT_VERSION,
        0,  # CRC16 placeholder
        board_revision_bytes,
        factory_firmware_version_bytes,
        serial_number_bytes
    )

    # --- Calculate CRC over the entire temp_packed_data (where CRC field is 0) ---
    # This is the simplest and most robust way to ensure all fields are covered.
    # The CRC algorithm effectively ignores the zeroed CRC field's contribution if calculated this way.
    crc_value = calculate_esp_rom_crc16_le_equivalent(temp_packed_data)
    print(f"Calculated CRC16 (esp_rom_crc16_le equivalent): 0x{crc_value:04X}")

    # --- Pack the final data with the calculated CRC ---
    final_packed_data = struct.pack(
        OEM_STRUCT_FORMAT,
        OEM_DATA_MAGIC_NUMBER,
        OEM_STRUCT_VERSION,
        crc_value,  # Actual calculated CRC
        board_revision_bytes,
        factory_firmware_version_bytes,
        serial_number_bytes
    )

    if len(final_packed_data) != OEM_STRUCT_SIZE:
        print(f"Error: Final packed data size ({len(final_packed_data)}) "
              f"does not match expected struct size ({OEM_STRUCT_SIZE}).")
        return

    # Optional: Check against partition size if you know it
    # if len(final_packed_data) > 4096: # Example partition size
    #     print(f"Warning: Packed data size ({len(final_packed_data)}) might exceed typical partition size.")

    try:
        with open(args.output_bin, "wb") as f:
            f.write(final_packed_data)
        print(f"OEM data written to {args.output_bin} ({len(final_packed_data)} bytes)")
    except IOError:
        print(f"Error: Could not write to output file {args.output_bin}")

if __name__ == "__main__":
    main()