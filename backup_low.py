
import subprocess
import sys
from pathlib import Path

OUTPUT = Path("flash_backup_16k")
PORT = "COM6"

START = 0x000000
END = 0x0E0000
CHUNK_SIZE = 0x4000

OUTPUT.mkdir(exist_ok=True)


def read_chunk(address, destination, no_stub=False):
    temp_file = destination.with_suffix(".tmp")

    if temp_file.exists():
        temp_file.unlink()

    command = [
        sys.executable,
        "-m", "esptool",
        "--chip", "esp32",
        "--port", PORT,
        "--baud", "115200",
    ]

    if no_stub:
        command.append("--no-stub")

    command.extend([
        "read_flash",
        hex(address),
        hex(CHUNK_SIZE),
        str(temp_file),
    ])

    try:
        subprocess.run(command, check=True)

        if not temp_file.exists() or temp_file.stat().st_size != CHUNK_SIZE:
            raise RuntimeError("Incorrect backup file size.")

        temp_file.replace(destination)
        return True

    except Exception as error:
        print(f"Read failed: {error}")

        if temp_file.exists():
            temp_file.unlink()

        return False


for address in range(START, END, CHUNK_SIZE):
    filename = OUTPUT / f"flash_{address:06X}.bin"

    if filename.exists() and filename.stat().st_size == CHUNK_SIZE:
        print(f"SKIP: 0x{address:06X}")
        continue

    print(f"\nReading 0x{address:06X}...")

    if read_chunk(address, filename):
        print(f"PASS: 0x{address:06X}")
        continue

    print(f"Retrying 0x{address:06X} without stub...")

    if read_chunk(address, filename, no_stub=True):
        print(f"PASS (NO-STUB): 0x{address:06X}")
        continue

    print(f"\nSTOP: Failed at 0x{address:06X}.")
    sys.exit(1)


print("\nLower flash backup completed successfully.")
