import subprocess
import sys
import os

PORT = '//./COM20'
BASE = '/c/ESPIDFprojects/RetroESP32-master'
ESPTOOL = '/c/Users/97254/esp/v3.3.1/esp-idf/components/esptool_py/esptool/esptool.py'

LAUNCHER_BUILD = f'{BASE}/Launchers/retro-esp32/build'
STELLA_BIN = f'{BASE}/Emulators/stella-odroid-go/build/stella-go.bin'
A800_BIN = f'{BASE}/Emulators/atari800-odroid-go/build/atari800-go.bin'

print("Flashing carousel debug firmware (launcher + stella + atari800)...")
flash_cmd = [
    sys.executable, ESPTOOL,
    '--chip', 'esp32', '--port', PORT, '--baud', '921600',
    '--before', 'default_reset', '--after', 'hard_reset',
    'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', 'detect',
    '0x1000',   f'{LAUNCHER_BUILD}/bootloader/bootloader.bin',
    '0x8000',   f'{LAUNCHER_BUILD}/partitions.bin',
    '0xd000',   f'{LAUNCHER_BUILD}/ota_data_initial.bin',
    '0x10000',  f'{LAUNCHER_BUILD}/retro-esp32.bin',
    '0x200000', f'{LAUNCHER_BUILD}/retro-esp32.bin',
    '0x5E0000', STELLA_BIN,
    '0xA70000', A800_BIN,
]
subprocess.run(flash_cmd, check=True)
print("Flash complete!")
