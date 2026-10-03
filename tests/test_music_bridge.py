import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import threading
import unittest
from unittest import mock
from urllib.error import HTTPError
from urllib.request import urlopen


MODULE_PATH = Path(__file__).resolve().parents[1] / 'scripts/music_bridge.py'
MODULE_SPEC = importlib.util.spec_from_file_location('music_bridge', MODULE_PATH)
music = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(music)


class MusicTests(unittest.TestCase):
    def setUp(self):
        self.config = {'bind': '127.0.0.1', 'port': 0, 'token': 'a' * 32,
                       'api_key': 'test-key', 'secret_key': 'test-secret', 'ffmpeg': 'ffmpeg'}

    def test_signature_matches_official_contract(self):
        headers = music.signed_headers('test-key', 'test-secret', 123456)
        expected = hmac.new(b'test-secret', b'key=test-key&timestamp=123456', hashlib.sha256).hexdigest()
        self.assertEqual(headers, {'X-Api-Key': 'test-key', 'X-Api-Timestamp': '123456', 'X-Api-Sign': expected})

    def test_track_prefers_direct_musicurl(self):
        track = music.parse_track({'code': 200, 'data': {'name': '晴天', 'songname': '歌手',
                                  'musicurl': 'https://m801.music.126.net/song.mp3',
                                  'url': 'https://music.163.com/song/media/outer/url?id=1.mp3'}})
        self.assertEqual(track['title'], '晴天')
        self.assertEqual(track['media'], 'https://m801.music.126.net/song.mp3')

    def test_rejects_untrusted_urls_and_provider_errors(self):
        for url in ('file:///music.mp3', 'https://127.0.0.1/audio', 'https://music.126.net.evil.test/audio',
                    'https://user:secret@music.163.com/audio', 'https://music.163.com:8080/audio'):
            with self.subTest(url=url), self.assertRaises(music.MusicError):
                music.media_url(url)
        for body in ({'code': 403}, {'code': 200, 'data': []}, {'code': 200, 'data': {}}):
            with self.assertRaises(music.MusicError):
                music.parse_track(body)

    def test_lookup_is_cached_and_signed(self):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps(
            {'code': 200, 'data': {'musicurl': 'https://music.163.com/test.mp3'}}).encode()
        provider = music.Provider(self.config)
        with mock.patch.object(music, 'urlopen', return_value=response) as fetch:
            self.assertEqual(provider.resolve('song', 'artist'), provider.resolve('song', 'artist'))
            self.assertEqual(fetch.call_count, 1)
            self.assertIn('X-api-sign', fetch.call_args.args[0].headers)

    def test_http_auth_search_limits_and_pcm_metadata(self):
        with music.MusicServer(self.config) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f'http://127.0.0.1:{server.server_port}'
            try:
                with mock.patch.object(server.provider, 'resolve', return_value={'title': 'song', 'artist': '', 'media': ''}) as resolve:
                    for path, status in (('/wrong/music/resolve?song=test', 404),
                                         ('/' + 'a' * 32 + '/music/resolve?song=', 400),
                                         ('/' + 'a' * 32 + '/music/resolve?song=test&song=other', 400),
                                         ('/' + 'a' * 32 + '/music/resolve?song=' + 'b' * 121, 400)):
                        with self.assertRaises(HTTPError) as failure:
                            urlopen(base + path, timeout=3)
                        self.assertEqual(failure.exception.code, status)
                    resolve.assert_not_called()
                    with urlopen(base + '/' + 'a' * 32 + '/music/resolve?song=test', timeout=3) as response:
                        result = json.load(response)
                    self.assertEqual(result['sampleRate'], 16000)
                    self.assertEqual(result['channels'], 1)
                    self.assertNotIn('url', result)
            finally:
                server.shutdown()
                thread.join(timeout=3)

    def test_no_audio_returns_error_and_releases_stream_slot(self):
        handler = object.__new__(music.MusicHandler)
        handler.server = mock.Mock(config=self.config, stream_slots=threading.BoundedSemaphore(1))
        process = mock.Mock()
        process.stdout.read.return_value = b''
        process.poll.return_value = 0
        with mock.patch.object(music.subprocess, 'Popen', return_value=process), self.assertRaises(music.MusicError):
            handler.stream({'media': 'https://music.163.com/test.mp3'})
        process.stdout.close.assert_called_once()
        self.assertTrue(handler.server.stream_slots.acquire(blocking=False))

    def test_pcm_stream_allows_pause_and_cleans_up(self):
        handler = object.__new__(music.MusicHandler)
        handler.server = mock.Mock(config=self.config, stream_slots=threading.BoundedSemaphore(1))
        handler.connection = mock.Mock()
        handler.wfile = mock.Mock()
        handler.send_response = mock.Mock()
        handler.send_header = mock.Mock()
        handler.end_headers = mock.Mock()
        process = mock.Mock()
        process.stdout.read.side_effect = [b'\x01\x00' * 2048, b'\x02\x00' * 2048, b'']
        process.poll.return_value = 0
        with mock.patch.object(music.subprocess, 'Popen', return_value=process) as spawn:
            handler.stream({'media': 'https://music.163.com/test.mp3'})
        handler.connection.settimeout.assert_called_once_with(1800)
        handler.send_response.assert_called_once_with(200)
        handler.send_header.assert_any_call('Content-Type', 'audio/pcm')
        self.assertEqual(handler.wfile.write.call_count, 2)
        self.assertIn('s16le', spawn.call_args.args[0])
        process.stdout.close.assert_called_once()
        self.assertTrue(handler.server.stream_slots.acquire(blocking=False))


if __name__ == '__main__':
    unittest.main()
