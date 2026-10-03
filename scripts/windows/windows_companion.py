#!/usr/bin/env python3
"""Windows companion for Codex Micro 1.85b: Codex quota sync + BLE GATT diagnosis.

This is the Windows port of the upstream macOS companion
(`work/codex-micro-stopwatch/companion/Sources/CodexWatchCompanion/main.swift`).
It performs exactly two jobs:

1. Read the 5-hour and weekly Codex allowances from the local Codex App Server
   over stdio (`initialize` -> `account/read` -> `account/rateLimits/read`),
   and convert both windows into the project-owned snapshot schema.
2. Write that snapshot to the private quota GATT characteristic on a *specific*
   board, addressed by its BLE address. Never by name matching, never by
   broadcast scan.

Design constraints that are deliberate, not incidental:

* No third-party packages. ``bleak`` is not installed on this machine and must
  not be installed, so BLE goes through Windows Runtime via ``powershell.exe``
  using the reflection ``AsTask`` bridge. The PowerShell source is pure ASCII,
  so it is staged into an ASCII-only temp ``.ps1`` and run with ``-File``.
  ``-EncodedCommand`` (base64 of UTF-16LE) is kept only as a fallback, because
  it inflates the ~13.5 KB bridge to ~36 KB and CreateProcess rejects a command
  line over 32767 characters with WinError 206. No non-ASCII path or payload
  can reach a ``.ps1``/``.bat`` file or a command line either way.
* Credentials are never read, tokens are never logged, and no API key is
  requested. Only the App Server's documented read methods are used; there are
  no account write operations and no login changes.
* GATT discovery asks for the *uncached* database, and a failed write is
  retried in a fresh session. Windows otherwise answers discovery from a
  per-device cache that survives re-pairing, which turns into AccessDenied /
  ERROR_CANCELLED once the firmware's attribute table has changed.
* If either window is absent (or the account cannot serve it), the program
  fails loudly. It never fabricates a snapshot.

See ``docs/COMPANION_PROTOCOL.md`` upstream for the wire contract.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime

# --------------------------------------------------------------------------
# Protocol constants (must match the firmware's codex_ble.cpp attribute table)
# --------------------------------------------------------------------------

QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
QUOTA_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"

#: Window identity comes from duration, never primary/secondary position.
FIVE_HOUR_WINDOW_MINUTES = 300
WEEKLY_WINDOW_MINUTES = 10080

#: Firmware rejects anything larger (codex_ble.cpp: `length > 512`).
MAX_PAYLOAD_BYTES = 512

#: Prefix for machine-readable event lines emitted by the PowerShell bridge.
EVENT_MARKER = "@@CX@@"

DEFAULT_INTERVAL_SECONDS = 60
MIN_INTERVAL_SECONDS = 10

#: Optional fast path: a local Codex quota service that already holds the
#: allowance and answers instantly.
#:
#: The App Server path spawns a fresh `codex app-server` (a Node CLI) on every
#: run and waits for it to boot, which is where the tens of seconds of "why do
#: I always have to wait" went. A service that is already up and already knows
#: the quota answers in milliseconds, so it is tried first and the App Server
#: remains the fallback when nothing is listening.
DEFAULT_BRIDGE_URL = "http://127.0.0.1:8787/quota"

#: Keep this short. The bridge is a localhost HTTP call; if it is not there,
#: failing fast matters more than waiting.
BRIDGE_TIMEOUT_SECONDS = 3.0

#: Per-attempt ceiling for a GATT write. Kept well under 30 s on purpose: a
#: stuck write pins the GATT session, and the firmware keeps notifying at 1 Hz
#: into a peer that is not servicing ATT. That is what pushes it into
#: ESP_GATT_CONGESTED (GATTS conf status 143), after which every notification
#: fails until the link is rebuilt.
DEFAULT_WRITE_TIMEOUT_MS = 12000

#: Windows cancels a GATT write with HRESULT 0x800704C7 (ERROR_CANCELLED) and
#: can answer characteristic discovery with AccessDenied when the cached
#: BluetoothLEDevice / GATT session has gone stale. On this board the failure
#: correlates with the HID host (ChatGPT Desktop) actively using the same
#: connection -- the device log shows `RPC method=v.oai.thstatus` alongside a
#: failed write, and no ATT write PDU ever reaches the firmware. Each retry
#: runs in a brand-new PowerShell process, so it gets a fresh device object and
#: session; the widening gaps exist to step over the other app's busy window.
DEFAULT_WRITE_ATTEMPTS = 4
RETRY_BACKOFF_SECONDS = (3, 8, 15)

#: `--probe-only` keeps the GATT link up for at least this long. The point is to
#: observe whether the link survives the firmware's idle timers, not to prove
#: anything about the HID host.
DEFAULT_PROBE_HOLD_SECONDS = 130

ADDRESS_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")


class CompanionError(Exception):
    """Any expected, user-facing failure. Never carries credentials."""


# --------------------------------------------------------------------------
# Codex App Server client (stdio JSON-RPC)
# --------------------------------------------------------------------------


def resolve_codex_path(explicit: str | None = None) -> str:
    """Locate the local `codex` launcher without assuming a shell."""
    if explicit:
        candidate = os.path.abspath(explicit)
        if not os.path.isfile(candidate):
            raise CompanionError(f"--codex-path does not exist: {candidate}")
        return candidate

    found = shutil.which("codex")
    if found:
        return found

    appdata = os.environ.get("APPDATA") or ""
    localappdata = os.environ.get("LOCALAPPDATA") or ""
    candidates = [
        os.path.join(appdata, "npm", "codex.cmd"),
        os.path.join(appdata, "npm", "codex.exe"),
        os.path.join(localappdata, "Programs", "codex", "codex.exe"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate

    raise CompanionError(
        "cannot find the `codex` CLI; pass --codex-path <path to codex.cmd>"
    )


class AppServerClient:
    """Minimal stdio client for the Codex App Server.

    Only documented read methods are exposed. Nothing here writes to the
    account, and stderr is kept only as a bounded tail for error messages.
    """

    def __init__(self, codex_path: str, verbose: bool = False, timeout: float = 20.0):
        self._verbose = verbose
        self._timeout = timeout
        self._lock = threading.Condition()
        self._responses: dict[int, dict] = {}
        self._stderr_tail = ""
        self._next_id = 1
        self._closed = False

        self._log(f"starting app server: {codex_path} app-server --listen stdio://")
        try:
            self._proc = subprocess.Popen(
                [codex_path, "app-server", "--listen", "stdio://"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
                creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
            )
        except OSError as exc:
            raise CompanionError(
                f"failed to launch the Codex App Server ({codex_path}): {exc}"
            ) from exc

        threading.Thread(target=self._pump_stdout, daemon=True).start()
        threading.Thread(target=self._pump_stderr, daemon=True).start()

        self._request(
            "initialize",
            {
                "clientInfo": {
                    "name": "codex_micro_windows_companion",
                    "title": "Codex Micro Windows Companion",
                    "version": "0.1.0",
                },
                "capabilities": {
                    "optOutNotificationMethods": [
                        "item/agentMessage/delta",
                        "item/reasoning/textDelta",
                    ]
                },
            },
            request_id=0,
            timeout=15.0,
        )
        self._notify("initialized", {})

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
        except OSError:
            pass
        # The launcher on Windows is a .cmd wrapper, so the real node process is
        # a grandchild. Kill the whole tree while the wrapper is still alive so
        # the grandchild cannot be orphaned; this is scoped to our own PID.
        if self._proc.poll() is None:
            try:
                subprocess.run(
                    ["taskkill", "/PID", str(self._proc.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10,
                    check=False,
                )
            except (OSError, subprocess.SubprocessError):
                pass
        try:
            self._proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        for stream in (self._proc.stdout, self._proc.stderr):
            try:
                if stream is not None:
                    stream.close()
            except OSError:
                pass

    def __enter__(self) -> "AppServerClient":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # -- transport ---------------------------------------------------------

    def _log(self, message: str) -> None:
        if self._verbose:
            print(f"[app-server] {message}", file=sys.stderr)

    def _pump_stdout(self) -> None:
        stream = self._proc.stdout
        if stream is None:
            return
        for raw in iter(stream.readline, b""):
            line = raw.strip()
            if not line:
                continue
            try:
                message = json.loads(line.decode("utf-8", "replace"))
            except ValueError:
                self._log(f"ignoring non-JSON line ({len(line)} bytes)")
                continue
            if not isinstance(message, dict):
                continue
            message_id = message.get("id")
            if not isinstance(message_id, int):
                continue  # notification, not a reply
            with self._lock:
                self._responses[message_id] = message
                self._lock.notify_all()

    def _pump_stderr(self) -> None:
        stream = self._proc.stderr
        if stream is None:
            return
        for raw in iter(stream.readline, b""):
            text = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not text:
                continue
            with self._lock:
                self._stderr_tail = (self._stderr_tail + "\n" + text)[-2000:]

    def _write(self, payload: dict) -> None:
        if self._proc.stdin is None or self._proc.poll() is not None:
            raise CompanionError("the Codex App Server exited unexpectedly")
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
        try:
            self._proc.stdin.write(data)
            self._proc.stdin.flush()
        except OSError as exc:
            raise CompanionError(f"failed to write to the App Server: {exc}") from exc

    def _notify(self, method: str, params: dict) -> None:
        self._write({"method": method, "params": params})

    def _wait(self, request_id: int, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        with self._lock:
            while request_id not in self._responses:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._lock.wait(remaining)
                if self._proc.poll() is not None and request_id not in self._responses:
                    break
            if request_id in self._responses:
                return self._responses.pop(request_id)
            tail = self._stderr_tail.strip()
        detail = f" App Server stderr tail: {tail}" if tail else ""
        raise CompanionError(
            f"timed out after {timeout:.0f}s waiting for the App Server reply "
            f"(id={request_id}).{detail}"
        )

    def _request(
        self,
        method: str,
        params: dict,
        request_id: int | None = None,
        timeout: float | None = None,
    ) -> dict:
        if request_id is None:
            request_id = self._next_id
            self._next_id += 1
        effective_timeout = self._timeout if timeout is None else timeout
        self._write({"method": method, "id": request_id, "params": params})
        return self._wait(request_id, effective_timeout)

    # -- documented read methods ------------------------------------------

    def read_account(self) -> dict:
        """`account/read` -- plan/type only. The email is deliberately dropped."""
        response = self._request("account/read", {})
        result = self._unwrap(response, "account/read")
        account = result.get("account")
        if not isinstance(account, dict):
            raise CompanionError(
                "the Codex App Server reports no signed-in account; "
                "sign in with the Codex CLI first"
            )
        return {
            "type": account.get("type"),
            "planType": account.get("planType"),
            "requiresOpenaiAuth": result.get("requiresOpenaiAuth"),
        }

    def read_rate_limits(self) -> dict:
        response = self._request("account/rateLimits/read", {})
        return self._unwrap(response, "account/rateLimits/read")

    @staticmethod
    def _unwrap(response: dict, method: str) -> dict:
        error = response.get("error")
        if isinstance(error, dict):
            message = error.get("message") or json.dumps(error)
            raise CompanionError(f"{method} failed: {message}")
        if error:
            raise CompanionError(f"{method} failed: {error}")
        result = response.get("result")
        if not isinstance(result, dict):
            raise CompanionError(f"{method} returned no result object")
        return result


# --------------------------------------------------------------------------
# Quota selection (pure functions -- unit tested)
# --------------------------------------------------------------------------


def select_codex_bucket(rate_limits_result: dict) -> dict:
    """Pick the primary Codex bucket. Never silently fall back to Spark."""
    by_limit_id = rate_limits_result.get("rateLimitsByLimitId")
    if isinstance(by_limit_id, dict):
        codex = by_limit_id.get("codex")
        if isinstance(codex, dict):
            return codex

    legacy = rate_limits_result.get("rateLimits")
    if isinstance(legacy, dict) and legacy.get("limitId") == "codex":
        return legacy

    raise CompanionError(
        "the App Server response contains no `codex` rate-limit bucket; "
        "refusing to substitute a secondary bucket"
    )


def select_window(bucket: dict, duration_minutes: int, label: str) -> tuple[str, dict]:
    """Return a Codex window identified by duration, never by slot name."""
    observed: list[str] = []
    for slot in ("primary", "secondary"):
        window = bucket.get(slot)
        if not isinstance(window, dict):
            continue
        minutes = window.get("windowDurationMins")
        observed.append(f"{slot}={minutes!r}")
        if isinstance(minutes, (int, float)) and int(minutes) == duration_minutes:
            return slot, window

    detail = ", ".join(observed) if observed else "no primary/secondary windows"
    raise CompanionError(
        f"no {label} Codex window (windowDurationMins={duration_minutes}) is "
        f"available in the codex bucket ({detail})"
    )


def select_five_hour_window(bucket: dict) -> tuple[str, dict]:
    return select_window(bucket, FIVE_HOUR_WINDOW_MINUTES, "5-hour")


def select_weekly_window(bucket: dict) -> tuple[str, dict]:
    return select_window(bucket, WEEKLY_WINDOW_MINUTES, "weekly")


def window_snapshot(window: dict, now: float, label: str) -> dict:
    used_raw = window.get("usedPercent")
    if not isinstance(used_raw, (int, float)) or isinstance(used_raw, bool):
        raise CompanionError(f"the {label} window has no numeric `usedPercent`")

    reset_raw = window.get("resetsAt")
    if not isinstance(reset_raw, (int, float)) or isinstance(reset_raw, bool):
        raise CompanionError(f"the {label} window has no numeric `resetsAt`")

    used = min(100.0, max(0.0, float(used_raw)))
    return {
        "remaining_percent": _tidy_number(100.0 - used),
        "reset_in_seconds": max(0, int(float(reset_raw) - now)),
        "used_percent": used,
        "reset_at": int(float(reset_raw)),
    }


def build_snapshot(rate_limits_result: dict, now: float | None = None) -> dict:
    """Convert both Codex windows into the dashboard's wire snapshot."""
    if now is None:
        now = time.time()

    bucket = select_codex_bucket(rate_limits_result)
    five_hour_slot, five_hour_window = select_five_hour_window(bucket)
    weekly_slot, weekly_window = select_weekly_window(bucket)
    five_hour = window_snapshot(five_hour_window, now, "5-hour")
    weekly = window_snapshot(weekly_window, now, "weekly")

    snapshot = {
        "five_hour_remaining_percent": five_hour["remaining_percent"],
        "five_hour_reset_in_seconds": five_hour["reset_in_seconds"],
        "weekly_remaining_percent": weekly["remaining_percent"],
        "weekly_reset_in_seconds": weekly["reset_in_seconds"],
    }
    snapshot["_source"] = {
        "limit_id": bucket.get("limitId"),
        "five_hour": {
            "slot": five_hour_slot,
            "window_minutes": FIVE_HOUR_WINDOW_MINUTES,
            "used_percent": five_hour["used_percent"],
            "reset_at": five_hour["reset_at"],
        },
        "weekly": {
            "slot": weekly_slot,
            "window_minutes": WEEKLY_WINDOW_MINUTES,
            "used_percent": weekly["used_percent"],
            "reset_at": weekly["reset_at"],
        },
        "plan_type": bucket.get("planType"),
    }
    return snapshot


def _tidy_number(value: float):
    """Emit 0 / 100 as ints and everything else rounded to one decimal."""
    rounded = round(float(value), 1)
    if rounded == int(rounded):
        return int(rounded)
    return rounded


# --------------------------------------------------------------------------
# Optional local bridge fast path
# --------------------------------------------------------------------------


def _parse_iso_epoch(text: str) -> float:
    """Epoch seconds for an ISO-8601 timestamp; naive input is read as local."""
    parsed = datetime.fromisoformat(text)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=datetime.now().astimezone().tzinfo)
    return parsed.timestamp()


def bridge_to_rate_limits(payload: dict) -> dict:
    """Reshape a local bridge's quota reply into App Server form.

    Converting into the App Server shape rather than straight into a snapshot
    keeps exactly one window-selection rule for both sources: `build_snapshot()`
    picks both windows by duration and never by slot name.
    """
    status = payload.get("status")
    if status not in (None, "ok"):
        raise CompanionError(f"the bridge reports status={status!r}")

    limit_id = payload.get("limitId") or "codex"
    bucket: dict = {"limitId": limit_id}
    if payload.get("planType") is not None:
        bucket["planType"] = payload["planType"]

    for slot in ("primary", "secondary"):
        minutes = payload.get(f"{slot}WindowMinutes")
        if not isinstance(minutes, (int, float)) or isinstance(minutes, bool):
            continue
        window: dict = {"windowDurationMins": int(minutes)}

        used = payload.get(f"{slot}UsedPercent")
        if isinstance(used, (int, float)) and not isinstance(used, bool):
            window["usedPercent"] = float(used)

        resets = payload.get(f"{slot}ResetsAt")
        if isinstance(resets, str):
            try:
                window["resetsAt"] = _parse_iso_epoch(resets)
            except ValueError:
                pass

        bucket[slot] = window

    if len(bucket) <= 1:
        raise CompanionError("the bridge reply carries no usable rate-limit window")
    return {"rateLimitsByLimitId": {limit_id: bucket}}


def read_bridge_quota(url: str, timeout: float = BRIDGE_TIMEOUT_SECONDS) -> dict:
    """Fetch and reshape the bridge's quota reply, or raise CompanionError."""
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read(64 * 1024)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        raise CompanionError(f"no quota service at {url}: {exc}") from exc

    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise CompanionError(f"{url} did not return JSON: {exc}") from exc
    if not isinstance(payload, dict):
        raise CompanionError(f"{url} returned {type(payload).__name__}, not an object")
    return bridge_to_rate_limits(payload)


def public_snapshot(snapshot: dict) -> dict:
    """Strip local diagnostics so only the wire fields remain."""
    return {
        "five_hour_remaining_percent": snapshot["five_hour_remaining_percent"],
        "five_hour_reset_in_seconds": snapshot["five_hour_reset_in_seconds"],
        "weekly_remaining_percent": snapshot["weekly_remaining_percent"],
        "weekly_reset_in_seconds": snapshot["weekly_reset_in_seconds"],
    }


def encode_payload(snapshot: dict) -> bytes:
    """Encode the wire payload exactly as the firmware's parser expects."""
    wire = public_snapshot(snapshot)
    payload = json.dumps(wire, separators=(",", ":"), sort_keys=True).encode("utf-8")
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise CompanionError(
            f"snapshot payload is {len(payload)} bytes; the firmware limit is "
            f"{MAX_PAYLOAD_BYTES}"
        )
    return payload


def format_reset(seconds: int) -> str:
    if seconds >= 86_400:
        return f"{seconds // 86_400}d {(seconds % 86_400) // 3_600}h"
    if seconds >= 3_600:
        return f"{seconds // 3_600}h {(seconds % 3_600) // 60}m"
    return f"{max(0, seconds) // 60}m"


# --------------------------------------------------------------------------
# Bluetooth address handling
# --------------------------------------------------------------------------


def parse_device_address(text: str) -> str:
    """Normalize `AA:BB:CC:DD:EE:FF`; reject anything else."""
    if not isinstance(text, str) or not ADDRESS_RE.match(text.strip()):
        raise CompanionError(
            f"invalid --device-address {text!r}; expected AA:BB:CC:DD:EE:FF"
        )
    return text.strip().upper()


def address_to_uint64(address: str) -> int:
    return int(address.replace(":", ""), 16)


# --------------------------------------------------------------------------
# PowerShell / WinRT GATT bridge
# --------------------------------------------------------------------------

PS_BRIDGE_TEMPLATE = r"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch { }

Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

$script:AsTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

function Await-Op {
    param(
        [Parameter(Mandatory = $true)] $Operation,
        [Parameter(Mandatory = $true)] [Type] $ResultType,
        [int] $TimeoutMs = 30000
    )
    $method = $script:AsTaskGeneric.MakeGenericMethod($ResultType)
    $task = $method.Invoke($null, @($Operation))
    try {
        if (-not $task.Wait($TimeoutMs)) {
            throw ("timeout after {0} ms waiting for {1}" -f $TimeoutMs, $ResultType.Name)
        }
    } catch [System.AggregateException] {
        # Task.Wait() wraps the real fault in an AggregateException whose
        # Message is the useless "One or more errors occurred". Unwrap to the
        # innermost cause so a failed GATT write reports its actual HRESULT
        # (e.g. an ATT protocol error) instead of hiding it.
        $inner = $_.Exception
        while (($inner -is [System.AggregateException]) -and ($null -ne $inner.InnerException)) {
            $inner = $inner.InnerException
        }
        throw (New-Object System.Exception (("{0}: {1}" -f $inner.GetType().Name, $inner.Message)))
    }
    return $task.Result
}

function Emit-Event {
    param([hashtable] $Payload)
    # [Console]::Out instead of Write-Output on purpose. Write-Output feeds the
    # PowerShell pipeline, so when Emit-Event is called from inside another
    # function its output is appended to THAT function's return value. That
    # silently turned Invoke-WriteAsync's return into an array of event strings
    # plus the real status, which made the multi-strategy fallback below think
    # a result had been produced and never run.
    [Console]::Out.WriteLine('@@CX@@' + ($Payload | ConvertTo-Json -Compress -Depth 6))
}

function Fail {
    param([string] $Code, [string] $Message)
    Emit-Event @{ event = 'error'; code = $Code; message = $Message }
    exit 10
}

[void][Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Bluetooth.BluetoothCacheMode, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
[void][Windows.Security.Cryptography.CryptographicBuffer, Windows.Security.Cryptography, ContentType = WindowsRuntime]

# Windows answers GATT discovery from a per-device cache by default. That cache
# survives re-pairing and is NOT invalidated when the peripheral changes its
# attribute table -- which this firmware did during porting (battery CCCD added,
# quota permission widened). A stale cache is exactly what produces
# AccessDenied on characteristic discovery and ERROR_CANCELLED (0x800704C7) on
# write, with no ATT write PDU ever reaching the board. Always ask for the
# uncached database; fall back to the cached call only if this Windows build
# rejects the overload.
function Get-ServicesFresh {
    param($Dev)
    try {
        return (Await-Op ($Dev.GetGattServicesAsync([Windows.Devices.Bluetooth.BluetoothCacheMode]::Uncached)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult]) 30000)
    } catch {
        Emit-Event @{ event = 'cache_mode'; scope = 'services'; mode = 'cached_fallback'; error = [string] $_.Exception.Message }
        return (Await-Op ($Dev.GetGattServicesAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult]) 30000)
    }
}

function Get-CharacteristicsFresh {
    param($Svc)
    try {
        return (Await-Op ($Svc.GetCharacteristicsAsync([Windows.Devices.Bluetooth.BluetoothCacheMode]::Uncached)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult]) 30000)
    } catch {
        Emit-Event @{ event = 'cache_mode'; scope = 'characteristics'; mode = 'cached_fallback'; error = [string] $_.Exception.Message }
        return (Await-Op ($Svc.GetCharacteristicsAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult]) 30000)
    }
}

$serviceUuid = [guid] '7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01'
$writeUuid = [guid] '7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02'
$address = [uint64] __ADDRESS__
$doWrite = __DO_WRITE__
$holdSeconds = __HOLD_SECONDS__
# Deliberately shorter than the default 30 s. A stuck write holds the GATT
# session open, and the firmware keeps notifying at 1 Hz into a peer that is
# not servicing ATT; that is what drives it into ESP_GATT_CONGESTED (conf
# status 143), after which notifications fail until the link is rebuilt.
$writeTimeoutMs = __WRITE_TIMEOUT_MS__

Emit-Event @{ event = 'start'; address = ('{0:X12}' -f $address); write = $doWrite; hold_seconds = $holdSeconds }

$device = $null
$session = $null
try {
    $device = [Windows.Devices.Bluetooth.BluetoothLEDevice] (Await-Op ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync($address)) ([Windows.Devices.Bluetooth.BluetoothLEDevice]) 20000)
    if ($null -eq $device) {
        Fail 'device_not_found' 'Windows has no BluetoothLEDevice for this address (never paired or not present)'
    }

    $paired = $null
    try { $paired = [bool] $device.DeviceInformation.Pairing.IsPaired } catch { $paired = $null }

    Emit-Event @{
        event = 'device'
        name = [string] $device.Name
        connection_status = [string] $device.ConnectionStatus
        device_id = [string] $device.DeviceId
        paired = $paired
    }

    $session = Await-Op ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession]::FromDeviceIdAsync($device.BluetoothDeviceId)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession]) 20000
    $session.MaintainConnection = $true
    Emit-Event @{ event = 'session'; status = [string] $session.SessionStatus; maintain = [bool] $session.MaintainConnection }

    # A GattSession goes Active asynchronously. Discovering services before that
    # returns GattCommunicationStatus::Unreachable and, on this firmware, can
    # drop the link, so wait for the link first and then retry discovery.
    $linkUp = $false
    for ($i = 1; $i -le 20; $i++) {
        Start-Sleep -Seconds 2
        if ([string] $session.SessionStatus -eq 'Active') { $linkUp = $true; break }
    }
    Emit-Event @{
        event = 'link'
        active = $linkUp
        session_status = [string] $session.SessionStatus
        connection_status = [string] $device.ConnectionStatus
        max_pdu = $session.MaxPduSize
    }
    if (-not $linkUp) {
        Fail 'link_not_established' 'the GATT session never became Active; the board is not connectable right now'
    }
    $linkUpAt = Get-Date

    $servicesResult = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $servicesResult = Get-ServicesFresh $device
        Emit-Event @{
            event = 'services'
            attempt = $attempt
            status = [string] $servicesResult.Status
            protocol_error = [string] $servicesResult.ProtocolError
            count = $servicesResult.Services.Count
            connection_status = [string] $device.ConnectionStatus
            session_status = [string] $session.SessionStatus
        }
        if ([string] $servicesResult.Status -eq 'Success') { break }
        Start-Sleep -Seconds 4
    }
    # A discovery failure is recorded, not fatal: --probe-only still wants to
    # know whether the link survives once discovery has been attempted.
    $discoveryOk = ([string] $servicesResult.Status -eq 'Success')
    if (-not $discoveryOk) {
        Emit-Event @{
            event = 'error'
            code = 'service_discovery_failed'
            message = ('GATT service discovery returned ' + [string] $servicesResult.Status)
        }
    }

    $service = $null
    $characteristic = $null
    if ($discoveryOk) {
        foreach ($candidate in $servicesResult.Services) {
            if ($candidate.Uuid -eq $serviceUuid) { $service = $candidate; break }
        }
        if ($null -eq $service) {
            $discoveryOk = $false
            Emit-Event @{ event = 'error'; code = 'quota_service_missing'; message = 'the device does not expose service 7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01' }
        } else {
            Emit-Event @{ event = 'quota_service'; uuid = [string] $service.Uuid }
        }
    }

    if ($discoveryOk) {
        $charsResult = Get-CharacteristicsFresh $service
        Emit-Event @{
            event = 'characteristics'
            status = [string] $charsResult.Status
            protocol_error = [string] $charsResult.ProtocolError
            count = $charsResult.Characteristics.Count
        }
        if ([string] $charsResult.Status -ne 'Success') {
            $discoveryOk = $false
            Emit-Event @{ event = 'error'; code = 'characteristic_discovery_failed'; message = ('GATT characteristic discovery returned ' + [string] $charsResult.Status) }
        } else {
            foreach ($candidate in $charsResult.Characteristics) {
                if ($candidate.Uuid -eq $writeUuid) { $characteristic = $candidate; break }
            }
            if ($null -eq $characteristic) {
                $discoveryOk = $false
                Emit-Event @{ event = 'error'; code = 'quota_characteristic_missing'; message = 'the quota service does not expose characteristic 7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02' }
            } else {
                Emit-Event @{
                    event = 'quota_characteristic'
                    uuid = [string] $characteristic.Uuid
                    properties = [string] $characteristic.CharacteristicProperties
                    handle = $characteristic.AttributeHandle
                }
            }
        }
    }

    $writeOk = $false
    if ($doWrite) {
        if ($null -eq $characteristic) {
            Emit-Event @{ event = 'error'; code = 'write_skipped'; message = 'the quota characteristic was not discovered; nothing was written' }
        } else {
            $bytes = [Convert]::FromBase64String('__PAYLOAD_B64__')
            Emit-Event @{ event = 'write_begin'; bytes = $bytes.Length }
            $buffer = [Windows.Security.Cryptography.CryptographicBuffer]::CreateFromByteArray($bytes)

            # PowerShell's overload resolution on WinRT instance methods is not
            # reliable: on some Windows builds WriteValueWithResultAsync(IBuffer)
            # fails with "no overload takes 1 argument" even though the API is
            # present. Walk the candidate calls instead of betting on one.
            $writeStatus = 'Unreachable'
            $writeProtocolError = ''
            $writeError = ''

            # Diagnostic: what PowerShell actually sees on the characteristic.
            try {
                $methodDump = @()
                foreach ($mi in $characteristic.GetType().GetMethods()) {
                    $methodDump += ($mi.Name + '(' + $mi.GetParameters().Count + ')')
                }
                Emit-Event @{ event = 'write_methods'; type = [string] $characteristic.GetType().FullName; methods = ($methodDump -join ',') }
            } catch { }

            # PowerShell's binder frequently fails on WinRT instance methods
            # ("no overload takes N arguments"). Reflection bypasses the binder.
            #
            # The two candidate names below issue the *same* ATT Write Request,
            # so trying the second is only worth it when the first failed to
            # *bind* -- PowerShell refusing an overload reports instantly. A
            # timeout is a different failure: no ATT response came back at all,
            # which means the session is dead (typically the link was released
            # underneath us). Retrying the other overload would burn a second
            # full timeout against the same dead link, so report it immediately
            # and let the caller start a fresh session instead.
            function Invoke-WriteAsync {
                param($Char, $Buf)
                foreach ($name in @('WriteValueWithResultAsync', 'WriteValueAsync')) {
                    foreach ($mi in ($Char.GetType().GetMethods() | Where-Object { $_.Name -eq $name -and $_.GetParameters().Count -eq 1 })) {
                        try {
                            $op = $mi.Invoke($Char, @($Buf))
                            if ($name -eq 'WriteValueAsync') {
                                return ([string] (Await-Op $op ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]) $writeTimeoutMs))
                            }
                            $res = Await-Op $op ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult]) $writeTimeoutMs
                            return ([string] $res.Status)
                        } catch {
                            $attemptError = [string] $_.Exception.Message
                            Emit-Event @{ event = 'write_attempt'; method = $name; error = $attemptError }
                            if ($attemptError -match 'timeout') { return '__dead_session__' }
                        }
                    }
                }
                return $null
            }

            $reflected = Invoke-WriteAsync $characteristic $buffer
            if ($reflected -eq '__dead_session__') {
                # Leave the status at Unreachable: nothing came back, so there is
                # no GATT status to report.
                $writeError = 'no ATT response before the write timeout; the session was abandoned so the caller can reconnect'
            } elseif ($null -ne $reflected) {
                $writeStatus = $reflected
            }

            if ($null -eq $reflected) {
                try {
                    $writeResult = Await-Op ($characteristic.WriteValueWithResultAsync($buffer)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult]) $writeTimeoutMs
                    $writeStatus = [string] $writeResult.Status
                    $writeProtocolError = [string] $writeResult.ProtocolError
                } catch {
                    $writeError = [string] $_.Exception.Message
                    try {
                        $writeResult = Await-Op ($characteristic.WriteValueWithResultAsync($buffer, [Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteOption]::WriteWithResponse)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult]) $writeTimeoutMs
                        $writeStatus = [string] $writeResult.Status
                        $writeProtocolError = [string] $writeResult.ProtocolError
                    } catch {
                        $writeError = [string] $_.Exception.Message
                        try {
                            $writeStatus = [string] (Await-Op ($characteristic.WriteValueAsync($buffer)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]) $writeTimeoutMs)
                        } catch {
                            $writeError = [string] $_.Exception.Message
                            $writeStatus = 'Unreachable'
                        }
                    }
                }
            }
            Emit-Event @{
                event = 'write_result'
                status = $writeStatus
                protocol_error = $writeProtocolError
                error = $writeError
            }
            if ($writeStatus -eq 'Success') {
                $writeOk = $true
                Emit-Event @{ event = 'write_ack'; status = 'Success' }
            } else {
                # Report the status we actually observed. The previous code read
                # $writeResult.Status, which is unset on the reflected path and
                # produced a message ending in nothing ("GATT write returned ").
                Emit-Event @{
                    event = 'error'
                    code = 'write_failed'
                    status = [string] $writeStatus
                    protocol_error = [string] $writeProtocolError
                    message = ('GATT write returned {0}' -f [string] $writeStatus)
                }
            }
        }
    }

    if ($holdSeconds -gt 0) {
        $deadline = $linkUpAt.AddSeconds($holdSeconds)
        $tick = 0
        while ((Get-Date) -lt $deadline) {
            $tick = $tick + 1
            Emit-Event @{
                event = 'link_status'
                tick = $tick
                connection_status = [string] $device.ConnectionStatus
                session_status = [string] $session.SessionStatus
                remaining_seconds = [int] [math]::Ceiling(($deadline - (Get-Date)).TotalSeconds)
            }
            Start-Sleep -Seconds 10
        }
        Emit-Event @{
            event = 'hold_complete'
            held_seconds = [int] ((Get-Date) - $linkUpAt).TotalSeconds
            requested_seconds = $holdSeconds
        }
    }

    Emit-Event @{ event = 'done'; write = $doWrite; discovery_ok = $discoveryOk; write_ok = $writeOk }
    if ($discoveryOk -and ((-not $doWrite) -or $writeOk)) { exit 0 }
    exit 10
}
catch {
    Emit-Event @{ event = 'error'; code = 'exception'; message = $_.Exception.Message }
    exit 11
}
finally {
    if ($null -ne $session) { try { $session.Dispose() } catch { } }
    if ($null -ne $device) { try { $device.Dispose() } catch { } }
}
"""


# --------------------------------------------------------------------------
# Pairing repair
# --------------------------------------------------------------------------
#
# Reflashing this board does not create a new Bluetooth address: the firmware
# derives it deterministically from the factory MAC, so it stays the same
# across every build. If Windows keeps a bond for that address while the board
# has forgotten its own (NVS erase, an SMP failure, a firmware change), the
# host keeps trying to resume encryption with a key the board no longer has.
# The local SMP procedure fails, Bluedroid drops the bond, the host terminates
# the link, and Windows ends up with the device listed as *not paired* while
# refusing to start a fresh pairing on its own. From the outside the board just
# looks "connectable but unusable".
#
# Unpair-then-pair is the documented way out. It is deliberately a separate,
# explicit mode: it changes this machine's Bluetooth state, so it never runs as
# a side effect of a normal quota write.

PS_PAIR_TEMPLATE = r"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch { }

Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

$script:AsTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

function Await-Op {
    param(
        [Parameter(Mandatory = $true)] $Operation,
        [Parameter(Mandatory = $true)] [Type] $ResultType,
        [int] $TimeoutMs = 30000
    )
    $method = $script:AsTaskGeneric.MakeGenericMethod($ResultType)
    $task = $method.Invoke($null, @($Operation))
    try {
        if (-not $task.Wait($TimeoutMs)) {
            throw ("timeout after {0} ms waiting for {1}" -f $TimeoutMs, $ResultType.Name)
        }
    } catch [System.AggregateException] {
        $inner = $_.Exception
        while (($inner -is [System.AggregateException]) -and ($null -ne $inner.InnerException)) {
            $inner = $inner.InnerException
        }
        throw (New-Object System.Exception (("{0}: {1}" -f $inner.GetType().Name, $inner.Message)))
    }
    return $task.Result
}

function Emit-Event {
    param([hashtable] $Payload)
    [Console]::Out.WriteLine('@@CX@@' + ($Payload | ConvertTo-Json -Compress -Depth 6))
}

[void][Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
[void][Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
[void][Windows.Devices.Enumeration.DevicePairingKinds, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
[void][Windows.Devices.Enumeration.DevicePairingResult, Windows.Devices.Enumeration, ContentType = WindowsRuntime]

$address = [uint64] __ADDRESS__

$device = $null
try {
    $device = [Windows.Devices.Bluetooth.BluetoothLEDevice] (Await-Op ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync($address)) ([Windows.Devices.Bluetooth.BluetoothLEDevice]) 20000)
    if ($null -eq $device) {
        Emit-Event @{ event = 'error'; code = 'device_not_found'; message = 'Windows has no BluetoothLEDevice for this address' }
        exit 10
    }

    $info = $device.DeviceInformation
    $pairing = $info.Pairing
    Emit-Event @{
        event = 'device'
        name = [string] $device.Name
        device_id = [string] $device.DeviceId
        paired = [bool] $pairing.IsPaired
        can_pair = [bool] $pairing.CanPair
        connection_status = [string] $device.ConnectionStatus
    }

    if ($pairing.IsPaired) {
        $unpair = Await-Op ($pairing.UnpairAsync()) ([Windows.Devices.Enumeration.DeviceUnpairingResult]) 30000
        Emit-Event @{ event = 'unpair'; status = [string] $unpair.Status }
        Start-Sleep -Seconds 2
    }

    # ConfirmOnly needs no UI: this board pairs Just Works.
    $pairResult = Await-Op ($pairing.PairAsync([Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmOnly)) ([Windows.Devices.Enumeration.DevicePairingResult]) 60000
    Emit-Event @{
        event = 'pair'
        status = [string] $pairResult.Status
        protection_level_used = [string] $pairResult.ProtectionLevelUsed
    }

    Start-Sleep -Seconds 3
    Emit-Event @{
        event = 'done'
        paired = [bool] $pairing.IsPaired
        connection_status = [string] $device.ConnectionStatus
    }
    if ([string] $pairResult.Status -eq 'Paired') { exit 0 }
    exit 10
}
catch {
    Emit-Event @{ event = 'error'; code = 'exception'; message = $_.Exception.Message }
    exit 11
}
finally {
    if ($null -ne $device) { try { $device.Dispose() } catch { } }
}
"""


def build_ps_pair_script(address: str) -> str:
    """Render the ASCII-only unpair-then-pair script for one board."""
    source = PS_PAIR_TEMPLATE.replace("__ADDRESS__", str(address_to_uint64(address)))
    if not source.isascii():
        raise CompanionError("internal error: generated pairing script is not ASCII")
    return source


def build_ps_bridge(
    address: str,
    payload: bytes | None = None,
    hold_seconds: int = 0,
    write_timeout_ms: int = DEFAULT_WRITE_TIMEOUT_MS,
) -> str:
    """Render the ASCII-only PowerShell bridge source.

    Everything variable is injected as an ASCII literal (hex, base64, int), so
    the source can never contain a non-ASCII character regardless of the paths
    or payload on this machine.
    """
    do_write = "$true" if payload is not None else "$false"
    payload_b64 = base64.b64encode(payload).decode("ascii") if payload else ""
    source = PS_BRIDGE_TEMPLATE
    source = source.replace("__ADDRESS__", str(address_to_uint64(address)))
    source = source.replace("__DO_WRITE__", do_write)
    source = source.replace("__HOLD_SECONDS__", str(int(hold_seconds)))
    source = source.replace("__WRITE_TIMEOUT_MS__", str(int(write_timeout_ms)))
    source = source.replace("__PAYLOAD_B64__", payload_b64)
    if not source.isascii():
        raise CompanionError("internal error: generated PowerShell source is not ASCII")
    return source


def encode_ps_command(source: str) -> str:
    return base64.b64encode(source.encode("utf-16-le")).decode("ascii")


#: Windows CreateProcess rejects a command line longer than 32767 characters.
#: ``-EncodedCommand`` needs base64 of UTF-16LE, i.e. 2.67x the source size, so
#: the 13.5 KB bridge renders to ~36 KB and CreateProcess fails with
#: WinError 206 ("filename or extension is too long") before PowerShell ever
#: starts. The bridge is therefore staged into an ASCII-only temp ``.ps1`` and
#: run with ``-File``; ``-EncodedCommand`` stays as a fallback for the (much
#: smaller) case where a temp file cannot be created.
MAX_COMMAND_LINE_CHARS = 32000

_PS_SCRIPT_PREFIX = "codex_micro_ble_"


def stage_ps_script(source: str) -> str:
    """Write the ASCII bridge source to a temp ``.ps1`` and return its path.

    The caller is responsible for deleting the file. A non-ASCII temp path is
    rejected because PowerShell 5.1 decodes ``.ps1`` files as ANSI, which would
    corrupt the script; the fallback in :func:`run_ps_bridge` handles that.
    """
    directory = tempfile.gettempdir()
    if not directory.isascii():
        raise CompanionError(
            f"temp directory is not ASCII ({directory}); cannot stage the bridge"
        )
    handle, path = tempfile.mkstemp(prefix=_PS_SCRIPT_PREFIX, suffix=".ps1")
    try:
        with os.fdopen(handle, "w", encoding="ascii", newline="\r\n") as stream:
            stream.write(source)
    except Exception:
        try:
            os.unlink(path)
        except OSError:
            pass
        raise
    return path


_CLIXML_TEXT_RE = re.compile(r"<S S=\"[^\"]*\">(.*?)</S>", re.DOTALL)
_CLIXML_ESCAPE_RE = re.compile(r"_x([0-9A-Fa-f]{4})_")


def clean_ps_stderr(text: str) -> str:
    """Turn PowerShell's CLIXML stderr wrapper into readable plain text.

    When stderr is redirected, Windows PowerShell serialises errors as CLIXML.
    Surfacing that XML to the user hides the actual message, so unwrap it.
    """
    if "#< CLIXML" not in text:
        return text.strip()

    chunks = _CLIXML_TEXT_RE.findall(text)
    if not chunks:
        return text.strip()
    decoded = "\n".join(
        _CLIXML_ESCAPE_RE.sub(lambda m: chr(int(m.group(1), 16)), chunk).strip()
        for chunk in chunks
    )
    return decoded.replace("\r", "").strip()


def find_powershell() -> str:
    """Prefer Windows PowerShell 5.1: WinRT projection is only reliable there."""
    system_root = os.environ.get("SystemRoot") or r"C:\Windows"
    candidate = os.path.join(
        system_root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe"
    )
    if os.path.isfile(candidate):
        return candidate
    found = shutil.which("powershell.exe")
    if found:
        return found
    raise CompanionError("cannot find powershell.exe; the BLE bridge needs it")


class BleResult:
    def __init__(self, returncode: int, events: list[dict], stderr: str):
        self.returncode = returncode
        self.events = events
        self.stderr = stderr

    def find(self, name: str) -> dict | None:
        for event in self.events:
            if event.get("event") == name:
                return event
        return None

    def last_error(self) -> dict | None:
        errors = [e for e in self.events if e.get("event") == "error"]
        return errors[-1] if errors else None

    @property
    def write_acknowledged(self) -> bool:
        """True only when the ATT write actually returned Success."""
        return self.find("write_ack") is not None


def run_ps_bridge(source: str, timeout: float) -> BleResult:
    powershell = find_powershell()

    # Preferred path: stage the ASCII source as a .ps1 and use -File. This is
    # the only form that survives the CreateProcess command-line limit, which
    # -EncodedCommand blows through for a bridge this size.
    script_path: str | None = None
    try:
        script_path = stage_ps_script(source)
    except (CompanionError, OSError):
        script_path = None

    if script_path is not None:
        arguments = [
            powershell,
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            script_path,
        ]
    else:
        command = encode_ps_command(source)
        if len(command) > MAX_COMMAND_LINE_CHARS:
            raise CompanionError(
                "the PowerShell bridge cannot be launched: no ASCII temp file "
                "available and the encoded command line exceeds the Windows "
                f"limit ({len(command)} > {MAX_COMMAND_LINE_CHARS} chars)"
            )
        arguments = [
            powershell,
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-EncodedCommand",
            command,
        ]

    try:
        completed = subprocess.run(
            arguments,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise CompanionError(
            f"the PowerShell BLE bridge did not finish within {timeout:.0f}s"
        ) from exc
    finally:
        if script_path is not None:
            try:
                os.unlink(script_path)
            except OSError:
                pass

    stdout = (completed.stdout or b"").decode("utf-8", "replace")
    stderr = clean_ps_stderr((completed.stderr or b"").decode("utf-8", "replace"))

    events: list[dict] = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line.startswith(EVENT_MARKER):
            continue
        try:
            parsed = json.loads(line[len(EVENT_MARKER):])
        except ValueError:
            continue
        if isinstance(parsed, dict):
            events.append(parsed)

    return BleResult(completed.returncode, events, stderr.strip())


# --------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------


class Options:
    def __init__(self) -> None:
        self.codex_path: str | None = None
        self.json_only = False
        self.once = False
        self.watch = False
        self.probe_only = False
        self.interval = DEFAULT_INTERVAL_SECONDS
        self.verbose = False
        self.device_address: str | None = None
        self.write_attempts = DEFAULT_WRITE_ATTEMPTS
        self.write_timeout_ms = DEFAULT_WRITE_TIMEOUT_MS
        self.repair_pairing = False
        self.hold_seconds = DEFAULT_PROBE_HOLD_SECONDS
        self.bridge_url: str | None = DEFAULT_BRIDGE_URL


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="windows_companion.py",
        description=(
            "Read the weekly Codex allowance from the local Codex App Server and "
            "write it to a specific Codex Micro board over BLE (Windows port)."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  windows_companion.py --json-only\n"
            "  windows_companion.py --device-address 28:84:85:B2:1C:78 --once\n"
            "  windows_companion.py --device-address 28:84:85:B2:1C:78 --probe-only\n"
            "  windows_companion.py --device-address 28:84:85:B2:1C:78 --watch --interval 60\n"
        ),
    )
    parser.add_argument("--codex-path", help="path to the codex launcher (codex.cmd)")
    parser.add_argument(
        "--json-only",
        action="store_true",
        help="read and print the snapshot JSON only; never touch Bluetooth",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="read the allowance once and write it once (default BLE mode)",
    )
    parser.add_argument(
        "--watch",
        action="store_true",
        help="keep refreshing; requires --interval >= 10",
    )
    parser.add_argument(
        "--probe-only",
        action="store_true",
        help=(
            "discover the quota service on the given board and hold the link; "
            "never writes. A GATT link here does not prove the HID host is online."
        ),
    )
    parser.add_argument(
        "--hold-seconds",
        type=int,
        default=DEFAULT_PROBE_HOLD_SECONDS,
        help=f"link hold time for --probe-only (default {DEFAULT_PROBE_HOLD_SECONDS})",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_INTERVAL_SECONDS,
        help=f"refresh interval in seconds for --watch (default {DEFAULT_INTERVAL_SECONDS})",
    )
    parser.add_argument(
        "--device-address",
        help="BLE address of the target board, e.g. 28:84:85:B2:1C:78",
    )
    parser.add_argument(
        "--repair-pairing",
        action="store_true",
        help=(
            "unpair and pair the board again, to clear a stale bond that makes "
            "Windows list it as unpaired while refusing to connect; changes this "
            "machine's Bluetooth state, so it never runs implicitly"
        ),
    )
    parser.add_argument(
        "--write-attempts",
        type=int,
        default=DEFAULT_WRITE_ATTEMPTS,
        help=(
            "how many times to retry a failed GATT write, each in a fresh "
            f"session (default {DEFAULT_WRITE_ATTEMPTS})"
        ),
    )
    parser.add_argument(
        "--write-timeout-ms",
        type=int,
        default=DEFAULT_WRITE_TIMEOUT_MS,
        help=(
            "per-attempt GATT write timeout in ms; keep this well under 30000 "
            f"(default {DEFAULT_WRITE_TIMEOUT_MS})"
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print App Server and BLE progress on stderr",
    )
    parser.add_argument(
        "--bridge-url",
        default=DEFAULT_BRIDGE_URL,
        help=(
            "local quota service to try before spawning the App Server "
            f"(default {DEFAULT_BRIDGE_URL}); it answers in milliseconds, "
            "while the App Server has to boot a Node CLI first"
        ),
    )
    parser.add_argument(
        "--no-bridge",
        action="store_true",
        help=(
            "never call the local quota service; always read the allowance "
            "from the App Server"
        ),
    )
    return parser


def options_from_args(argv: list[str] | None = None) -> Options:
    parser = build_parser()
    namespace = parser.parse_args(argv)

    options = Options()
    options.codex_path = namespace.codex_path
    options.json_only = namespace.json_only
    options.once = namespace.once
    options.watch = namespace.watch
    options.probe_only = namespace.probe_only
    options.interval = namespace.interval
    options.verbose = namespace.verbose
    options.hold_seconds = namespace.hold_seconds
    options.write_attempts = namespace.write_attempts
    options.write_timeout_ms = namespace.write_timeout_ms
    options.repair_pairing = namespace.repair_pairing
    options.bridge_url = None if namespace.no_bridge else (namespace.bridge_url or None)

    if namespace.device_address:
        try:
            options.device_address = parse_device_address(namespace.device_address)
        except CompanionError as exc:
            parser.error(str(exc))

    _validate(options, parser)
    return options


def _validate(options: Options, parser: argparse.ArgumentParser) -> None:
    if options.probe_only and options.json_only:
        parser.error("--probe-only and --json-only are mutually exclusive")
    if options.probe_only and options.watch:
        parser.error("--probe-only and --watch are mutually exclusive")
    if options.watch and options.interval < MIN_INTERVAL_SECONDS:
        parser.error(f"--interval must be at least {MIN_INTERVAL_SECONDS} seconds")
    if options.hold_seconds < 0:
        parser.error("--hold-seconds must not be negative")
    if options.write_attempts < 1:
        parser.error("--write-attempts must be at least 1")
    if options.write_timeout_ms < 1000:
        parser.error("--write-timeout-ms must be at least 1000")

    needs_bluetooth = (
        options.probe_only or options.once or options.watch or options.repair_pairing
    )
    if needs_bluetooth and not options.device_address:
        parser.error(
            "--device-address is required for every Bluetooth mode; "
            "this tool never scans for a matching name"
        )
    if options.probe_only and options.once:
        parser.error("--probe-only and --once are mutually exclusive")
    if options.repair_pairing and (
        options.probe_only or options.json_only or options.once or options.watch
    ):
        parser.error(
            "--repair-pairing is a standalone mode; it cannot be combined with "
            "--probe-only, --json-only, --once or --watch"
        )

    if not (
        options.json_only
        or options.probe_only
        or options.once
        or options.watch
        or options.repair_pairing
    ):
        parser.error(
            "choose a mode: --json-only, --once, --watch, --probe-only or "
            "--repair-pairing (see --help)"
        )


# --------------------------------------------------------------------------
# Modes
# --------------------------------------------------------------------------


def read_snapshot(options: Options) -> dict:
    # Fast path first. Spawning `codex app-server` is the slow part of every
    # run, so an already-running local quota service is worth trying before it;
    # if nothing answers, fall through to the App Server unchanged.
    if options.bridge_url:
        try:
            rate_limits = read_bridge_quota(options.bridge_url)
            snapshot = build_snapshot(rate_limits)
        except CompanionError as exc:
            if options.verbose:
                print(f"[bridge] {exc}; falling back to the App Server",
                      file=sys.stderr)
        else:
            if options.verbose:
                source = snapshot["_source"]
                print(
                    "[bridge] Codex windows: 5h slot={five_hour[slot]} "
                    "used={five_hour[used_percent]}%; weekly slot={weekly[slot]} "
                    "used={weekly[used_percent]}% plan={plan_type}".format(**source),
                    file=sys.stderr,
                )
            return snapshot

    codex_path = resolve_codex_path(options.codex_path)
    with AppServerClient(codex_path, verbose=options.verbose) as client:
        account = client.read_account()
        if options.verbose:
            print(
                f"[app-server] account type={account.get('type')} "
                f"plan={account.get('planType')}",
                file=sys.stderr,
            )
        rate_limits = client.read_rate_limits()
    snapshot = build_snapshot(rate_limits)
    if options.verbose:
        source = snapshot["_source"]
        print(
            "[app-server] Codex windows: 5h slot={five_hour[slot]} "
            "used={five_hour[used_percent]}%; weekly slot={weekly[slot]} "
            "used={weekly[used_percent]}% plan={plan_type}".format(**source),
            file=sys.stderr,
        )
    return snapshot


def report_snapshot(snapshot: dict) -> None:
    wire = public_snapshot(snapshot)
    print(json.dumps(wire, separators=(",", ":"), sort_keys=True))


def run_probe(options: Options) -> int:
    assert options.device_address is not None
    source = build_ps_bridge(
        options.device_address, payload=None, hold_seconds=options.hold_seconds
    )
    # Worst case: 40s to bring the link up, 3 discovery attempts, then the hold.
    timeout = 200 + options.hold_seconds
    if options.verbose:
        print(
            f"[ble] probing {options.device_address}; holding the link for "
            f"{options.hold_seconds}s (a GATT link is not proof of an HID host)",
            file=sys.stderr,
        )
    result = run_ps_bridge(source, timeout)

    if options.verbose:
        for event in result.events:
            print(f"[ble] {json.dumps(event, ensure_ascii=False)}", file=sys.stderr)

    device = result.find("device")
    if device is None:
        error = result.last_error()
        detail = error.get("message") if error else (result.stderr or "no events")
        print(f"BLE probe failed: {detail}", file=sys.stderr)
        return 1

    link = result.find("link")
    services = result.find("services")
    characteristic = result.find("quota_characteristic")
    hold = result.find("hold_complete")
    ticks = [e for e in result.events if e.get("event") == "link_status"]
    final_tick = ticks[-1] if ticks else None

    print(
        "device={name} paired={paired} "
        "link_active={link_active} link_established={link_established} "
        "service_discovery={discovery} quota_service={service} "
        "quota_characteristic={characteristic} "
        "held_seconds={held}/{requested} "
        "last_link_status={last}".format(
            name=device.get("name"),
            paired=device.get("paired"),
            link_active=link.get("active") if link else None,
            link_established=device.get("connection_status"),
            discovery=services.get("status") if services else "not attempted",
            service="yes" if result.find("quota_service") else "no",
            characteristic=(
                f"yes({characteristic.get('properties')})" if characteristic else "no"
            ),
            held=hold.get("held_seconds") if hold else 0,
            requested=options.hold_seconds,
            last=(
                f"{final_tick.get('connection_status')}/{final_tick.get('session_status')}"
                if final_tick
                else "n/a"
            ),
        )
    )

    if characteristic is None:
        error = result.last_error()
        detail = error.get("message") if error else "the quota characteristic was not found"
        print(f"BLE probe incomplete: {detail}", file=sys.stderr)
        return 1
    if hold is None:
        print(
            "BLE probe incomplete: the link did not stay up for the requested hold",
            file=sys.stderr,
        )
        return 1
    return 0


def run_repair_pairing(options: Options) -> int:
    assert options.device_address is not None
    source = build_ps_pair_script(options.device_address)
    print(
        f"repairing the pairing for {options.device_address} "
        "(unpair, then pair again; this board pairs Just Works)",
        file=sys.stderr,
    )
    result = run_ps_bridge(source, timeout=180)

    if options.verbose:
        for event in result.events:
            print(f"[pair] {json.dumps(event, ensure_ascii=False)}", file=sys.stderr)

    device = result.find("device")
    if device is None:
        error = result.last_error()
        detail = error.get("message") if error else (result.stderr or "no events")
        print(f"pairing repair failed: {detail}", file=sys.stderr)
        return 1

    unpair = result.find("unpair")
    pair = result.find("pair")
    done = result.find("done")

    print(
        "device={name} was_paired={paired} can_pair={can_pair} "
        "unpair={unpair} pair={pair} paired_now={now} link={link}".format(
            name=device.get("name"),
            paired=device.get("paired"),
            can_pair=device.get("can_pair"),
            unpair=unpair.get("status") if unpair else "not needed",
            pair=pair.get("status") if pair else "not attempted",
            now=done.get("paired") if done else None,
            link=done.get("connection_status") if done else None,
        )
    )

    if pair is None or pair.get("status") != "Paired":
        error = result.last_error()
        if error:
            print(f"pairing repair incomplete: {error.get('message')}", file=sys.stderr)
        print(
            "pairing repair incomplete: Windows did not report a fresh pairing. "
            "Remove the board in Settings > Bluetooth and add it again.",
            file=sys.stderr,
        )
        return 1

    print(
        "pairing repaired; if the Codex UI still ignores the device, reopen the "
        "ChatGPT desktop app so it re-binds the HID interface"
    )
    return 0


def run_ble_once(options: Options, snapshot: dict) -> int:
    assert options.device_address is not None
    payload = encode_payload(snapshot)

    # Each attempt spawns a new PowerShell process, i.e. a new
    # BluetoothLEDevice object and a new GATT session. That is the point:
    # ERROR_CANCELLED (0x800704C7) is raised against a *stale* cached session,
    # so retrying inside one process would keep failing on the same object.
    attempts = max(1, options.write_attempts)
    last_detail = "no attempt was made"

    for attempt in range(1, attempts + 1):
        source = build_ps_bridge(
            options.device_address,
            payload=payload,
            hold_seconds=0,
            write_timeout_ms=options.write_timeout_ms,
        )
        if options.verbose:
            prefix = f"[ble] attempt {attempt}/{attempts}:" if attempts > 1 else "[ble]"
            print(
                f"{prefix} writing {len(payload)} bytes to {options.device_address}: "
                f"{payload.decode('utf-8')}",
                file=sys.stderr,
            )

        # 40 s to bring the link up, 3 discovery attempts at 30 s, then the
        # (shortened) write timeout -- with headroom on top.
        timeout = 200 + (options.write_timeout_ms / 1000.0)
        result = run_ps_bridge(source, timeout=timeout)

        if options.verbose:
            for event in result.events:
                print(f"[ble] {json.dumps(event, ensure_ascii=False)}", file=sys.stderr)

        if result.write_acknowledged:
            wire = public_snapshot(snapshot)
            print(
                "write acknowledged by ATT: 5h remaining {five_hour}%, resets in "
                "{reset}; weekly remaining {weekly}%".format(
                    five_hour=wire["five_hour_remaining_percent"],
                    reset=format_reset(wire["five_hour_reset_in_seconds"]),
                    weekly=wire["weekly_remaining_percent"],
                )
            )
            return 0

        error = result.last_error()
        last_detail = (
            error.get("message") if error else (result.stderr or "no ATT acknowledgement")
        )
        write_result = result.find("write_result")
        if write_result is not None:
            last_detail = f"{last_detail} (GATT status {write_result.get('status')})"

        if attempt < attempts:
            backoff = RETRY_BACKOFF_SECONDS[
                min(attempt - 1, len(RETRY_BACKOFF_SECONDS) - 1)
            ]
            print(
                f"BLE write attempt {attempt}/{attempts} failed: {last_detail}; "
                f"retrying with a fresh GATT session in {backoff}s",
                file=sys.stderr,
            )
            time.sleep(backoff)

    print(f"BLE write failed after {attempts} attempt(s): {last_detail}", file=sys.stderr)
    return 1


def run(argv: list[str] | None = None) -> int:
    options = options_from_args(argv)

    if options.repair_pairing:
        return run_repair_pairing(options)

    if options.probe_only:
        return run_probe(options)

    if options.json_only:
        snapshot = read_snapshot(options)
        report_snapshot(snapshot)
        if options.watch:
            while True:
                time.sleep(options.interval)
                report_snapshot(read_snapshot(options))
        return 0

    # BLE modes.
    first = True
    while True:
        snapshot = read_snapshot(options)
        if first or options.verbose:
            report_snapshot(snapshot)
        code = run_ble_once(options, snapshot)
        if code != 0:
            return code
        first = False
        if not options.watch:
            return 0
        time.sleep(options.interval)


def main() -> int:
    try:
        return run()
    except CompanionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
