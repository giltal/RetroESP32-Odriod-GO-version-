import serial
import time
import subprocess
import sys
import threading

PORT = 'COM20'
BAUD = 115200

# Flash with no_reset so we can control timing
print("Flashing (no auto-reset)...")
flash_cmd = [
    sys.executable,
    r'C:\Users\97254\esp\v3.3.1\esp-idf\components\esptool_py\esptool\esptool.py',
    '--chip', 'esp32', '--port', PORT, '--baud', '921600',
    '--before', 'default_reset', '--after', 'no_reset',
    'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', 'detect',
    '0x10000', 'build/atari800-go.bin'
]
subprocess.run(flash_cmd, timeout=30)

# Now open serial and reset via RTS
print("\nOpening serial and triggering reset...")
s = serial.Serial(PORT, BAUD, timeout=2, dsrdtr=False, rtscts=False)
s.dtr = False
s.rts = True   # EN pin low (reset)
time.sleep(0.1)
s.rts = False   # EN pin high (run)
s.flushInput()

print("Reset done, monitoring...")
start = time.time()
while time.time() - start < 30:
    line = s.readline()
    if line:
        text = line.decode('utf-8', 'replace').strip()
        if text:
            print(text)

s.close()
