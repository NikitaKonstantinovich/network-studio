"""Exact event-driven reference for the manual's three-pool TM synapse.

No neuron solver or C++ recurrence is used. Between point events, the linear
ODE is solved in closed form; at an event u is incremented before release.
Times and time constants are in seconds. x+y+z=1; the original NeuroNet pool
total 20 corresponds to gain=20 here. Output peaks are measured from zero,
whereas jumps exclude the remaining tail of preceding responses.

tau_fac=0 is an explicit mathematical no-facilitation limit: u decays to zero
over any positive interval, so each event uses U. The studio currently rejects
this parameter, and this reference does not imply that it supports that limit.
Run this module for analytic self-checks and the ideal regular-event table.
"""
from __future__ import annotations

import bisect
import math


def _finite(value, name):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{name} must be finite")
    return value


def _parameters(U, tau_fac, tau_rec, tau_syn, gain):
    values = tuple(_finite(v, k) for v, k in zip(
        (U, tau_fac, tau_rec, tau_syn, gain),
        ("U", "tau_fac", "tau_rec", "tau_syn", "gain")))
    U, tau_fac, tau_rec, tau_syn, gain = values
    if not 0 <= U <= 1 or tau_fac < 0 or tau_rec <= 0 or tau_syn <= 0 or gain < 0:
        raise ValueError("Require 0<=U<=1, tau_fac>=0, tau_rec>0, tau_syn>0, gain>=0")
    return values


def _state(u, x, y):
    u, x, y = (_finite(v, k) for v, k in zip((u, x, y), ("u", "x", "y")))
    tol = 2e-12
    if not -tol <= u <= 1+tol or x < -tol or y < -tol or x+y > 1+tol:
        raise ValueError("State outside 0<=u<=1, x>=0, y>=0, x+y<=1")
    return u, x, y


def _times(values, name, strict):
    result = [_finite(v, name) for v in values]
    if any(t < 0 for t in result) or any(
            b <= a if strict else b < a for a, b in zip(result, result[1:])):
        raise ValueError(f"{name} must be nonnegative and {'strictly increasing' if strict else 'sorted'}")
    return result


def _recovery_kernel(dt, tau_rec, tau_syn):
    """Coefficient multiplying initial active pool in x(t)."""
    ed, ei = math.exp(-dt/tau_rec), math.exp(-dt/tau_syn)
    if tau_rec == tau_syn:
        return ed, ei, dt/tau_rec*ed
    # expm1 avoids cancellation when the two time constants nearly coincide.
    q = dt*(tau_rec-tau_syn)/(tau_syn*tau_rec)
    difference = -ed*math.expm1(-q) if abs(q) < .5 else ed-ei
    return ed, ei, tau_syn/(tau_rec-tau_syn)*difference


def evolve(u, x, y, dt, *, tau_fac=1., tau_rec=.05, tau_syn=.01):
    """Return exact (u,x,y) after dt without events, including tau_rec=tau_syn."""
    _, tau_fac, tau_rec, tau_syn, _ = _parameters(.5, tau_fac, tau_rec, tau_syn, 20.)
    u, x, y = _state(u, x, y)
    dt = _finite(dt, "dt")
    if dt < 0:
        raise ValueError("dt must be nonnegative")
    if dt == 0:
        return u, x, y
    ed, ei, kernel = _recovery_kernel(dt, tau_rec, tau_syn)
    # Integrate y'= -y/tau_syn and x'= (1-x-y)/tau_rec.
    result = (u*math.exp(-dt/tau_fac) if tau_fac else 0.,
              1+(x-1)*ed-y*kernel, y*ei)
    return _state(*result)


def tm_responses(times_s, *, U=.5, tau_fac=1., tau_rec=.05, tau_syn=.01,
                 gain=20., initial=(0., 1., 0.)):
    """Return post-event state, peak from zero and jump for every input event.

    Each row has time_s,u,x,y,z,release,peak,jump,pre_u,pre_x,pre_y.
    Events must be strictly increasing; simultaneous repeated releases are
    deliberately not inferred from a single presynaptic spike.
    """
    U, tau_fac, tau_rec, tau_syn, gain = _parameters(U, tau_fac, tau_rec, tau_syn, gain)
    state = _state(*initial)
    times = _times(times_s, "event times", True)
    previous, rows = 0., []
    for time in times:
        pre_u, pre_x, pre_y = evolve(*state, time-previous,
                                    tau_fac=tau_fac, tau_rec=tau_rec, tau_syn=tau_syn)
        u = pre_u+U*(1-pre_u)
        release = u*pre_x
        state = _state(u, pre_x-release, pre_y+release)
        u, x, y = state
        rows.append(dict(time_s=time, u=u, x=x, y=y, z=1-x-y, release=release,
                         peak=gain*y, jump=gain*release,
                         pre_u=pre_u, pre_x=pre_x, pre_y=pre_y))
        previous = time
    return rows


def euler_tm_responses(times_s, dt=.0005, *, U=.5, tau_fac=1., tau_rec=.05,
                       tau_syn=.01, gain=20., initial=(0., 1., 0.)):
    """Diagnostic explicit-Euler TM on grid-aligned events; NOT a NeuroNet run.

    Each continuous step uses OLD x,y,z,u for all four derivatives, followed
    by the same u-before-release event jump as tm_responses. The inter-event
    linear Euler map is raised to an integer power in closed form, avoiding
    millions of Python iterations without replacing Euler by an exact ODE.
    Results share the tm_responses row schema. Event timestamps use the caller's
    grid convention; initial state is at time zero before any event there.

    Binary evidence is documented by ../lab1/neuronet_probe.py and its
    results/neuronet_comparison/binary_evidence.json. This helper implements
    only synaptic updates and does not infer membrane thresholds, output
    timer gating, stimulus quantization or original GUI history.
    """
    U, tau_fac, tau_rec, tau_syn, gain = _parameters(U, tau_fac, tau_rec, tau_syn, gain)
    dt = _finite(dt, "dt")
    if dt <= 0 or dt > min(tau_rec, tau_syn, tau_fac if tau_fac else math.inf):
        raise ValueError("Require 0<dt<=all positive time constants for physical Euler decay")
    times = _times(times_s, "event times", True)
    ticks = [round(t/dt) for t in times]
    if any(abs(t-k*dt) > 1e-10*max(1., abs(t)) for t, k in zip(times, ticks)):
        raise ValueError("Euler event times must be multiples of dt")
    if any(b <= a for a, b in zip(ticks, ticks[1:])):
        raise ValueError("Euler events must occupy distinct grid ticks")
    state, previous, rows = _state(*initial), 0, []
    decay_d, decay_i = 1-dt/tau_rec, 1-dt/tau_syn
    decay_f = 1-dt/tau_fac if tau_fac else 0.
    for time, tick in zip(times, ticks):
        n = tick-previous
        u, x, y = state
        if n:
            ed, ei = decay_d**n, decay_i**n
            if tau_rec == tau_syn:
                kernel = n*(dt/tau_rec)*decay_d**(n-1)
            else:
                kernel = tau_syn/(tau_rec-tau_syn)*(ed-ei)
            u, x, y = _state(u*decay_f**n, 1+(x-1)*ed-y*kernel, y*ei)
        pre_u, pre_x, pre_y = u, x, y
        u += U*(1-u)
        release = u*x
        state = _state(u, x-release, y+release)
        u, x, y = state
        rows.append(dict(time_s=time, u=u, x=x, y=y, z=1-x-y, release=release,
                         peak=gain*y, jump=gain*release,
                         pre_u=pre_u, pre_x=pre_x, pre_y=pre_y))
        previous = tick
    return rows


def state_at(times_s, sample_times_s, *, U=.5, tau_fac=1., tau_rec=.05,
             tau_syn=.01, gain=20., initial=(0., 1., 0.)):
    """Exact state at sorted samples; a sample at an event is AFTER its jump."""
    _parameters(U, tau_fac, tau_rec, tau_syn, gain)
    times = _times(times_s, "event times", True)
    samples = _times(sample_times_s, "sample times", False)
    responses = tm_responses(times, U=U, tau_fac=tau_fac, tau_rec=tau_rec,
                             tau_syn=tau_syn, gain=gain, initial=initial)
    result = []
    for time in samples:
        position = bisect.bisect_right(times, time)-1
        if position < 0:
            anchor, state = 0., initial
        else:
            row = responses[position]
            anchor, state = row["time_s"], (row["u"], row["x"], row["y"])
        u, x, y = evolve(*state, time-anchor, tau_fac=tau_fac, tau_rec=tau_rec, tau_syn=tau_syn)
        result.append(dict(time_s=time, u=u, x=x, y=y, z=1-x-y, output=gain*y))
    return result


def regular_response(frequency_hz, *, U=.5, tau_fac=1., tau_rec=.05,
                     tau_syn=.01, gain=20.):
    """Closed-form steady response to ONE spike per period, without a neuron.

    Solves the fixed point of the exact event map, rather than integrating or
    waiting an arbitrarily selected number of cycles for a transient to settle.
    """
    U, tau_fac, tau_rec, tau_syn, gain = _parameters(U, tau_fac, tau_rec, tau_syn, gain)
    frequency_hz = _finite(frequency_hz, "frequency_hz")
    if frequency_hz <= 0:
        raise ValueError("frequency_hz must be positive")
    period = 1/frequency_hz
    ed, ei, kernel = _recovery_kernel(period, tau_rec, tau_syn)
    one_d, one_i = -math.expm1(-period/tau_rec), -math.expm1(-period/tau_syn)
    ef = math.exp(-period/tau_fac) if tau_fac else 0.
    u = U/(1-(1-U)*ef) if U else 0.
    resource_cost = ed/one_d + kernel/(one_d*one_i)
    release = u/(1+u*resource_cost)
    pre_x = 1/(1+u*resource_cost)
    x, y = pre_x-release, release/one_i
    return dict(frequency_hz=frequency_hz, period_s=period, u=u, x=x, y=y, z=1-x-y,
                pre_x=pre_x, release=release, peak=gain*y, jump=gain*release)


def self_test():
    def close(a, b):
        assert math.isclose(a, b, rel_tol=1e-11, abs_tol=1e-12), (a, b)
    assert tm_responses([6.])[0]["peak"] == 10.
    pair = tm_responses([6., 6.004])
    close(pair[1]["peak"], 14.295718200207)
    close(pair[1]["jump"], 7.592517739850)
    euler = euler_tm_responses([0., .0005, .004])
    close(euler[0]["peak"], 10.)
    close(euler[1]["pre_y"], .475)
    close(euler[1]["pre_x"], .5)
    close(euler[1]["u"], .749875)
    # Independent literal old-pool Euler loop checks the powered Euler map.
    u, x, y, z, previous = 0., 1., 0., 0., 0
    for tick, expected in zip([0, 1, 8], euler):
        for _ in range(tick-previous):
            u, x, y, z = u-.0005*u, x+.0005*z/.05, y-.0005*y/.01, z+.0005*(y/.01-z/.05)
        u += .5*(1-u); release = u*x; x -= release; y += release
        for value, key in [(u, "u"), (x, "x"), (y, "y"), (z, "z")]: close(value, expected[key])
        previous = tick
    # In the no-facilitation limit, utilization equals U at every event.
    assert all(row["u"] == .5 for row in tm_responses([0., .01, .1], tau_fac=0))
    # Equal time constants have a finite, distinct limiting solution.
    u, x, y = evolve(.2, .3, .4, .01, tau_rec=.01, tau_syn=.01)
    close(x, 1-1.1/math.e); close(y, .4/math.e)
    semigroup_a = evolve(*evolve(.2, .3, .4, .017), .043)
    semigroup_b = evolve(.2, .3, .4, .060)
    for a, b in zip(semigroup_a, semigroup_b): close(a, b)
    for frequency in [.1, .5, 1, 5, 10, 25, 50, 100]:
        expected = regular_response(frequency)
        settled = tm_responses([k/frequency for k in range(1000)])[-1]
        for name in ["u", "x", "y", "peak", "jump"]: close(settled[name], expected[name])
    at = state_at([1.], [0., 1., 1.01])
    close(at[0]["output"], 0); close(at[1]["output"], 10); close(at[2]["output"], 10/math.e)
    for invalid in [[-.1], [1., 1.], [2., 1.], [float("nan")]]:
        try: tm_responses(invalid)
        except ValueError: pass
        else: raise AssertionError("Invalid event sequence accepted")
    return "PASS TM reference: exact isolated/doublet, Euler old-pool map, no-facilitation limit, equal taus, semigroup, steady fixed points, sample convention, invalid events"


if __name__ == "__main__":
    print(self_test())
    print("frequency_hz,peak_from_zero,jump_above_pre_event")
    for frequency in [.1, .5, 1, 5, 10, 25, 50, 100]:
        row = regular_response(frequency)
        print(f"{frequency:g},{row['peak']:.12g},{row['jump']:.12g}")
