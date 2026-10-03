import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import threading
import time
import unittest
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / 'scripts/windows/windows_companion.py'
MODULE_SPEC = importlib.util.spec_from_file_location('windows_companion', MODULE_PATH)
companion = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(companion)
NOW = datetime(2026, 10, 3, 3, tzinfo=timezone.utc).timestamp()


def timestamp(epoch):
    return datetime.fromtimestamp(epoch, timezone.utc).isoformat()


def quota_payload(now=NOW):
    return {
        'status': 'ok',
        'limitId': 'codex',
        'source': 'codex-wham',
        'planType': 'plus',
        'capturedAt': timestamp(now),
        'observedAt': timestamp(now),
        'primaryUsedPercent': 11,
        'primaryRemainingPercent': 89,
        'primaryWindowMinutes': 300,
        'primaryResetsAt': timestamp(now + 3600),
        'secondaryUsedPercent': 2,
        'secondaryRemainingPercent': 98,
        'secondaryWindowMinutes': 10080,
        'secondaryResetsAt': timestamp(now + 86400),
    }


class QuotaConversionTests(unittest.TestCase):
    def setUp(self):
        self.payload = quota_payload()

    def snapshot(self):
        limits = companion.bridge_to_rate_limits(self.payload, now=NOW)
        return companion.build_snapshot(limits, now=NOW)

    def test_reference_bridge_schema_and_wire_contract(self):
        snapshot = self.snapshot()
        self.assertEqual(companion.public_snapshot(snapshot), {
            'five_hour_remaining_percent': 89,
            'five_hour_reset_in_seconds': 3600,
            'weekly_remaining_percent': 98,
            'weekly_reset_in_seconds': 86400,
        })
        encoded = companion.encode_payload(snapshot)
        self.assertLessEqual(len(encoded), companion.MAX_PAYLOAD_BYTES)
        self.assertNotIn('_source', json.loads(encoded))

    def test_windows_are_selected_by_duration_not_position(self):
        for suffix in ('UsedPercent', 'RemainingPercent', 'WindowMinutes', 'ResetsAt'):
            primary = self.payload['primary' + suffix]
            self.payload['primary' + suffix] = self.payload['secondary' + suffix]
            self.payload['secondary' + suffix] = primary
        self.assertEqual(self.snapshot()['five_hour_remaining_percent'], 89)
        self.assertEqual(self.snapshot()['weekly_remaining_percent'], 98)

    def test_remaining_percentage_can_supply_missing_used_value(self):
        del self.payload['primaryUsedPercent']
        self.assertEqual(self.snapshot()['five_hour_remaining_percent'], 89)

    def test_zero_and_full_allowance_are_not_missing(self):
        self.payload['primaryUsedPercent'] = 0
        self.payload['secondaryUsedPercent'] = 100
        snapshot = self.snapshot()
        self.assertEqual(snapshot['five_hour_remaining_percent'], 100)
        self.assertEqual(snapshot['weekly_remaining_percent'], 0)

    def test_reset_in_past_is_zero(self):
        self.payload['primaryResetsAt'] = timestamp(NOW - 1)
        self.assertEqual(self.snapshot()['five_hour_reset_in_seconds'], 0)

    def test_timezone_offsets_and_z_timestamps(self):
        self.payload['capturedAt'] = '2026-10-03T11:00:00+08:00'
        self.payload['primaryResetsAt'] = '2026-10-03T04:00:00Z'
        self.assertEqual(self.snapshot()['five_hour_reset_in_seconds'], 3600)

    def test_unavailable_and_unknown_status_are_rejected(self):
        for status in ('no_data', 'auth_required', 'request_failed', None):
            with self.subTest(status=status):
                self.payload['status'] = status
                with self.assertRaises(companion.CompanionError):
                    self.snapshot()

    def test_other_buckets_and_missing_identity_are_rejected(self):
        for limit_id in ('codex-spark', None, True):
            with self.subTest(limit_id=limit_id):
                self.payload['limitId'] = limit_id
                with self.assertRaises(companion.CompanionError):
                    self.snapshot()

    def test_old_capture_is_rejected_even_if_observation_is_new(self):
        self.payload['capturedAt'] = timestamp(NOW - 181)
        with self.assertRaisesRegex(companion.CompanionError, 'stale'):
            self.snapshot()

    def test_age_boundary_and_configured_max_age(self):
        self.payload['capturedAt'] = timestamp(NOW - 180)
        self.snapshot()
        self.payload['capturedAt'] = timestamp(NOW - 181)
        companion.bridge_to_rate_limits(self.payload, now=NOW, max_age=300)

    def test_future_capture_is_rejected(self):
        self.payload['capturedAt'] = timestamp(NOW + 61)
        with self.assertRaisesRegex(companion.CompanionError, 'clock-skewed'):
            self.snapshot()

    def test_missing_invalid_and_naive_capture_are_rejected(self):
        for captured_at in (None, 'invalid', '2026-10-03T03:00:00', NOW):
            with self.subTest(captured_at=captured_at):
                self.payload['capturedAt'] = captured_at
                with self.assertRaises(companion.CompanionError):
                    self.snapshot()

    def test_missing_window_is_not_fabricated(self):
        for key in list(self.payload):
            if key.startswith('secondary'):
                del self.payload[key]
        with self.assertRaisesRegex(companion.CompanionError, 'weekly'):
            self.snapshot()

    def test_invalid_window_values_are_rejected(self):
        baseline = copy.deepcopy(self.payload)
        invalid_values = {
            'primaryUsedPercent': [None, True, '11', -1, 101, float('nan'), float('inf'), 10 ** 400],
            'primaryWindowMinutes': [True, '300', 300.1, -1, float('nan'), 10 ** 400],
            'primaryResetsAt': [None, 'invalid', '2026-10-03T04:00:00', NOW],
        }
        for key, values in invalid_values.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.payload = copy.deepcopy(baseline)
                    self.payload.pop('primaryRemainingPercent')
                    self.payload[key] = value
                    with self.assertRaises(companion.CompanionError):
                        self.snapshot()


class SourceSelectionTests(unittest.TestCase):
    def test_default_source_is_bridge(self):
        options = companion.options_from_args(['--json-only'])
        self.assertEqual(options.bridge_url, companion.DEFAULT_BRIDGE_URL)
        self.assertEqual(options.bridge_max_age, 180)

    def test_bridge_error_does_not_launch_app_server(self):
        options = companion.Options()
        with mock.patch.object(companion, 'read_bridge_quota', side_effect=companion.CompanionError('offline')):
            with mock.patch.object(companion.subprocess, 'Popen') as launch:
                with self.assertRaisesRegex(companion.CompanionError, 'bundled PC bridge'):
                    companion.read_snapshot(options)
                launch.assert_not_called()

    def test_bridge_source_passes_configured_age(self):
        options = companion.Options()
        options.bridge_max_age = 240
        limits = companion.bridge_to_rate_limits(quota_payload(), now=NOW)
        with mock.patch.object(companion, 'read_bridge_quota', return_value=limits) as read:
            with mock.patch.object(companion.subprocess, 'Popen') as launch:
                companion.read_snapshot(options)
                read.assert_called_once_with(options.bridge_url, max_age=240)
                launch.assert_not_called()

    def test_obsolete_app_server_flags_are_rejected(self):
        for arguments in (['--no-bridge'], ['--codex-path', 'codex.cmd']):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    companion.options_from_args(['--json-only'] + arguments)

    def test_empty_url_and_invalid_max_age_fail_argument_validation(self):
        for arguments in (['--bridge-url', ''], ['--bridge-max-age', '0'], ['--bridge-max-age', '-1']):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    companion.options_from_args(['--json-only'] + arguments)


class BridgePackageTests(unittest.TestCase):
    def test_bundled_executable_matches_recorded_checksum(self):
        root = MODULE_PATH.parents[1] / 'bridge'
        record = json.loads((root / 'runtime.json').read_text(encoding='utf-8'))
        executable = root / record['executable']['path']
        self.assertEqual(executable.stat().st_size, record['executable']['bytes'])
        self.assertEqual(hashlib.sha256(executable.read_bytes()).hexdigest(), record['executable']['sha256'])

    def test_copied_source_files_match_recorded_checksums(self):
        root = MODULE_PATH.parents[1] / 'bridge'
        record = json.loads((root / 'runtime.json').read_text(encoding='utf-8'))
        for relative_path, expected in record['source_files'].items():
            with self.subTest(path=relative_path):
                content = (root / relative_path).read_bytes().replace(b'\r\n', b'\n')
                self.assertEqual(hashlib.sha256(content).hexdigest(), expected)


class BridgeHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(cls.status)
                self.end_headers()
                self.wfile.write(cls.body)

            def log_message(self, *args):
                pass

        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.url = f'http://127.0.0.1:{cls.server.server_port}/quota'
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        type(self).status = 200
        type(self).body = json.dumps(quota_payload(time.time())).encode()

    def test_local_http_works_even_with_invalid_proxy_configuration(self):
        with mock.patch.dict(os.environ, {'http_proxy': 'http://127.0.0.1:1', 'no_proxy': ''}):
            limits = companion.read_bridge_quota(self.url)
        self.assertEqual(companion.build_snapshot(limits)['weekly_remaining_percent'], 98)

    def test_non_json_and_non_object_responses_are_rejected(self):
        for body in (b'not JSON', b'[]', b'null', b'\xff'):
            with self.subTest(body=body):
                type(self).body = body
                with self.assertRaises(companion.CompanionError):
                    companion.read_bridge_quota(self.url)

    def test_oversized_response_is_rejected(self):
        type(self).body = b' ' * (64 * 1024 + 1)
        with self.assertRaisesRegex(companion.CompanionError, '64 KiB'):
            companion.read_bridge_quota(self.url)

    def test_http_error_is_reported_as_companion_error(self):
        type(self).status = 503
        with self.assertRaises(companion.CompanionError):
            companion.read_bridge_quota(self.url)


if __name__ == '__main__':
    unittest.main()
