import ctypes
import os
from pathlib import Path
import subprocess
import sys


def main():
    directory = Path(__file__).resolve().parent
    root = directory.parent.parent
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_wchar_p]
    kernel.CreateMutexW.restype = ctypes.c_void_p
    mutex = kernel.CreateMutexW(None, False, 'Local\\CodexMicroAllowanceCompanion')
    if not mutex:
        raise ctypes.WinError(ctypes.get_last_error())
    if ctypes.get_last_error() == 183:
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle(mutex)
        return 0
    logs = root / 'logs' / 'companion'
    logs.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment['PYTHONUNBUFFERED'] = '1'
    try:
        with (logs / 'background.log').open('ab', buffering=0) as output:
            process = subprocess.Popen(
                [sys.argv[1], '-NoProfile', '-NonInteractive', '-ExecutionPolicy',
                 'Bypass', '-File', str(directory / 'start-companion.ps1')],
                cwd=root, env=environment, stdin=subprocess.DEVNULL,
                stdout=output, stderr=output, creationflags=subprocess.CREATE_NO_WINDOW,
            )
            return process.wait()
    finally:
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle(mutex)


if __name__ == '__main__':
    sys.exit(main())
