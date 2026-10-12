"""Compare lab snapshots across the explicit schema 2 -> 3 -> 4 migrations.

This adapts configuration metadata only; it contains no simulation equations.
The historical lab inputs stay in schema 2 so compatibility remains exercised.
"""
from copy import deepcopy


def canonical_config(config):
    result = deepcopy(config)
    if result["schema"] == 2:
        result["schema"] = 3
        for synapse in result["synapses"]:
            synapse.update(stdp=False, stdp_rule="additive", a_plus=.01,
                           a_minus=.012, tau_plus=.020, tau_minus=.020,
                           weight_min=0., weight_max=2.)
            # The old conductance model ignored its serialized weight field.
            if synapse["model"] == "conductance_stp":
                synapse["weight"] = 1.
    if result["schema"] == 3:
        result["schema"] = 4
        # Schemas 2/3 always record every state, and have no display-type flag.
        # Match C++ migration rather than retaining ignored legacy extra keys.
        result.update(record_all=True, record_neurons=[], record_synapses=[])
        for neuron in result["neurons"]:
            neuron["inhibitory"] = False
    elif result["schema"] != 4:
        raise ValueError("Lab snapshot comparison supports schema 2, 3 or 4.")
    return result
