# Connectome credits

`pn_kc.csv` was extracted with `Tools/extract_pn_kc.py` from the FlyWire FAFB release 783 tables
(`connections_princeton.csv.gz`, `consolidated_cell_types.csv.gz`) downloaded from https://codex.flywire.ai.
It lists uniglomerular projection neuron -> Kenyon cell contacts with 3 or more synapses in the right hemisphere.

License: CC-BY 4.0 (https://creativecommons.org/licenses/by/4.0/). The full licence text is at
https://creativecommons.org/licenses/by/4.0/legalcode.

Changes made to the source data: only uniglomerular projection neuron -> Kenyon cell pairs with three or more
synapses in the right hemisphere were kept, the synapse counts were summed per pair, and everything else
(neuron ids, coordinates, other neuropils, other cell types) was dropped.

Please cite:

- Dorkenwald S. et al. (2024). Neuronal wiring diagram of an adult brain. *Nature* 634, 124-138.
- Schlegel P. et al. (2024). Whole-brain annotation and multi-connectome cell typing of Drosophila. *Nature* 634, 139-152.
