# Independent audit: isolated Izhikevich pulse, I=30 for 3 ms

An isolated neuron; a=0.02, b=0.2, c=-65, d=8, peak=30 mV; v(0)=-65, u(0)=-13.
The stimulus is exactly [100,103) ms. Both exact solvers split at 100 and 103 ms and locate upward threshold events before applying the reset.

This is one pulse after 100 ms without input, not the steady response to a periodic train. Its two-spike Euler 0.5 ms result cannot be generalized to later pulses, when the recovery variable still retains the preceding activity.
The [original NeuroNet comparison](../neuronet_comparison/README.md) covers the separate 100 ms periodic protocol. With unchanged RS parameters, zero noise and Spike showing=1, the original program gives one steady spike per period at Step=0.5 ms and two at Step=0.025 ms. Its displayed frequencies 20/40 use a 50 ms counting window; they are not the long-run rates over a 100 ms stimulus period.

DOP853 response latencies: 1.498652255 ms; 3.156522502 ms. Radau maximum event-time difference: 2.98e-12 ms.

| Case | h, ms | Response spikes | First latency, ms | Second latency, ms | Dose |
|---|---:|---:|---:|---:|---:|
| reference_DOP853 | — | 2 | 1.4986523 | 3.1565225 | 90 |
| reference_Radau | — | 2 | 1.4986523 | 3.1565225 | 90 |
| euler_1ms | 1 | 1 | 3 | — | 90 |
| euler_0.5ms | 0.5 | 2 | 2 | 6 | 90 |
| euler_0.25ms | 0.25 | 2 | 2 | 5.25 | 90 |
| euler_0.1ms | 0.1 | 2 | 1.7 | 3.7 | 90 |
| euler_0.025ms | 0.025 | 2 | 1.55 | 3.275 | 90 |
| historical_1ms | 1 | 2 | 2 | 7 | 90 |
| historical_0.5ms | 0.5 | 2 | 2 | 5.5 | 90 |
| historical_0.25ms | 0.25 | 2 | 1.75 | 4 | 90 |
| historical_0.1ms | 0.1 | 2 | 1.6 | 3.4 | 90 |
| historical_0.025ms | 0.025 | 2 | 1.525 | 3.225 | 90 |
| historical_old_u_1ms | 1 | 2 | 2 | 6 | 90 |
| historical_old_u_0.5ms | 0.5 | 2 | 2 | 5.5 | 90 |
| historical_old_u_0.25ms | 0.25 | 2 | 1.75 | 4 | 90 |
| historical_old_u_0.1ms | 0.1 | 2 | 1.6 | 3.4 | 90 |
| historical_old_u_0.025ms | 0.025 | 2 | 1.525 | 3.225 | 90 |
| euler_1ms_both_closed | 1 | 2 | 3 | 9 | 120 |
| euler_1ms_both_open | 1 | 1 | 4 | — | 60 |
| euler_1ms_right_closed | 1 | 1 | 4 | — | 90 |
| historical_1ms_both_closed | 1 | 2 | 2 | 5 | 120 |
| historical_1ms_both_open | 1 | 1 | 3 | — | 60 |
| historical_1ms_right_closed | 1 | 2 | 3 | 8 | 90 |
| hypothesis_hold_1ms_peak_frozen | — | 1 | 1.4986523 | — | 90 |
| hypothesis_hold_1ms_peak_recovery | — | 1 | 1.4986523 | — | 90 |
| hypothesis_hold_1ms_reset_frozen | — | 1 | 1.4986523 | — | 90 |
| hypothesis_hold_1ms_reset_recovery | — | 1 | 1.4986523 | — | 90 |
| cpp_50us | 0.05 | 2 | 1.5 | 3.2 | 90 |
| cpp_25us | 0.025 | 2 | 1.5 | 3.175 | 90 |
| cpp_12.5us | 0.0125 | 2 | 1.5 | 3.1625 | 90 |
| cpp_5us | 0.005 | 2 | 1.5 | 3.16 | 90 |
| cpp_2us | 0.002 | 2 | 1.5 | 3.16 | 90 |
| cpp_1us | 0.001 | 2 | 1.499 | 3.157 | 90 |
| cpp_equilibrium_overrides_manual | 0.025 | 2 | 1.5 | 3.175 | 90 |
| reference_Radau_onset0 | — | 2 | 1.3488175 | 3.0371826 | 90 |
| cpp_onset0 | 0.025 | 2 | 1.35 | 3.05 | 90 |
| reference_Radau_onset1000 | — | 2 | 1.4918767 | 3.1438751 | 90 |
| cpp_onset1000 | 0.025 | 2 | 1.5 | 3.175 | 90 |

The three-pool synapse and the other neuron were removed, so feedback cannot cause the second spike.
The JSON file exported by every CLI run is compared structurally against its input; initial v/u and equilibrium-u precedence are checked explicitly.
The C++ input dose is reconstructed from its exported configuration and midpoint/fmod source formula; it is not a separately measured current channel.
C++ checkpoint values use the nearest recorded sample; their exact time offsets are included in audit.json.

## Why the second spike occurs

Immediately before the first reset: v=30 mV and u=-13.7602862907; immediately after: v=-65 mV and u=-5.76028629072.
The input remains on for 1.5013477452 ms after that reset. Its initial dv/dt is 19.7602862907 mV/ms; without the input it would be -10.2397137093 mV/ms.
At exactly 103 ms, v=-2.82177403214 mV and u=-5.85546283619. Even after removing the current, dv/dt=132.065089023 mV/ms. The second spike follows at 103.156522502 ms.

## Interpretation boundaries

Euler, semi-implicit and the historical two-half-v-step scheme are numerically distinct. The historical scheme uses the updated v in the full u update. The old-u-order variant is included as an explicit hypothesis.
both_closed includes both endpoint samples (four samples at h=1 ms); both_open omits both (two samples); right_closed gives three samples shifted by one step. Their doses therefore differ from, or are shifted relative to, [100,103).
The hold rows are retained as rejected diagnostic hypotheses. They explicitly change the hybrid ODE by suppressing voltage evolution for 1 ms after a spike. Inspection of the original executable shows that v and u continue to be integrated while Spike showing/refractory_time gates event/output bookkeeping. These frozen-state hold models therefore do not describe that executable and do not explain its periodic result; see the [binary and experimental evidence](../neuronet_comparison/README.md).
The 2021 NeuroNet article reports Euler integration with a 0.5 ms step. In this isolated-pulse audit Euler 0.5 ms produces two spikes. A periodic train has a different state at pulse onset: u has not fully recovered. Coarse Euler then registers and resets the first spike later, leaving less of the 3 ms input after reset; the state reached at pulse offset can relax instead of producing a second spike. The original program's one-spike steady train at 0.5 ms and two-spike train at 0.025 ms are consistent with this numerical and recovery-state dependence. Neither the isolated-pulse table nor the one-spike Euler 1 ms row establishes a need for an extra post-spike hold.

The source audit finds correct millisecond-to-second conversion, RS coefficients, upward threshold, u+=d/v=c reset, strict JSON model selection, and a 3 ms midpoint-sampled stimulus at every audited C++ step. The C++ reset is located at step end, so spike times have a first-order event timing error despite the RK4 subthreshold solver.
The existing pulse-count tests alone are not independent model validation. The adaptive-solver and parameter sensitivity results here validate the isolated-pulse hybrid ODE. They do not establish numerical equivalence to NeuroNet's coarse Euler periodic simulation; that comparison is documented separately.

References: [Izhikevich original model and code](https://www.izhikevich.org/publications/spikes.htm); [Lobov et al., 2021](https://pmc.ncbi.nlm.nih.gov/articles/PMC8070389/).

Generated by `audit_numerics.py`; see `audit.json` for hashes, exact reset states, parameter sweeps, and software versions.
