#pragma once
#include <cmath>

#if defined(__CUDACC__)
#define NA_HD __host__ __device__
#else
#define NA_HD
#endif

// One astrocyte contacts both glutamatergic synapses onto a single HH neuron.
// This core has no CUDAynamics globals and is also usable from ordinary C++.
namespace neuro_astro {
enum Variable {
#define NA_VARIABLE(id, value) id,
#include "variables.inc"
#undef NA_VARIABLE
    VAR_COUNT
};
enum Parameter {
#define NA_PARAMETER(id, value) id,
#include "parameters.inc"
#undef NA_PARAMETER
    method, COUNT  // CUDAynamics appends the integration step at COUNT.
};
enum Method { ExplicitEuler, ExplicitMidpoint, ExplicitRungeKutta4 };
constexpr int ODE_COUNT = t;

static const char* const variable_names[] = {
#define NA_VARIABLE(id, value) #id,
#include "variables.inc"
#undef NA_VARIABLE
};
static const char* const parameter_names[] = {
#define NA_PARAMETER(id, value) #id,
#include "parameters.inc"
#undef NA_PARAMETER
    "method", "h"
};

template<class T> NA_HD inline void defaults(T* state, T* params) {
#define NA_VARIABLE(id, value) state[id] = T(value);
#include "variables.inc"
#undef NA_VARIABLE
#define NA_PARAMETER(id, value) params[id] = T(value);
#include "parameters.inc"
#undef NA_PARAMETER
    params[method] = T(ExplicitRungeKutta4);
    params[COUNT] = T(0.000025);  // 25 us = 0.025 ms
}

template<class T> NA_HD inline T sq(T value) { return value * value; }
template<class T> NA_HD inline T cube(T value) { return value * value * value; }
template<class T> NA_HD inline T hill2(T value, T half) {
    return sq(value) / (sq(value) + sq(half));
}
template<class T> NA_HD inline T hill4(T value, T half) {
    return sq(sq(value)) / (sq(sq(value)) + sq(sq(half)));
}

// Removable HH singularities at V=-40 and -55 mV; no clipping of states.
template<class T> NA_HD inline T vtrap(T value, T scale) {
    const T ratio = value / scale;
    if (fabs(ratio) < T(1e-4))
        return scale * (T(1) - ratio / T(2) + sq(ratio) / T(12));
    return value / expm1(ratio);
}

// Finite pulse trains. freq=0 disables the train; DC currents are independent.
template<class T> NA_HD inline T pulse(T time, const T* p, int neuron) {
    const int offset = neuron;
    const T frequency = p[freq1 + offset];
    if (frequency <= T(0) || time < p[start1 + offset] ||
        time >= p[stop1 + offset] || p[width1 + offset] <= T(0)) return T(0);
    const T phase = fmod(time - p[start1 + offset], T(1) / frequency);
    return phase < p[width1 + offset] ? p[amp1 + offset] : T(0);
}

template<class T> NA_HD inline T resting_release(const T* y, const T* p) {
    return p[U_base] + p[feedback] * (p[alpha] - p[U_base]) * y[GammaS];
}

template<class T> NA_HD inline void currents(const T* y, const T* p,
                                            T& synaptic, T& astrocytic) {
    synaptic = (p[g_syn1] * y[s1] + p[g_syn2] * y[s2]) * (p[E_syn] - y[vPost]);
    // A reduced slow inward conductance, not an explicit NMDA receptor model.
    astrocytic = p[feedback] * p[g_SIC] * y[GA] / (p[K_SIC] + y[GA]) *
                 (p[E_syn] - y[vPost]);
}

template<class T> NA_HD inline void hh_rhs(const T* y, T* dy, int base,
                                         T injected, const T* p) {
    const T voltage = y[base], m = y[base+1], gate_h = y[base+2], n = y[base+3];
    const T a_m = T(0.1) * vtrap(-voltage - T(40), T(10));
    const T b_m = T(4) * exp(-(voltage + T(65)) / T(18));
    const T a_h = T(0.07) * exp(-(voltage + T(65)) / T(20));
    const T b_h = T(1) / (T(1) + exp(-(voltage + T(35)) / T(10)));
    const T a_n = T(0.01) * vtrap(-voltage - T(55), T(10));
    const T b_n = T(0.125) * exp(-(voltage + T(65)) / T(80));
    const T ionic = p[g_Na] * cube(m) * gate_h * (voltage-p[E_Na]) +
                    p[g_K] * sq(sq(n)) * (voltage-p[E_K]) + p[g_L]*(voltage-p[E_L]);
    // Original HH rates are per millisecond; all model time is in seconds.
    dy[base] = T(1000) * (injected - ionic) / p[C_m];
    dy[base+1] = T(1000) * (a_m*(T(1)-m) - b_m*m);
    dy[base+2] = T(1000) * (a_h*(T(1)-gate_h) - b_h*gate_h);
    dy[base+3] = T(1000) * (a_n*(T(1)-n) - b_n*n);
}

template<class T> NA_HD inline void rhs(const T* y, const T* p, T input1,
                                      T input2, T* dy) {
    T synaptic, astrocytic;
    currents(y, p, synaptic, astrocytic);
    hh_rhs(y, dy, vPre1, p[I_pre1] + input1, p);
    hh_rhs(y, dy, vPre2, p[I_pre2] + input2, p);
    hh_rhs(y, dy, vPost, p[I_post] + synaptic + astrocytic, p);
    for (int base = u1; base <= u2; base += u2-u1) {
        dy[base] = -y[base] / p[tau_f];
        dy[base+1] = (T(1)-y[base+1]) / p[tau_d];
        dy[base+2] = -y[base+2] / p[tau_syn];
        dy[base+3] = -p[Omega_Y] * y[base+3];
    }
    const T glutamate = p[spill1]*y[Y1] + p[spill2]*y[Y2];
    dy[GammaA] = p[O_N]*glutamate*(T(1)-y[GammaA]) - p[Omega_N]*y[GammaA];
    const T ip3 = y[IP3], calcium = y[Ca];
    dy[IP3] = p[O_beta]*y[GammaA] +
        p[O_delta]*p[kappa_delta]/(p[kappa_delta]+ip3)*hill2(calcium,p[K_delta]) -
        p[O_3K]*hill4(calcium,p[K_D])*ip3/(ip3+p[K_3K]) - p[Omega_5P]*ip3;
    const T activation = ip3/(ip3+p[d1]) * calcium/(calcium+p[d5]);
    const T store_drive = p[C_T] - (T(1)+p[rho_A])*calcium;
    dy[Ca] = (p[Omega_C]*cube(activation)*cube(y[hA]) + p[Omega_L])*store_drive -
             p[O_P]*hill2(calcium,p[K_P]);
    const T q2 = p[d2]*(ip3+p[d1])/(ip3+p[d3]);
    dy[hA] = p[O_2]*(q2*(T(1)-y[hA]) - calcium*y[hA]);
    dy[xA] = (T(1)-y[xA])/p[tau_A];
    dy[GA] = -p[Omega_G]*y[GA];
    dy[GammaS] = p[O_G]*y[GA]*(T(1)-y[GammaS]) - p[Omega_S]*y[GammaS];
}

// RK continuous flow, followed by spike / calcium upward-crossing jumps.
// Events are placed at the step end: timing error <= dt. All jumps use the
// same pre-event receptor state so simultaneous presynaptic events are symmetric.
template<class T> NA_HD inline void step(const T* previous, T* next,
                                       const T* p, T dt) {
    T old[VAR_COUNT];
    for (int i=0; i<VAR_COUNT; ++i) old[i] = previous[i]; // supports in-place stepping
    T k1[ODE_COUNT], k2[ODE_COUNT], k3[ODE_COUNT], k4[ODE_COUNT], stage[ODE_COUNT];
    // Midpoint sampling holds external pulses constant over this step.
    const T time_mid = old[t] - old[tComp] + dt/T(2);
    const T input1 = pulse(time_mid, p, 0), input2 = pulse(time_mid, p, 1);
    rhs(old, p, input1, input2, k1);
    const int integrator = int(p[method]);
    if (integrator == ExplicitEuler) {
        for (int i=0; i<ODE_COUNT; ++i) next[i] = old[i] + dt*k1[i];
    } else if (integrator == ExplicitMidpoint) {
        for (int i=0; i<ODE_COUNT; ++i) stage[i] = old[i]+dt/T(2)*k1[i];
        rhs(stage, p, input1, input2, k2);
        for (int i=0; i<ODE_COUNT; ++i) next[i] = old[i]+dt*k2[i];
    } else {
        for (int i=0; i<ODE_COUNT; ++i) stage[i] = old[i]+dt/T(2)*k1[i];
        rhs(stage, p, input1, input2, k2);
        for (int i=0; i<ODE_COUNT; ++i) stage[i] = old[i]+dt/T(2)*k2[i];
        rhs(stage, p, input1, input2, k3);
        for (int i=0; i<ODE_COUNT; ++i) stage[i] = old[i]+dt*k3[i];
        rhs(stage, p, input1, input2, k4);
        for (int i=0; i<ODE_COUNT; ++i)
            next[i] = old[i] + dt/T(6)*(k1[i]+T(2)*k2[i]+T(2)*k3[i]+k4[i]);
    }
    const T U = resting_release(next, p);
    for (int neuron=0; neuron<2; ++neuron) {
        const int voltage = neuron == 0 ? vPre1 : vPre2;
        const int base = neuron == 0 ? u1 : u2;
        if (old[voltage] < p[V_spike] && next[voltage] >= p[V_spike]) {
            next[base] += U*(T(1)-next[base]);
            const T released = next[base]*next[base+1];
            next[base+1] -= released;
            next[base+2] += released;
            next[base+3] += p[Y_release]*released;
        }
    }
    if (old[Ca] < p[C_threshold] && next[Ca] >= p[C_threshold]) {
        const T released = p[U_A]*next[xA];
        next[xA] -= released;
        next[GA] += p[G_release]*released;
    }
    // Kahan clock prevents seconds of drift in long float GUI simulations.
    const T increment = dt-old[tComp];
    next[t] = old[t]+increment;
    next[tComp] = (next[t]-old[t])-increment;
    currents(next, p, next[Isyn], next[Iastro]);
}
} // namespace neuro_astro
#undef NA_HD
