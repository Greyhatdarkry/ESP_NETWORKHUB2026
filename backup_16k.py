
import subprocess
import sys
import shutil
from pathlib import Path

OUTPUT = Path("flash_backup_16k")
PORT = "COM6"

START = 0xE0000
END = 0x400000
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

        if not temp_file.exists():
            raise RuntimeError("Output file was not created.")

        if temp_file.stat().st_size != CHUNK_SIZE:
            raise RuntimeError(
                f"Incorrect file size: {temp_file.stat().st_size}"
            )

        temp_file.replace(destination)
        return True

    except Exception as error:
        print(f"Read failed: {error}")

        if temp_file.exists():
            temp_file.unlink()

        return False


for address in range(START, END, CHUNK_SIZE):

    filename = OUTPUT / f"flash_{address:06X}.bin"

    # Preserve previously completed backup chunks.
    if filename.exists() and filename.stat().st_size == CHUNK_SIZE:
        print(f"SKIP: 0x{address:06X}")
        continue

    print(f"\nReading 0x{address:06X}...")

    # Reuse the manually verified 0x288000 backup.
    if address == 0x288000:
        existing_retry = Path("retry_288000.bin")

        if existing_retry.exists() and existing_retry.stat().st_size == CHUNK_SIZE:
            shutil.copy2(existing_retry, filename)
            print("PASS: Reused verified retry_288000.bin")
            continue

    # First attempt: normal esptool read.
    if read_chunk(address, filename, no_stub=False):
        print(f"PASS: 0x{address:06X}")
        continue

    # Second attempt: bypass the esptool stub.
    print(f"Retrying 0x{address:06X} without stub...")

    if read_chunk(address, filename, no_stub=True):
        print(f"PASS (NO-STUB): 0x{address:06X}")
        continue

    print(f"\nSTOP: Could not read 0x{address:06X}.")
    print("Existing completed backup chunks have been preserved.")
    sys.exit(1)


print("\nAll requested backup chunks have been read successfully.")
