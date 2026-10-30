"""実 loopback でのプロセス起動、signal 終了、異常終了と再生復旧の検証。"""
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'test/fixtures/bridge_real_session/selected_records.hgraw'


class ApiProcessTests(unittest.TestCase):
    def start(self, replay=True):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        source = ['--replay', str(FIXTURE)] if replay else ['--port', '/dev/hoshiguard-synthetic-missing-device']
        proc = subprocess.Popen([sys.executable, '-m', 'hoshiguard', *source,
                                 '--api', '--api-port', str(port)], cwd=ROOT,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        def cleanup():
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=2)
            proc.stderr.close()
        self.addCleanup(cleanup)
        base = f'http://127.0.0.1:{port}'
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                self.fail('API exited: ' + proc.stderr.read().decode())
            try:
                status = self.get(base + '/api/v1/status')
                if status['receiver'] in ('replay_complete', 'disconnected'):
                    return proc, base, status
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(.02)
        self.fail('API did not become ready')

    @staticmethod
    def get(url):
        with urllib.request.urlopen(url, timeout=.5) as response:
            return json.load(response)

    def test_sigterm_shutdown_and_replay_restart(self):
        first, base, status1 = self.start()
        before = self.get(base + '/api/v1/alerts/latest')
        self.assertEqual(before['data']['provenance']['input'], 'replay')
        first.terminate()
        self.assertEqual(first.wait(timeout=5), 0)
        second, base2, status2 = self.start()
        after = self.get(base2 + '/api/v1/alerts/latest')
        self.assertEqual(before['data'], after['data'])
        self.assertNotEqual(status1['process_id'], status2['process_id'])
        self.assertEqual(status2['data_mode'], 'historical')

    def test_abrupt_exit_restart_never_presents_previous_cache_as_live(self):
        first, base, _ = self.start()
        self.assertIsNotNone(self.get(base + '/api/v1/alerts/latest')['data'])
        first.kill()
        first.wait(timeout=5)
        second, base2, status = self.start(replay=False)
        self.assertEqual(status['receiver'], 'disconnected')
        self.assertEqual(status['data_mode'], 'historical')
        self.assertIsNone(self.get(base2 + '/api/v1/alerts/latest')['data'])
        second.terminate()
        self.assertEqual(second.wait(timeout=5), 0)


if __name__ == '__main__':
    unittest.main()
