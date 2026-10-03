import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / 'scripts/companion/background_companion.py'
MODULE_SPEC = importlib.util.spec_from_file_location('background_companion', MODULE_PATH)
background = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(background)


class BackgroundTests(unittest.TestCase):
    def test_hidden_child_and_handle_cleanup(self):
        kernel = mock.Mock()
        kernel.CreateMutexW.return_value = 123
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'scripts' / 'companion' / 'background_companion.py'
            with mock.patch.object(background, '__file__', str(script)), \
                 mock.patch.object(background.ctypes, 'WinDLL', return_value=kernel, create=True), \
                 mock.patch.object(background.ctypes, 'get_last_error', return_value=0, create=True), \
                 mock.patch.object(background.subprocess, 'CREATE_NO_WINDOW', 0x08000000, create=True), \
                 mock.patch.object(background.subprocess, 'Popen') as spawn, \
                 mock.patch.object(background.sys, 'argv', ['background', 'pwsh.exe']):
                spawn.return_value.wait.return_value = 0
                self.assertEqual(background.main(), 0)
                arguments, options = spawn.call_args
                self.assertEqual(arguments[0][0], 'pwsh.exe')
                self.assertEqual(options['creationflags'], 0x08000000)
                self.assertEqual(options['env']['PYTHONUNBUFFERED'], '1')
                self.assertEqual(options['stdin'], background.subprocess.DEVNULL)
                kernel.CloseHandle.assert_called_once_with(123)

    def test_duplicate_does_not_spawn(self):
        kernel = mock.Mock()
        kernel.CreateMutexW.return_value = 123
        with mock.patch.object(background.ctypes, 'WinDLL', return_value=kernel, create=True), \
             mock.patch.object(background.ctypes, 'get_last_error', return_value=183, create=True), \
             mock.patch.object(background.subprocess, 'Popen') as spawn:
            self.assertEqual(background.main(), 0)
            spawn.assert_not_called()
            kernel.CloseHandle.assert_called_once_with(123)


if __name__ == '__main__':
    unittest.main()
