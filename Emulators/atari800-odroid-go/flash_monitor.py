import serial
import time
import subprocess
import sys

PORT = 'COM20'
BAUD = 115200

# Flash
print("Flashing...")
flash_cmd = [
    sys.executable,
    r'C:\Users\97254\esp\v3.3.1\esp-idf\components\esptool_py\esptool\esptool.py',
    '--chip', 'esp32', '--port', PORT, '--baud', '921600',
    '--before', 'default_reset', '--after', 'no_reset',
    'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', 'detect',
    '0x10000', 'build/atari800-go.bin'
]
subprocess.run(flash_cmd, timeout=60, check=True)

# Open serial and reset
s = serial.Serial(PORT, BAUD, timeout=2)
time.sleep(0.1)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)

print("Monitoring from reset...")
start = time.time()
while time.time() - start < 25:
    line = s.readline()
    if line:
        text = line.decode('utf-8', 'replace').strip()
        if text:
            print(text)

s.close()
