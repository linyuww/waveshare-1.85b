"""Verify real Codex HID RPC round trips without sending control keys.

Requires hidapi. Run with the device paired; the Codex app may remain open.
"""
import argparse, hid, json, time
p = argparse.ArgumentParser()
p.add_argument('--seconds', type=int, default=120)
args = p.parse_args()
if args.seconds <= 0:
    p.error('--seconds must be positive')
devices = [d for d in hid.enumerate(0x303a, 0x8360) if d['usage_page'] == 0xff00]
print('HID paths:', devices, flush=True)
if not devices: raise SystemExit('No Codex HID interface')
device = hid.device()
device.open_path(devices[0]['path'])
started = time.monotonic()
count = 0
try:
    while time.monotonic() - started < args.seconds:
        count += 1
        method = 'sys.version' if count % 2 else 'device.status'
        payload = json.dumps({'method': method, 'id': 9000 + count}, separators=(',', ':')).encode() + b'\n'
        report = bytes([6, 2, len(payload)]) + payload
        report += bytes(64 - len(report))
        sent = time.monotonic()
        if device.write(report) != 64: raise SystemExit('Incomplete HID write')
        response = bytearray()
        while time.monotonic() - sent < 5:
            incoming = bytes(device.read(64, 1000))
            if not incoming: continue
            offset = 1 if incoming[0] == 6 else 0
            if len(incoming) < offset + 2 or incoming[offset] != 2: continue
            response.extend(incoming[offset + 2:offset + 2 + incoming[offset + 1]])
            if b'\n' in response:
                result = json.loads(response.split(b'\n')[0])
                response.clear()
                if result.get('id') == 9000 + count:
                    if 'result' not in result: raise SystemExit('RPC error: ' + str(result))
                    print('PASS', count, method, result['result'], 'ms', round((time.monotonic() - sent) * 1000), flush=True)
                    break
        else: raise SystemExit('HID response timeout')
        time.sleep(2)
finally:
    device.close()
print('PASS sustained HID round trips:', count, flush=True)
