import subprocess
import sys
from pathlib import Path

OUTPUT = Path("flash_backup")
PORT = "COM6"

START = 0x80000
END = 0x400000
CHUNK_SIZE = 0x10000

OUTPUT.mkdir(exist_ok=True)

for address in range(START, END, CHUNK_SIZE):
    filename = OUTPUT / f"flash_{address:06X}.bin"

    if filename.exists() and filename.stat().st_size == CHUNK_SIZE:
        print(f"SKIP 0x{address:06X}: already backed up")
        continue

    print(f"\nReading 0x{address:06X}...")

    if filename.exists():
        filename.unlink()

    command = [
        sys.executable,
        "-m", "esptool",
        "--chip", "esp32",
        "--port", PORT,
        "--baud", "115200",
        "read_flash",
        hex(address),
        hex(CHUNK_SIZE),
        str(filename),
    ]

    subprocess.run(command, check=True)

print("\nRemaining backup reads completed.")