"""Scientific figures from the newly calculated audit data; no image editing."""
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parents[1]
sys.path.insert(0, str(WORKSPACE / 'tmp/python_deps'))
os.environ['MPLCONFIGDIR'] = str(HERE / 'qa/matplotlib')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    audit = HERE.parent / 'verification/repro_labs_20261012'
    data = json.loads((audit / 'lab3/task1_findings.json').read_text(encoding='utf-8'))
    calibrated = data['calibrated']
    frequencies = [row['frequency_hz'] for row in calibrated]
    baseline = [row for row in data['historical'] if '_fine' not in row['name'] and 'single_spike' not in row['name']]
    assert len(baseline) == len(frequencies) == 8
    baseline_by_frequency = {float(row['name'].removeprefix('historical_f').replace('p', '.')): row for row in baseline}
    assert set(baseline_by_frequency) == set(frequencies)
    raw_peaks = [baseline_by_frequency[f]['metrics']['peak_mean'] for f in frequencies]
    assert all(row['metrics']['spikes_per_period_min'] == row['metrics']['spikes_per_period_max'] == 1 for row in calibrated)
    output = HERE / 'figures'
    output.mkdir(exist_ok=True)
    with plt.rc_context({'font.family': 'DejaVu Sans', 'font.size': 9,
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'axes.labelcolor': '#182630', 'text.color': '#182630'}):
        fig, ax = plt.subplots(figsize=(7.2, 3.1), constrained_layout=True)
        ax.semilogx(frequencies, raw_peaks, 'o--', color='#C68136', lw=1.4, ms=4,
                    label='Наш расчёт: 30 × 3 мс')
        ax.semilogx(frequencies, [row['metrics']['peak_mean'] for row in calibrated], 'o-',
                    color='#137C79', lw=1.7, ms=4, label='Наш расчёт: один спайк на импульс')
        ax.semilogx(frequencies, [14.5, 14.5, 14.5, 17.5, 15.5, 10, 6.75, 6.75],
                    's', color='#6A7082', ms=4, label='NeuroNet: оценки по скриншотам')
        ax.errorbar([50, 100], [6.75, 6.75], yerr=[.25, .25], color='#6A7082', fmt='none', capsize=3)
        ax.axhline(10, color='#8C999D', linewidth=.7, alpha=.7)
        ax.set_xticks(frequencies, ['0,1', '0,5', '1', '5', '10', '25', '50', '100'])
        ax.set_xlabel('Частота стимулятора, Гц')
        ax.set_ylabel('Вершина выхода g·y, ед. модели')
        ax.set_ylim(0, 21)
        ax.grid(alpha=.2)
        ax.legend(loc='lower left', frameon=False, fontsize=8)
        fig.savefig(output / 'lab3_frequency.png', dpi=220)
        plt.close(fig)
    print('Frequency figure saved from fresh audit values.')


if __name__ == '__main__':
    main()
