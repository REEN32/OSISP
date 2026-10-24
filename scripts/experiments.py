"""Run 3 repetitions of every configuration, preserving each original JSON."""
import argparse
import datetime
import hashlib
import json
import platform
import subprocess
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe', required=True, type=Path)
    p.add_argument('--out', type=Path, default=Path('data/windows'))
    p.add_argument('--backend-required', choices=['windows-api', 'portable-validation'], default='windows-api')
    a = p.parse_args()
    exe = a.exe.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    if (a.out / 'manifest.json').exists():
        p.error('Output already contains a suite. Choose a new --out directory to preserve measurements.')
    records = []
    for preset, seed, cycles in [('standard', 1000, 100), ('individual', 3619, 235)]:
        for mode in ['asymmetric', 'ordered']:
            for n in [5, 7]:
                for think in [2, 15]:
                    for repeat in [1, 2, 3]:
                        name = f'{preset}_{mode}_n{n}_t{think}_r{repeat}'
                        path = a.out / f'{name}.json'
                        if path.exists(): p.error(f'Refusing to replace {path}; choose a new directory')
                        cmd = [str(exe), '--quiet', '--philosophers', str(n), '--cycles', str(cycles),
                               '--think-ms', str(think), '--eat-ms', '2', '--jitter-ms', '1',
                               '--seed', str(seed), '--mode', mode, '--duration-ms', '60000',
                               '--timeout-ms', '1000', '--json', str(path.resolve())]
                        print(name, flush=True)
                        result = subprocess.run(cmd, text=True, capture_output=True, timeout=70)
                        if result.returncode:
                            raise RuntimeError(result.stdout + result.stderr)
                        d = json.loads(path.read_text())
                        if d['backend'] != a.backend_required:
                            raise RuntimeError('Wrong backend: refusing to mislabel results')
                        if not d['success'] or d['violations'] or d['errors'] or d['deadline']:
                            raise RuntimeError(f'Invalid experiment {path}')
                        records.append({'file': path.name, 'preset': preset, 'repeat': repeat,
                                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'command': cmd})
    manifest = {'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'backend': a.backend_required, 'host': platform.platform(),
                'processor': platform.processor(), 'python': platform.python_version(),
                'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'records': records}
    (a.out / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
    print(f'Completed {len(records)} real runs in {a.out}')

if __name__ == '__main__': main()
