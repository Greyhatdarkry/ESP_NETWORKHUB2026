
from pathlib import Path
import hashlib
import sys

folder = Path("flash_backup_16k")
output = Path("ESP32_full_flash_backup.bin")

chunk_size = 0x4000
flash_size = 0x400000
chunk_count = flash_size // chunk_size

sha256 = hashlib.sha256()

try:
    with output.open("wb") as combined:
        for address in range(0, flash_size, chunk_size):
            chunk = folder / f"flash_{address:06X}.bin"

            if not chunk.exists():
                raise FileNotFoundError(f"Missing: {chunk}")

            data = chunk.read_bytes()

            if len(data) != chunk_size:
                raise ValueError(
                    f"Invalid size at 0x{address:06X}: {len(data)} bytes"
                )

            combined.write(data)
            sha256.update(data)

    if output.stat().st_size != flash_size:
        raise ValueError("Combined backup has an incorrect size.")

    print("FULL FLASH BACKUP CREATED")
    print("Chunks:", chunk_count)
    print("File:", output.resolve())
    print("Size:", output.stat().st_size, "bytes")
    print("SHA-256:", sha256.hexdigest())

except Exception as error:
    if output.exists():
        output.unlink()

    print("BACKUP COMBINATION FAILED:", error)
    sys.exit(1)
