#!/usr/bin/env python3
"""Enumerate the BLE + HID devices Windows already knows about.

Diagnostic only: it pairs nothing, connects to nothing, and writes nothing.
It answers two questions that a serial log cannot:

  1. Which Windows-side device owns a given BLE address? The board's serial
     log prints the peer address on every connect (`connected peer xx:...`),
     and knowing whose address that is decides whether the peer that keeps
     dropping the link is the Codex desktop app, the Settings UI, or something
     else entirely.
  2. Has Windows enumerated the board as a HID device? If no HID interface
     exists, the host will never send HID RPCs, and no amount of firmware
     tuning will make the Codex UI respond.

    python scripts/windows/ble_devices.py [filter]

`filter` is an optional case-insensitive substring; only rows whose name,
address, or id contain it are printed.
"""

import base64
import json
import os
import re
import subprocess
import sys

# Same shape as ble_scan.py: a short ASCII script delivered with
# -EncodedCommand. Keep it under the CreateProcess limit; it is far smaller
# than the GATT bridge, so the temp-.ps1 staging used by the companion is not
# needed here.
PS = r"""
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch { }
Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

function Await-Op {
    param($Operation, [Type] $ResultType, [int] $TimeoutMs = 20000)
    $task = $asTaskGeneric.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    if (-not $task.Wait($TimeoutMs)) { throw 'timeout' }
    return $task.Result
}

[void][Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
[void][Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]

function Emit-Row {
    param([string] $Source, [string] $Name, [string] $Address, $Paired, [string] $Kind, [string] $Id)
    $row = @{ source = $Source; name = $Name; address = $Address; kind = $Kind; id = $Id }
    if ($null -ne $Paired) { $row['paired'] = [bool] $Paired }
    [Console]::Out.WriteLine('@@CX@@' + ($row | ConvertTo-Json -Compress))
}

# A BLE device id looks like
#   BluetoothLE#BluetoothLEe0:0a:f6:80:71:d2-28:84:85:b2:1c:79
# i.e. <local adapter address>-<remote device address>. The local adapter
# address must not be mistaken for the peripheral, so take the *last* colon
# group pair in the string rather than the first.
function Get-RemoteAddress {
    param([string] $Id)
    $matches2 = [regex]::Matches($Id, '((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})')
    if ($matches2.Count -gt 0) { return $matches2[$matches2.Count - 1].Value.ToUpper() }
    $plain = [regex]::Matches($Id, '[0-9a-fA-F]{12}')
    if ($plain.Count -gt 0) { return $plain[$plain.Count - 1].Value.ToUpper() }
    return ''
}

# --- 1. Every BLE device Windows knows, paired or merely seen -------------
$bleSelector = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelector()
$ble = Await-Op ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($bleSelector)) ([Windows.Devices.Enumeration.DeviceInformationCollection]) 25000
foreach ($info in $ble) {
    $id = [string] $info.Id
    $paired = $null
    try { $paired = [bool] $info.Pairing.IsPaired } catch { }
    Emit-Row 'ble' ([string] $info.Name) (Get-RemoteAddress $id) $paired '' $id
}

# --- 2. Bluetooth-classic devices, so a dual-mode peer is not mistaken for
#        a BLE peripheral ------------------------------------------------
try {
    $btSelector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelector()
    $bt = Await-Op ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($btSelector)) ([Windows.Devices.Enumeration.DeviceInformationCollection]) 25000
    foreach ($info in $bt) {
        $id = [string] $info.Id
        $paired = $null
        try { $paired = [bool] $info.Pairing.IsPaired } catch { }
        Emit-Row 'classic' ([string] $info.Name) (Get-RemoteAddress $id) $paired '' $id
    }
} catch {
    Emit-Row 'classic-error' ([string] $_.Exception.Message) '' $null '' ''
}

# --- 3. HID interfaces. A board that Windows accepted as a HOGP device has
#        an entry here; a board that never finished HID enumeration does not.
try {
    $hid = Await-Op ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync('System.Devices.InterfaceClassGuid:="{4D1E55B2-F16F-11CF-88CB-001111000030}"')) ([Windows.Devices.Enumeration.DeviceInformationCollection]) 25000
    foreach ($info in $hid) {
        $id = [string] $info.Id
        $address = ''
        if ($id -match '([0-9a-fA-F]{12})') { $address = $Matches[1].ToUpper() }
        Emit-Row 'hid' ([string] $info.Name) $address $null ([string] $info.Kind) $id
    }
} catch {
    Emit-Row 'hid-error' ([string] $_.Exception.Message) '' $null '' ''
}
"""


def _run(source: str, timeout: int) -> str:
    assert source.isascii(), "generated PowerShell must stay ASCII"
    command = base64.b64encode(source.encode("utf-16-le")).decode("ascii")
    if len(command) > 32000:
        raise SystemExit("script grew past the CreateProcess limit")

    system_root = os.environ.get("SystemRoot") or r"C:\Windows"
    powershell = os.path.join(
        system_root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe"
    )
    completed = subprocess.run(
        [powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
         "-EncodedCommand", command],
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    stdout = (completed.stdout or b"").decode("utf-8", "replace")
    stderr = (completed.stderr or b"").decode("utf-8", "replace").strip()
    if not stdout.strip() and stderr:
        sys.stderr.write("powershell said:\n")
        sys.stderr.write(re.sub(r"<[^>]+>", "", stderr)[:1500] + "\n")
    return stdout


def _pretty(address: str) -> str:
    return ":".join(address[i:i + 2] for i in range(0, 12, 2)) if len(address) == 12 else address


def main() -> int:
    needle = sys.argv[1].lower() if len(sys.argv) > 1 else ""

    rows = []
    for line in _run(PS, 120).splitlines():
        line = line.strip()
        if not line.startswith("@@CX@@"):
            continue
        try:
            rows.append(json.loads(line[len("@@CX@@"):]))
        except ValueError:
            continue

    if not rows:
        sys.stderr.write("no device rows returned\n")
        return 1

    for source in ("ble", "classic", "hid"):
        group = [r for r in rows if r.get("source") == source]
        if not group:
            continue
        print(f"\n=== {source} ({len(group)}) ===")
        for item in sorted(group, key=lambda e: (e.get("name") or "", e.get("address") or "")):
            name = item.get("name") or "(unnamed)"
            address = item.get("address") or ""
            haystack = f"{name} {address} {item.get('id') or ''}".lower()
            if needle and needle not in haystack:
                continue
            paired = item.get("paired")
            flag = "" if paired is None else (" paired" if paired else " unpaired")
            print(f"  {_pretty(address):<18} {name}{flag}")

    for item in rows:
        if item.get("source", "").endswith("-error"):
            print(f"\n!! {item['source']}: {item.get('name')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
