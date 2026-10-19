#include "network.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace studio;

namespace {
size_t checks = 0;

void same(NoiseCache& cache, const Config& c, int neuron, double time,
          const char* label) {
    // The uncached public function remains an independent reference for the
    // cache. Compare bits, including signed zero, rather than a tolerance.
    const double expected = external_current(c, neuron, time);
    const double actual = cache.current(c, neuron, time);
    ++checks;
    if (std::memcmp(&actual, &expected, sizeof(double)) != 0) {
        std::ostringstream message;
        message << label << ": neuron=" << neuron << ", t="
                << std::setprecision(17) << time << ", expected=" << expected
                << ", cached=" << actual;
        throw std::runtime_error(message.str());
    }
}

Config fixture() {
    Config c;
    c.neurons.resize(3);
    for (auto& n : c.neurons) {
        n.model = NeuronModel::Izhikevich;
        n.current = 0;
        n.frequency = 0;
    }
    c.noise_sigma = .7;
    c.noise_interval = .001;
    c.seed = 4294967295u;
    return c;
}

void times_and_parameters() {
    auto c = fixture();
    NoiseCache cache;
    const double infinity = std::numeric_limits<double>::infinity();
    for (double interval : {.001, .000073, .00073}) {
        c.noise_interval = interval;
        for (double sigma : {.7, .45, 8., 1.23456789012345}) {
            c.noise_sigma = sigma;
            for (uint64_t bin : {0ull, 1ull, 17ull, 143ull, 1000000000000ull}) {
                const double edge = double(bin) * interval;
                for (double time : {std::nextafter(edge, infinity), edge,
                                    bin ? std::nextafter(edge, 0.) : 0.,
                                    edge + interval * .25,
                                    edge + interval * .75, edge}) {
                    for (int i : {0, 2, 1, 0})
                        same(cache, c, i, time, "absolute bin and backward time");
                }
            }
        }
    }

    const double time = .0004;
    c.noise_interval = .001;
    for (uint32_t seed : {0u, 1u, 42u, 4294967295u, 42u}) {
        c.seed = seed;
        same(cache, c, 0, time, "seed changed inside the bin");
    }
    for (double sigma : {0., .7, 8., 0., .45}) {
        c.noise_sigma = sigma;
        same(cache, c, 0, time, "global sigma changed inside the bin");
    }
    auto& n = c.neurons[0];
    n.individual_noise = true;
    for (double sigma : {0., .7, 8., 0., .45}) {
        n.noise_sigma = sigma;
        same(cache, c, 0, time, "individual sigma changed inside the bin");
    }
    for (bool individual : {false, true, false, true}) {
        n.individual_noise = individual;
        same(cache, c, 0, time, "individual override toggled inside the bin");
    }
    for (double interval : {.001, .002, .000073, .001}) {
        c.noise_interval = interval;
        same(cache, c, 0, time, "noise interval changed");
    }
}

void changing_non_noise_input() {
    auto c = fixture();
    NoiseCache cache;
    const double time = .0004;
    auto& n = c.neurons[0];
    same(cache, c, 0, time, "warm noise cache");
    for (double current : {3.125, -10., 0.}) {
        n.current = current;
        same(cache, c, 0, time, "DC changes inside one bin");
    }
    n.frequency = 100;
    n.start = .0002;
    n.stop = .01;
    n.width = .0004;
    for (double amplitude : {30., -3., 0., 7.3}) {
        n.amplitude = amplitude;
        same(cache, c, 0, time, "legacy pulse amplitude changes");
    }
    for (double t : {.0001, .0002, .0004, .0006, .0009, .0004})
        same(cache, c, 0, t, "pulse edges inside one noise bin");
    n.pulse_intervals = {{.0001, .0002}, {.0005, .0006}};
    for (double t : {.00025, .00035, .00045, .00075, .00085})
        same(cache, c, 0, t, "pulse table changes inside one noise bin");

    Stimulator source;
    source.id = 1;
    source.recipients = {0, 2};
    source.signal.start = 0;
    source.signal.stop = .01;
    source.signal.width = .001;
    source.signal.frequency = 100;
    source.signal.amplitude = 13.7;
    c.stimulators.push_back(source);
    c.next_stimulator_id = 2;
    same(cache, c, 0, time, "shared stimulator added inside one bin");
    same(cache, c, 1, time, "shared stimulator nonrecipient");
    c.stimulators[0].signal.amplitude = -2.25;
    same(cache, c, 0, time, "shared amplitude changes inside one bin");
    c.stimulators[0].recipients = {1};
    same(cache, c, 0, time, "shared recipient removed");
    same(cache, c, 1, time, "shared recipient added");
    c.stimulators[0].enabled = false;
    same(cache, c, 1, time, "shared stimulator disabled");
    c.stimulators.clear();
    same(cache, c, 0, time, "shared stimulator removed");
}

void resizing_and_new_instances() {
    auto c = fixture();
    NoiseCache cache;
    const double time = .0004;
    for (int i = 0; i < int(c.neurons.size()); ++i)
        same(cache, c, i, time, "initial neuron streams");
    c.neurons.resize(7, c.neurons[0]);
    for (int i = 6; i >= 0; --i)
        same(cache, c, i, time, "neuron count increased");
    c.neurons.resize(1);
    same(cache, c, 0, time, "neuron count decreased");
    c.neurons.resize(4, c.neurons[0]);
    c.neurons[3].individual_noise = true;
    c.neurons[3].noise_sigma = 2.75;
    for (int i = 0; i < int(c.neurons.size()); ++i)
        same(cache, c, i, time, "neuron count increased again");
    std::swap(c.neurons[0], c.neurons[3]);
    for (int i : {0, 3})
        same(cache, c, i, time, "neuron parameters reassigned at existing indices");

    // Derived cache state must be reconstructible after a load/reset and must
    // not retain callbacks or references into a moved Simulator's old address.
    NoiseCache fresh;
    NoiseCache moved = std::move(cache);
    for (int i = 0; i < int(c.neurons.size()); ++i) {
        same(fresh, c, i, time, "fresh cache at nonzero absolute time");
        same(moved, c, i, time, "moved cache at nonzero absolute time");
    }
    auto other = c;
    other.seed = 173;
    other.noise_sigma = 11;
    for (int i = 0; i < int(other.neurons.size()); ++i) {
        same(moved, other, i, time, "different configuration same dimensions");
        same(moved, c, i, time, "return to original configuration");
    }
}

void same_stdp(StdpDecayCache& cache, const Synapse& synapse,
               StdpState& direct, StdpState& cached, uint64_t pre,
               uint64_t post, double dt, double rate, const char* label) {
    update_stdp(synapse, direct, pre, post, dt, rate);
    update_stdp_cached(synapse, cached, pre, post, dt, rate, cache);
    const std::array<double, 4> a = {
        direct.weight, direct.pre_trace, direct.post_trace, direct.post_slow_trace};
    const std::array<double, 4> b = {
        cached.weight, cached.pre_trace, cached.post_trace, cached.post_slow_trace};
    ++checks;
    if (std::memcmp(a.data(), b.data(), sizeof(double) * a.size()) != 0) {
        std::ostringstream message;
        message << label << ": rule=" << int(synapse.stdp_rule)
                << ", dt=" << std::setprecision(17) << dt
                << ", pre=" << pre << ", post=" << post << ", rate=" << rate;
        throw std::runtime_error(message.str());
    }
}

void stdp_coefficients() {
    Synapse synapse;
    synapse.stdp = true;
    StdpDecayCache cache;
    const double infinity = std::numeric_limits<double>::infinity();
    const double neighboring_dt = std::nextafter(.000025, infinity);
    const double neighboring_tau = std::nextafter(.020, infinity);
    // One reusable cache intentionally sees repeated and alternating tuples.
    // Every call uses old nonzero traces, including simultaneous events.
    for (auto rule : {StdpRule::Multiplicative, StdpRule::Triplet,
                      StdpRule::Additive, StdpRule::Triplet,
                      StdpRule::Multiplicative}) {
        synapse.stdp_rule = rule;
        for (double tau : {.020, neighboring_tau, .007, .020}) {
            synapse.tau_plus = tau;
            for (double minus : {.020, .019, .020}) {
                synapse.tau_minus = minus;
                for (double slow : {.100, .071, .100}) {
                    synapse.tau_slow = slow;
                    for (double dt : {.000025, .000025, neighboring_dt,
                                      .000013, .000000001, .100, .000025}) {
                        for (double rate : {0., .3, 1.}) {
                            for (double weight : {0., .7, 2.}) {
                                StdpState direct{weight, .1, .7, .3};
                                auto cached = direct;
                                same_stdp(cache, synapse, direct, cached, 0, 0,
                                          dt, rate, "STDP trace decay without events");
                                same_stdp(cache, synapse, direct, cached, 2, 3,
                                          dt, rate, "STDP simultaneous counted events");
                                same_stdp(cache, synapse, direct, cached, 1, 0,
                                          dt, rate, "STDP presynaptic event");
                                same_stdp(cache, synapse, direct, cached, 0, 1,
                                          dt, rate, "STDP postsynaptic event");
                            }
                        }
                    }
                }
            }
        }
    }

    for (bool enabled : {true, false, true}) {
        synapse.enabled = enabled;
        for (bool learning : {true, false, true}) {
            synapse.stdp = learning;
            for (auto rule : {StdpRule::Triplet, StdpRule::Multiplicative,
                              StdpRule::Additive, StdpRule::Triplet}) {
                synapse.stdp_rule = rule;
                synapse.tau_plus = .011;
                synapse.tau_minus = .013;
                synapse.tau_slow = .017;
                synapse.a_plus = .71;
                synapse.a_minus = .93;
                synapse.weight_min = .2;
                synapse.weight_max = .8;
                for (double weight : {.2, .5, .8}) {
                    StdpState direct{weight, 37., 19., 23.};
                    auto cached = direct;
                    same_stdp(cache, synapse, direct, cached, 0, 5, .000025, 1,
                              "STDP enabled/rule transitions and upper bound");
                    same_stdp(cache, synapse, direct, cached, 7, 0, .000025, 1,
                              "STDP lower bound");
                    same_stdp(cache, synapse, direct, cached, 1, 1, .000025, 0,
                              "STDP frozen weight still updates traces");
                }
            }
        }
    }

    StdpState direct{.5, .1, .3, .7};
    auto cached = direct;
    StdpDecayCache fresh;
    auto moved = std::move(cache);
    same_stdp(fresh, synapse, direct, cached, 1, 0, .000073, .3,
              "STDP freshly reconstructed coefficients");
    same_stdp(moved, synapse, direct, cached, 0, 1, .000073, .3,
              "STDP moved cache coefficients");
}
}

int main() {
    try {
        times_and_parameters();
        changing_non_noise_input();
        resizing_and_new_instances();
        stdp_coefficients();
        std::cout << "PASS " << checks << " bitwise noise/STDP cache checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL numerical cache: " << e.what() << '\n';
        return 1;
    }
}
