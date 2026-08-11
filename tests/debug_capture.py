import sys, time, json
sys.path.insert(0, '.')
from pxview_automation import McpClient, McpError

c = McpClient()
c.connect()
print('Connected:', c.connected, 'Tools:', len(c.tools))

# Get demo device
devs = c.get_devices()
print('Devices:', devs)
dev_id = devs[0]['id']
print('Device ID:', dev_id)

# Connect device
print('Connecting device...')
try:
    c.connect_device(dev_id)
    print('Device connected')
except Exception as e:
    print('Connect error:', e)

time.sleep(1)

# Get capture status before
status = c.get_capture_status()
print('Status before capture:', json.dumps(status))

# Start a 0.5s timed capture
print('Starting 0.5s capture...')
result = c.start_capture(dev_id, {
    'digitalChannels': [0, 1],
    'digitalSampleRate': 1000000,
}, {
    'timedCaptureMode': {'durationSeconds': 0.5}
})
print('start_capture result:', result)

# Check status immediately
time.sleep(0.2)
status = c.get_capture_status()
print('Status during capture:', json.dumps(status))

# Wait 2 seconds and check status again (without using wait_capture)
time.sleep(2)
status = c.get_capture_status()
print('Status after 2s:', json.dumps(status))

# Try wait_capture with short timeout
print('Calling wait_capture...')
try:
    result = c.wait_capture(timeout_seconds=10, timeout=15)
    print('wait_capture result:', result)
except McpError as e:
    print('wait_capture error:', e)

# Final status
status = c.get_capture_status()
print('Final status:', json.dumps(status))
