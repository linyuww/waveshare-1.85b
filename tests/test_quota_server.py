import importlib.util
import json
from pathlib import Path
import threading
import unittest
from http.server import ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import urlopen, Request
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('quota_server', Path(__file__).parents[1] / 'scripts/quota_server.py')
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


def fixture():
    return dict(status='ok', limitId='codex', capturedAt='2026-10-08T08:00:00Z',
                primaryWindowMinutes=300, primaryUsedPercent=20,
                primaryResetsAt='2026-10-08T09:00:00Z', secondaryWindowMinutes=10080,
                secondaryRemainingPercent=65, secondaryResetsAt='2026-10-09T08:00:00Z')


class QuotaTests(unittest.TestCase):
    def setUp(self):
        self.now = server.timestamp('2026-10-08T08:00:30Z')

    def test_windows_are_selected_by_duration(self):
        value = fixture()
        for suffix in ('WindowMinutes', 'UsedPercent', 'RemainingPercent', 'ResetsAt'):
            primary, secondary = value.pop('primary' + suffix, None), value.pop('secondary' + suffix, None)
            if secondary is not None: value['primary' + suffix] = secondary
            if primary is not None: value['secondary' + suffix] = primary
        result = server.normalize(value, self.now)
        self.assertEqual(result['five_hour_remaining_percent'], 80)
        self.assertEqual(result['weekly_remaining_percent'], 65)
        self.assertEqual(result['five_hour_reset_in_seconds'], 3570)
        self.assertEqual(result['age_seconds'], 30)

    def test_rejects_stale_missing_invalid_and_auth_errors(self):
        invalid = [('status', 'auth_required'), ('limitId', 'other'), ('capturedAt', None),
                   ('primaryWindowMinutes', 10080), ('primaryUsedPercent', True),
                   ('primaryUsedPercent', float('nan')), ('secondaryRemainingPercent', 101),
                   ('primaryResetsAt', 'no-date')]
        for key, bad in invalid:
            with self.subTest(key=key, bad=bad):
                value = fixture(); value[key] = bad
                with self.assertRaises(ValueError): server.normalize(value, self.now)
        with self.assertRaises(ValueError): server.normalize(fixture(), self.now + 181)
        with self.assertRaises(ValueError): server.normalize(fixture(), self.now - 100)

    def test_http_serves_only_quota_and_does_not_fabricate_errors(self):
        http = ThreadingHTTPServer(('127.0.0.1', 0), server.handler('http://127.0.0.1:8786/quota'))
        thread = threading.Thread(target=http.serve_forever, daemon=True); thread.start()
        url = f'http://127.0.0.1:{http.server_port}'
        try:
            with patch.object(server, 'fetch', return_value=server.normalize(fixture(), self.now)):
                with urlopen(url + '/quota') as response:
                    self.assertEqual(json.load(response)['five_hour_remaining_percent'], 80)
            with patch.object(server, 'fetch', side_effect=ValueError('secret')):
                with self.assertRaises(HTTPError) as failure: urlopen(url + '/quota')
                self.assertEqual(failure.exception.code, 503)
                self.assertEqual(json.load(failure.exception), {'status': 'unavailable'})
            with self.assertRaises(HTTPError) as failure: urlopen(url + '/ready')
            self.assertEqual(failure.exception.code, 404)
            with self.assertRaises(HTTPError) as failure: urlopen(Request(url + '/quota', data=b'write'))
            self.assertEqual(failure.exception.code, 501)
        finally:
            http.shutdown(); http.server_close(); thread.join()


if __name__ == '__main__': unittest.main()
