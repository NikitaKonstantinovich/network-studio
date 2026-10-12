"""Independent DOP853 hybrid reference for delayed Izhikevich networks.

Uses continuous ODEs from the earlier independent reference, terminal roots,
explicit input breakpoints, and direct sums of historical events for STDP.
No production integration/queue/trace implementation is imported.
"""
import heapq
import math
import sys
from pathlib import Path
import numpy as np
from scipy.integrate import solve_ivp

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'numerics'))
from reference import initial, input_intervals, rhs  # loaded via alias below by caller


def learning(edge, weight, pres, posts, time, npre, npost):
    if not edge['stdp'] or not edge['enabled']:
        return weight
    p = math.fsum(math.exp(-(time-t)/edge['tau_plus']) for t in pres)
    q = math.fsum(math.exp(-(time-t)/edge['tau_minus']) for t in posts)
    slow = math.fsum(math.exp(-(time-t)/edge['tau_slow']) for t in posts)
    mul = edge['stdp_rule'] != 'additive'
    plus = npost*edge['a_plus']*(edge['weight_max']-weight if mul else 1)*p
    if edge['stdp_rule'] == 'triplet':
        plus *= slow
    minus = npre*edge['a_minus']*(weight-edge['weight_min'] if mul else 1)*q
    return max(edge['weight_min'], min(edge['weight_max'], weight+plus-minus))


def solve(config, rtol=2e-12):
    c = config
    assert all(n['model'] == 'izhikevich' for n in c['neurons']) and not c['astrocytes']
    nn, ns = len(c['neurons']), len(c['synapses'])
    state = initial(c)
    weights = np.array([s['weight'] for s in c['synapses']], dtype=float)
    pres, posts = [[] for _ in range(ns)], [[] for _ in range(nn)]
    spans, boundaries = input_intervals(c)
    queue, events, deliveries, segments = [], [], [], []
    time = 0.
    functions = []
    for i, n in enumerate(c['neurons']):
        def crossing(t, y, i=i, peak=n['izh_peak']):
            return y[4*i]-peak
        crossing.terminal, crossing.direction = True, 1
        functions.append(crossing)
    for left, right in zip(boundaries[:-1], boundaries[1:]):
        mid = (left+right)/2
        current = np.array([n['current']+(n['amplitude'] if any(a <= mid < b for a, b in spans[i]) else 0)
                            for i, n in enumerate(c['neurons'])], dtype=float)
        while time < right:
            end = min(right, queue[0][0] if queue else right)
            sol = solve_ivp(lambda t, y: rhs(c, y, current, weights), (time, end), state,
                            method='DOP853', rtol=rtol, atol=rtol*.01, max_step=50e-6,
                            events=functions, dense_output=True)
            assert sol.success, sol.message
            at = float(sol.t[-1])
            assert at > time
            segments.append((time, at, sol.sol, weights.copy()))
            state = sol.y[:, -1].copy()
            fired = {i for i, times in enumerate(sol.t_events) if len(times)}
            if fired:
                for i, n in enumerate(c['neurons']):
                    if abs(state[4*i]-n['izh_peak']) < 1e-8:
                        fired.add(i)
            due = []
            while queue and queue[0][0] <= at:
                due.append(heapq.heappop(queue))
            for i in sorted(fired):
                n = c['neurons'][i]
                state[4*i] = n['izh_c']; state[4*i+1] += n['izh_d']
                events.append((at, i))
                for j, s in enumerate(c['synapses']):
                    if s['enabled'] and s['source'] == i:
                        item = (at+s['delay'], j, at)
                        if item[0] == at:
                            due.append(item)
                        else:
                            heapq.heappush(queue, item)
            arrivals = [0]*ns
            for arrival, j, emit in sorted(due):
                s = c['synapses'][j]; b = 4*(nn+j)
                release = 1.
                if s['model'] != 'exponential_current':
                    state[b] += s['release']*(1-state[b])
                    release = state[b]*state[b+1]; state[b+1] -= release
                state[b+2] += release
                if (s['reversal'] > -40 if s['model'] == 'conductance_stp' else s['stdp'] or s['weight'] > 0):
                    state[b+3] += c['parameters']['Y_release']*release
                arrivals[j] += 1; deliveries.append((arrival, j, emit))
            npre = []
            for j, s in enumerate(c['synapses']):
                count = arrivals[j] if s['stdp_timing'] == 'arrival' else int(s['source'] in fired and s['enabled'])
                npre.append(count)
                weights[j] = learning(s, weights[j], pres[j], posts[s['target']], at, count, int(s['target'] in fired))
            for j, count in enumerate(npre): pres[j].extend([at]*count)
            for i in fired: posts[i].append(at)
            time = at
    def sample(times):
        times = np.asarray(times)
        y = np.full((len(times), len(state)), np.nan)
        w = np.full((len(times), ns), np.nan)
        for left, right, dense, weight in segments:
            mask = (times >= left) & (times <= right)
            if mask.any():
                y[mask] = dense(times[mask]).T; w[mask] = weight
        y[times == c['duration']] = state; w[times == c['duration']] = weights
        assert np.isfinite(y).all() and np.isfinite(w).all()
        return y, w
    return dict(events=events, deliveries=deliveries, final=state, weights=weights, sample=sample)
