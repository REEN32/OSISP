"""Black-box acceptance tests; runs against either backend, checks real JSON."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())

class ModelTests(unittest.TestCase):
    def invoke(self, *args, expected=0):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'result.json'
            r = subprocess.run([EXE, '--quiet', '--json', str(path), *map(str, args)],
                               capture_output=True, text=True, timeout=12)
            self.assertEqual(r.returncode, expected, r.stdout + r.stderr)
            return json.loads(path.read_text()) if path.exists() else r

    def assert_clean(self, d):
        self.assertTrue(d['success'])
        self.assertFalse(d['deadline'])
        self.assertEqual(d['violations'], 0)
        self.assertEqual(d['errors'], 0)
        self.assertEqual(d['completed'], d['philosophers'] * d['cycles'])
        self.assertEqual(len({s['tid'] for s in d['workers']}), d['philosophers'])
        self.assertTrue(all(w['completed'] == d['cycles'] for w in d['workers']))
        self.assertGreater(d['elapsed_ms'], 0)
        self.assertGreaterEqual(d['resource_utilization'], 0)
        self.assertLessEqual(d['resource_utilization'], 1.01)

    def test_model_sizes_and_modes(self):
        for n in [2, 3, 5, 7, 8, 16, 32]:
            for mode in ['asymmetric', 'ordered']:
                with self.subTest(n=n, mode=mode):
                    self.assert_clean(self.invoke('--philosophers', n, '--mode', mode,
                        '--cycles', 20, '--think-ms', 0, '--eat-ms', 0, '--jitter-ms', 0))

    def test_contention_timeouts_release_first_fork(self):
        d = self.invoke('--philosophers', 7, '--cycles', 25, '--think-ms', 0,
                        '--eat-ms', 3, '--jitter-ms', 0, '--timeout-ms', 1)
        self.assert_clean(d)
        self.assertGreater(d['timeouts'], 0)

    def test_deadline_and_cooperative_release(self):
        d = self.invoke('--philosophers', 7, '--cycles', 100000,
                        '--think-ms', 0, '--eat-ms', 5000,
                        '--duration-ms', 20, expected=2)
        self.assertTrue(d['deadline'])
        self.assertFalse(d['success'])
        self.assertEqual(d['violations'], 0)
        self.assertEqual(d['errors'], 0)
        self.assertLess(d['elapsed_ms'], 2000)

    def test_individual_preset_and_override(self):
        d = self.invoke('--individual', '--think-ms', 0, '--eat-ms', 0, '--jitter-ms', 0)
        self.assert_clean(d)
        self.assertEqual((d['seed'], d['cycles']), (3619, 235))
        d = self.invoke('--cycles', 3, '--individual', '--think-ms', 0, '--eat-ms', 0)
        self.assertEqual(d['cycles'], 3)

    def test_invalid_parameters(self):
        for args in [('--philosophers', '1'), ('--cycles', '-2'), ('--think-ms', '2x'),
                     ('--seed', '4294967296'), ('--mode', 'unsafe'), ('--bad', '1'),
                     ('--timeout-ms', '0'), ('--cycles',)]:
            with self.subTest(args=args): self.invoke(*args, expected=1)

    def test_log_has_identifiers_and_states(self):
        r = subprocess.run([EXE, '--cycles', '2', '--think-ms', '0', '--eat-ms', '0',
                            '--jitter-ms', '0'], capture_output=True, text=True, timeout=5)
        self.assertEqual(r.returncode, 0)
        for token in ['THINKING', 'WAITING', 'ACQUIRED', 'EATING', 'RELEASED', 'FINISHED',
                      'TID=', 'RESOURCE=', 'VIOLATIONS=0']:
            self.assertIn(token, r.stdout)

if __name__ == '__main__': unittest.main(verbosity=2)
