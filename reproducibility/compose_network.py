"""Assemble the network page only from completed original-CLI simulations."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
AUDIT = HERE.parent / 'verification/repro_labs_20261012/lab3'


def num(value, digits=3):
    return f'{value:.{digits}f}'.replace('.', ',')


def main():
    network = json.loads((AUDIT / 'network_findings.json').read_text(encoding='utf-8'))
    runs = sorted(network['runs'], key=lambda r: r['frequency_hz'])
    assert [r['frequency_hz'] for r in runs] == [.1, 1., 100.]
    assert all(r['duration_s'] == 30. and r['analysis_window_s'] == [10., 30.] for r in runs)
    sensitivity = json.loads((AUDIT / 'network_step_sensitivity.json').read_text(encoding='utf-8'))
    p = lambda text: {'kind': 'paragraph', 'text': text}
    small = lambda text: {'kind': 'small', 'text': text}
    table = lambda headers, rows, widths: {'kind': 'table', 'headers': headers, 'rows': rows, 'widths': widths}
    rows = []
    for r in runs:
        peak = r['neuron453']['stimulus_period_output_peak_mean']
        rows.append([num(r['frequency_hz'], 1), str(r['active_neuron_count']),
                     num(r['mean_population_rate_hz'], 4),
                     num(r['neuron453']['spike_rate_hz'], 1), num(peak)])
    a, b, c = runs
    spatial = ' / '.join(num(100*r['active_convex_hull_fraction'], 2) for r in runs)
    responding = c['neuron453']['responding_complete_periods']
    complete = c['complete_period_recruited']['periods']
    blocks = [
        p('Перенесён исходный граф NET_files/STP: 500 нейронов (400 E + 100 I), 7625 связей, параметры, начальные состояния и пять мишеней стимулятора. Шум и STDP выключены. Для каждой частоты: 30 с, RK4 с шагом 25 мкс, независимый сброс. Транспортные буферы NeuroNet не воспроизводятся [4].'),
        table(['Стимулы, Гц', 'Активных из 500', 'Сеть, Гц/нейрон', '№453, спайков/с', 'Пик Output №453'], rows, [1, 1.15, 1.25, 1.25, 1.3]),
        small('Окно 10–30 с. Частота сети усреднена по всем 500 узлам. Пик — средний максимальный постспайковый Output относительно нуля за полные периоды с ответом; периоды без спайков исключены. '
              f'При 100 Гц отвечают {responding} из {complete} периодов. Для 0,1 Гц в окне только один полный период.'),
        small('Визуальные оценки пиков NeuroNet: ≈15 / 15–16 / 6,5–7 соответственно. Высокочастотная вершина согласуется с этим диапазоном; точного ряда внешних амплитуд нет.'),
        p(f'У NeuroNet при 0,1 Гц видна активность части сети, при 1 Гц — более широкие вспышки, при 100 Гц — около пяти активных нейронов. В нашем расчёте при 0,1 и 1 Гц вовлечено {a["active_neuron_count"]} и {b["active_neuron_count"]} узла: ожидаемое расширение при 1 Гц не воспроизведено. При 100 Гц активны {c["active_neuron_count"]} узлов, из них {c["active_nonstimulated_count"]} без прямой стимуляции.'),
        small(f'Выпуклая оболочка активных координат занимает {spatial}% оболочки всей сети соответственно. Это геометрический охват, включающий неактивные точки внутри контура, а не площадь возбуждённой ткани. В историческом опыте частоты менялись без сброса; его история состояний отличается от наших независимых запусков.'),
        {'kind': 'subheading', 'text': 'Чувствительность к шагу'},
        table(['Шаг, мкс', 'Спайков за первые 0,2 с', 'Активных нейронов'],
              [[num(r['dt_us'], 4).rstrip('0').rstrip(','), str(r['total_spikes']), str(r['active_neurons'])]
               for r in sensitivity['rows']], [1, 1.8, 1.3]),
        p('Массовое вовлечение 95,4–96% устойчиво, но индивидуальные события чувствительны к шагу. При 25 мкс относительно 1,5625 мкс число спайков выше на 2,33%; у 106 узлов меняется число событий, max сдвиг первого спайка 19,03 мс. **Полная 30-секундная траектория не объявляется сошедшейся.**'),
        small('Независимый внешний контроль: 21 исходный снимок NeuroNet на интервале 0–10 мс подтверждает нормировку ресурса x=20, обновление u перед высвобождением и первый Output=10 у №189. Межшаговое затухание согласуется с Euler 0,5 мс с остатком ≤3·10⁻⁶. Это не эталон всей частотной кривой или сетевой динамики.')
    ]
    (HERE / 'network_report_blocks.json').write_text(json.dumps(blocks, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print('Network page assembled from three completed original-CLI runs.')


if __name__ == '__main__':
    main()
