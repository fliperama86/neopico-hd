# Autonomous Testing Procedures

## Serial Monitor - CRITICAL

**NEVER use `cat` or `screen` for serial monitoring** - they are IO-blocking and will cause the agent to hang indefinitely if the device doesn't respond.

### Correct Method: Python with Timeout

```python
import serial
import time
import sys

def monitor_serial(port, baudrate=115200, duration=20):
    """
    Non-blocking serial monitor with timeout.
    Returns immediately if port doesn't exist or after duration expires.
    """
    try:
        ser = serial.Serial(port, baudrate, timeout=0.1)
        start_time = time.time()

        while time.time() - start_time < duration:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                print(data.decode('utf-8', errors='replace'), end='')
                sys.stdout.flush()
            time.sleep(0.05)

        ser.close()
        return True
    except Exception as e:
        print(f"Serial error: {e}", file=sys.stderr)
        return False

# Usage
port = '/dev/tty.usbmodem*'  # Find actual port first
monitor_serial(port, duration=20)
```

## Testing Workflow

1. **Build**: `cd build && make <target>`
2. **Flash**: `picotool load -f ./src/<target>.uf2`
3. **Find Port**: `ls -t /dev/tty.usbmodem* | head -1`
4. **Monitor**: Python script with timeout (NEVER cat/screen)
5. **Analyze**: Parse output, make decisions
6. **Fix**: Update code if needed
7. **Repeat**: Loop until success or blocked

## When to Report to User

- ✅ Definitive results (success or identified root cause)
- ✅ Blocked by major decision (multiple valid approaches)
- ✅ Hardware not responding (device stuck/not found)
- ❌ NOT for status updates or intermediate steps
