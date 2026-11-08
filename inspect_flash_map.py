
from pathlib import Path

BACKUP = Path("ESP32_full_flash_backup.bin")

FLASH_SIZE = 0x400000
SECTOR_SIZE = 0x1000

# Current application partition ends at 0x110000.
# Proposed LittleFS partition starts at 0x190000.
START = 0x110000
END = FLASH_SIZE
STORAGE_START = 0x190000

if not BACKUP.exists():
    raise SystemExit("ERROR: Full flash backup not found.")

if BACKUP.stat().st_size != FLASH_SIZE:
    raise SystemExit("ERROR: Backup is not exactly 4 MB.")

data = BACKUP.read_bytes()

occupied = []
erased = []

for address in range(START, END, SECTOR_SIZE):
    sector = data[address:address + SECTOR_SIZE]
    non_erased = sum(byte != 0xFF for byte in sector)

    entry = (address, non_erased)

    if non_erased == 0:
        erased.append(entry)
    else:
        occupied.append(entry)


def print_ranges(title, entries):
    print(f"\n{title}")

    if not entries:
        print("  NONE")
        return

    first = previous = entries[0][0]
    total = entries[0][1]

    for address, count in entries[1:]:
        if address == previous + SECTOR_SIZE:
            previous = address
            total += count
        else:
            print(
                f"  0x{first:06X} - 0x{previous + SECTOR_SIZE - 1:06X}"
                f" | non-FF bytes: {total}"
            )
            first = previous = address
            total = count

    print(
        f"  0x{first:06X} - 0x{previous + SECTOR_SIZE - 1:06X}"
        f" | non-FF bytes: {total}"
    )


storage_sectors = [
    item for item in occupied
    if item[0] >= STORAGE_START
]

storage_erased = [
    item for item in erased
    if item[0] >= STORAGE_START
]

print("ESP32 FLASH OCCUPANCY REPORT")
print("============================")
print(f"Backup: {BACKUP.resolve()}")
print(f"Analyzed range: 0x{START:06X} - 0x{END - 1:06X}")
print(f"Sector size: {SECTOR_SIZE} bytes")
print(f"Total sectors analyzed: {(END - START) // SECTOR_SIZE}")

print("\nENTIRE ANALYZED RANGE")
print("Occupied sectors:", len(occupied))
print("Fully erased sectors:", len(erased))

print("\nPROPOSED LITTLEFS REGION")
print(f"Range: 0x{STORAGE_START:06X} - 0x{END - 1:06X}")
print("Occupied sectors:", len(storage_sectors))
print("Fully erased sectors:", len(storage_erased))

print_ranges("OCCUPIED SECTOR RANGES", occupied)
print_ranges("ERASED SECTOR RANGES", erased)
