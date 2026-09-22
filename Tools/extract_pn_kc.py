"""
Extracts the projection neuron -> Kenyon cell wiring from the FlyWire connectome and writes
Resources/Connectome/pn_kc.csv, which OptiCompanion uses to wire the fly's mushroom body.

Inputs (download them from https://codex.flywire.ai/api/download, FlyWire public release 783):
  - connections_princeton.csv.gz (or connections.csv.gz): pre_root_id, post_root_id, neuropil, syn_count, ...
  - consolidated_cell_types.csv.gz: root_id, primary_type, ...   (or classification.csv.gz with cell_type)

Usage:
  python extract_pn_kc.py --connections connections_princeton.csv.gz --types consolidated_cell_types.csv.gz

The FlyWire data is CC-BY 4.0. If you publish the generated file, credit:
  Dorkenwald et al. 2024, "Neuronal wiring diagram of an adult brain", Nature 634.
  Schlegel et al. 2024, "Whole-brain annotation and multi-connectome cell typing of Drosophila", Nature 634.

Only the Python standard library is needed.
"""

import argparse
import csv
import gzip
import os
from collections import defaultdict

# The 51 uniglomerular PN types the plugin maps its inputs to (must match EOptiGlom::RealName).
GLOMERULI = [
    "DA1", "DA2", "DA3", "DA4l", "DA4m", "DC1", "DC2", "DC3", "DC4", "DL1", "DL2d", "DL2v", "DL3", "DL4", "DL5",
    "DM1", "DM2", "DM3", "DM4", "DM5", "DM6", "DP1l", "DP1m", "D", "VA1d", "VA1v", "VA2", "VA3", "VA4", "VA5",
    "VA6", "VA7l", "VA7m", "VC1", "VC2", "VC3", "VC4", "VC5", "VL1", "VL2a", "VL2p", "VM1", "VM2", "VM3", "VM4",
    "VM5d", "VM5v", "VM7d", "VM7v", "V", "VP1d",
]


def open_any(path):
    return gzip.open(path, "rt", newline="") if path.endswith(".gz") else open(path, newline="")


def glomerulus_of(cell_type):
    """'DA1_lPN' -> 'DA1'; returns None for multiglomerular or unknown PNs."""
    parts = (cell_type or "").split("_")
    if len(parts) < 2 or not parts[-1].upper().endswith("PN"):
        return None
    glom = parts[0]
    for known in GLOMERULI:
        if known.lower() == glom.lower():
            return known
    return None


def side_of_row(row, side_of, kc):
    """Hemisphere of a connection: from the cell annotations when they have it, else from the neuropil (CA_R, MB_CA_L...)."""
    side = side_of.get(kc, "")
    if side:
        return side
    neuropil = (row.get("neuropil") or "").upper()
    if neuropil.endswith("_R"):
        return "right"
    if neuropil.endswith("_L"):
        return "left"
    return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--connections", required=True)
    parser.add_argument("--types", required=True)
    parser.add_argument("--min-synapses", type=int, default=3, help="drop weaker PN->KC contacts (default 3)")
    parser.add_argument("--hemisphere", default="right", help="left, right or both (default right, like the hemibrain)")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "Resources", "Connectome", "pn_kc.csv"))
    args = parser.parse_args()

    pn_glomerulus = {}
    kenyon_cells = set()
    side_of = {}
    with open_any(args.types) as handle:
        for row in csv.DictReader(handle):
            root = row.get("root_id")
            cell_type = row.get("primary_type") or row.get("cell_type") or row.get("hemibrain_type") or ""
            side_of[root] = (row.get("side") or "").lower()
            if cell_type.upper().startswith("KC"):
                kenyon_cells.add(root)
                continue
            glom = glomerulus_of(cell_type)
            if glom:
                pn_glomerulus[root] = glom
    print(f"{len(pn_glomerulus)} uniglomerular PNs, {len(kenyon_cells)} Kenyon cells")

    edges = defaultdict(int)
    with open_any(args.connections) as handle:
        for row in csv.DictReader(handle):
            pre, post = row["pre_root_id"], row["post_root_id"]
            if pre in pn_glomerulus and post in kenyon_cells:
                if args.hemisphere != "both" and side_of_row(row, side_of, post) not in ("", args.hemisphere):
                    continue
                edges[(post, pn_glomerulus[pre])] += int(row.get("syn_count") or 1)

    rows = [(kc, glom, syn) for (kc, glom), syn in edges.items() if syn >= args.min_synapses]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["kc_id", "glomerulus", "synapses"])
        writer.writerows(sorted(rows))
    print(f"Wrote {len(rows)} PN->KC edges for {len({r[0] for r in rows})} Kenyon cells to {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()
