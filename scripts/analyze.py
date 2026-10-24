"""Verify source hashes and build tables and two plots from real measurements."""
import argparse
import csv
import hashlib
import json
import statistics as st
from collections import defaultdict
from pathlib import Path


def analyze(folder):
    manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
    groups = defaultdict(list)
    workers = []
    for rec in manifest['records']:
        path = folder / rec['file']
        if hashlib.sha256(path.read_bytes()).hexdigest() != rec['sha256']:
            raise ValueError(f'Raw measurement modified: {path}')
        d = json.loads(path.read_text())
        if d['backend'] != manifest['backend'] or not d['success'] or d['violations'] or d['errors']:
            raise ValueError(f'Failed or mixed-backend run: {path}')
        key = (rec['preset'], d['mode'], d['philosophers'], d['think_ms'])
        if (d['seed'], d['cycles']) != ((1000, 100) if rec['preset'] == 'standard' else (3619, 235)):
            raise ValueError(f'Incorrect preset: {path}')
        if d['completed'] != d['philosophers']*d['cycles'] or any(w['completed'] != d['cycles'] for w in d['workers']):
            raise ValueError(f'Incomplete run: {path}')
        groups[key].append((rec['repeat'], d))
        for w in d['workers']:
            workers.append({'file': rec['file'], **w})
    rows = []
    for (preset, mode, n, think), runs in sorted(groups.items()):
        runs.sort()
        if [r for r, _ in runs] != [1, 2, 3]: raise ValueError('Need exactly three repetitions')
        ds = [d for _, d in runs]
        times = [d['elapsed_ms'] for d in ds]
        row = dict(preset=preset, mode=mode, philosophers=n, think_ms=think,
                   run1_ms=times[0], run2_ms=times[1], run3_ms=times[2], mean_ms=st.mean(times),
                   std_ms=st.stdev(times), completed=ds[0]['completed'],
                   throughput_ops_s=st.mean(d['throughput_ops_s'] for d in ds),
                   mean_wait_ms=st.mean(d['mean_wait_ms'] for d in ds),
                   max_wait_ms=max(d['max_wait_ms'] for d in ds),
                   utilization=st.mean(d['resource_utilization'] for d in ds),
                   timeouts=sum(d['timeouts'] for d in ds), violations=sum(d['violations'] for d in ds))
        rows.append(row)
    if len(rows) != 16: raise ValueError('Suite requires 16 configurations')
    for name, values in [('summary.csv', rows), ('workers.csv', workers)]:
        with (folder / name).open('w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=list(values[0])); writer.writeheader(); writer.writerows(values)
    return manifest, rows


def plot(folder, manifest, rows):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    backend = manifest['backend']
    # Two independent characteristics, all mandatory configurations and comparison mode.
    for field, ylabel, filename in [('throughput_ops_s', 'Completed cycles per second', 'throughput'),
                                    ('mean_wait_ms', 'Mean wait for two forks, ms', 'waiting')]:
        fig, axes = plt.subplots(1, 2, figsize=(10.4, 4), sharey=True, layout='constrained')
        for ax, preset in zip(axes, ['standard', 'individual']):
            for mode, style in [('asymmetric', '-'), ('ordered', '--')]:
                for think, color in [(2, '#2166ac'), (15, '#b35806')]:
                    rs = sorted((r for r in rows if r['preset'] == preset and r['mode'] == mode and r['think_ms'] == think), key=lambda r: r['philosophers'])
                    ax.plot([r['philosophers'] for r in rs], [r[field] for r in rs], style,
                            marker='o', color=color, label=f'{mode}, think={think} ms')
            ax.set_title(preset + (' (100 cycles)' if preset == 'standard' else ' (235 cycles)'))
            ax.set_xlabel('Number of philosophers'); ax.set_xticks([5, 7]); ax.grid(alpha=.25)
        axes[0].set_ylabel(ylabel)
        axes[1].legend(fontsize=7)
        fig.suptitle(f'{backend} | means of 3 real runs', fontsize=11)
        fig.savefig(folder / f'{filename}.png', dpi=180)
        fig.savefig(folder / f'{filename}.svg')
        plt.close(fig)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--data', type=Path, required=True)
    a = p.parse_args(); m, r = analyze(a.data); plot(a.data, m, r)
    print(f'Validated {len(m["records"])} runs; wrote tables and plots to {a.data}')
