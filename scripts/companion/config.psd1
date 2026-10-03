@{
    # ---------------------------------------------------------------------
    # Codex Micro companion settings
    #
    # Everything here is optional. Leave a value empty ('') to fall back to
    # auto-detection, which is the recommended default.
    # ---------------------------------------------------------------------

    # BLE address of the board. Empty = auto-detect by name via scripts/windows/ble_devices.py.
    # Only pin this if auto-detection picks the wrong device.
    # Note: re-flashing the firmware with a new bond generation CHANGES this
    # address, so a pinned value goes stale -- that is why empty is the default.
    DeviceAddress = ''

    # Seconds between refreshes. The Codex CLI's own rate-limit poll is 60 s,
    # and --watch refuses anything below 10.
    IntervalSeconds = 60

    # GATT write retries per refresh, each in a fresh session. See debug/porting-log.md 6.11.
    WriteAttempts = 4

    # Per-attempt GATT write timeout. Keep well under 30000 ms: a stuck write
    # pushes the firmware into ESP_GATT_CONGESTED and notifications stop.
    WriteTimeoutMs = 12000

    # Explicit Python interpreter. Empty = resolve automatically.
    PythonPath = ''

    # Path to codex.cmd if the Codex CLI is not on PATH. Empty = let the
    # companion find it.
    CodexPath = ''
}
