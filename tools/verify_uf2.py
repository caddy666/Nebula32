import struct
import sys

# Expected Magic Numbers
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30

# Raspberry Pi Family IDs
PICO_FAMILIES = {
    0xe48bff56: "Raspberry Pi RP2040 (Pico 1)",
    0x4fb9600e: "Raspberry Pi RP2350 ARM (Pico 2)",
    0x2dba19eb: "Raspberry Pi RP2350 RISC-V (Pico 2)"
}

def verify_uf2(filename):
    try:
        with open(filename, 'rb') as f:
            block = f.read(512)
            
        if len(block) < 512:
            print("❌ Invalid: File is smaller than a single 512-byte UF2 block.")
            return

        # Unpack the header and footer layout
        # V1: Magic0, V2: Magic1, Flags, TargetAddr, PayloadSize, BlockNo, NumBlocks, FamilyID
        header = struct.unpack("<IIIIIIII", block[0:32])
        magic_end = struct.unpack("<I", block[508:512])[0]

        if header[0] != UF2_MAGIC_START0 or header[1] != UF2_MAGIC_START1 or magic_end != UF2_MAGIC_END:
            print("❌ Invalid: File structure is not a valid UF2 format.")
            return

        flags = header[2]
        family_id = header[7]

        # Check if the Family ID flag (0x00002000) is set
        if not (flags & 0x00002000):
            print("❌ Invalid: UF2 file does not contain a specific Chip Family ID assignment.")
            return

        if family_id in PICO_FAMILIES:
            print(f"✅ Confirmed: Valid UF2 firmware targeting {PICO_FAMILIES[family_id]}")
        else:
            print(f"⚠️ Warning: Valid UF2 file, but built for a non-Pico architecture (Family ID: 0x{family_id:08x}).")

    except Exception as e:
        print(f"❌ Error reading file: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python verify_uf2.py <filename.uf2>")
    else:
        verify_uf2(sys.argv[1])
