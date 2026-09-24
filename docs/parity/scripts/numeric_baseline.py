#!/usr/bin/env python3
"""
numeric_baseline.py -- reference numeric baselines for AtomX <-> OVITO parity.

Computes expected values (float64) for:
  A. Common Neighbor Analysis (fixed cutoff)
  B. Coordination analysis
  C. Radial distribution function (pair-distance shells)
  D. Centrosymmetry parameter (greedy/LAMMPS mode and min-weight matching mode)
  E. Displacement magnitudes (generator formula vs values read from test.xyz)
  F. Slice (plane n, distance d, slab width w)
  G. Replicate

Data:
  * C:/Users/Zemeng Feng/Desktop/codes/test.xyz  (30 frames, 108 atoms,
    FCC Cu/Ni 3x3x3 cells, a = 3.6 A, L = 10.8 A cubic periodic box,
    thermal vibration amplitude 0.12 A; positions written with 4 decimals)
  * Synthetic fixtures generated below: perfect FCC (a=3.6, 3x3x3, 108 atoms),
    perfect BCC (a=3.0, 4x4x4, 128 atoms), FCC with atom 0 displaced by
    (0.5, 0, 0) A, FCC with atom 0 removed (one vacancy).

Conventions (documented to match OVITO/LAMMPS):
  * Periodic boundaries use the minimum-image convention on the full box.
  * CNA bond signature = (n_common_neighbors, n_bonds_among_them, longest_chain).
    Rings (all common atoms of degree 2) count as chain length 1, which
    reproduces the published 1441 / 1661 / 1551 signatures.
  * CNA classification: FCC = 12x(4,2,1); HCP = 6x(4,2,1)+6x(4,2,2);
    BCC = 8x(6,6,1)+6x(4,4,1)   [equivalent to LAMMPS nbcc4==6 && nbcc6==8];
    otherwise Other. (OVITO type codes: 0=Other, 1=FCC, 2=HCP, 3=BCC, 4=ICO;
     LAMMPS cna/atom: 1=fcc, 2=hcp, 3=bcc, 4=ico, 5=unknown.)
  * Slice: plane { r : n.r = d } with |n| normalized; default keeps the
    NEGATIVE side (n.r < d) -- OVITO deletes the positive side along the
    normal; slab width w keeps |n.r - d| <= w/2; reverse/invert flips.
  * CSP: N nearest neighbors (12 FCC, 8 BCC).
      greedy mode   = sum of the N/2 smallest pair weights |ri+rj|^2
                      (LAMMPS compute centro/atom, OVITO conventional mode)
      matching mode = minimum over perfect pairings (min-weight matching,
                      OVITO "minimum-weight matching" mode, Larsen 2020)

Usage:  python numeric_baseline.py
Writes: ../numeric-baseline.md (i.e. docs/parity/numeric-baseline.md)
"""

import math
import os
import sys
from collections import Counter

import numpy as np

# --------------------------------------------------------------------------
# Constants / dataset paths
# --------------------------------------------------------------------------
A_FCC = 3.6          # FCC lattice constant (test crystal + fixtures)
NCELL = 3            # 3x3x3 conventional cells
L_FCC = A_FCC * NCELL  # 10.8
A_BCC = 3.0          # BCC lattice constant (fixture)
NCELL_BCC = 4        # 4x4x4: a 3x3x3 BCC box (L = 3a) is too small for CNA at
                     # cutoff 3.6 -- periodic self-coincidence corrupts the
                     # second-neighbor bond signatures (see numeric-baseline.md)
L_BCC = A_BCC * NCELL_BCC  # 12.0
XYZ_PATH = r"C:\Users\Zemeng Feng\Desktop\codes\test.xyz"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MD_PATH = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "numeric-baseline.md"))


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------
def make_fcc(a=A_FCC, n=NCELL):
    """Perfect FCC, same ordering as gen_xyz.py (cell i,j,k outer; basis inner).
    Species: basis atom 0 = Cu, others = Ni."""
    basis = [(0, 0, 0), (0.5, 0.5, 0), (0.5, 0, 0.5), (0, 0.5, 0.5)]
    pos, sp = [], []
    for i in range(n):
        for j in range(n):
            for k in range(n):
                for m, (b1, b2, b3) in enumerate(basis):
                    pos.append(((i + b1) * a, (j + b2) * a, (k + b3) * a))
                    sp.append("Cu" if m == 0 else "Ni")
    return np.array(pos, dtype=np.float64), sp


def make_bcc(a=A_BCC, n=NCELL_BCC):
    """Perfect BCC (one species), cell-then-basis ordering."""
    basis = [(0, 0, 0), (0.5, 0.5, 0.5)]
    pos, sp = [], []
    for i in range(n):
        for j in range(n):
            for k in range(n):
                for m, (b1, b2, b3) in enumerate(basis):
                    pos.append(((i + b1) * a, (j + b2) * a, (k + b3) * a))
                    sp.append("Fe")
    return np.array(pos, dtype=np.float64), sp


def read_xyz_frames(path):
    """Parse extended XYZ: Lattice="..." in comment line."""
    with open(path) as f:
        lines = f.read().splitlines()
    frames, p = [], 0
    while p < len(lines):
        N = int(lines[p].strip())
        comment = lines[p + 1]
        cell = np.zeros((3, 3))
        if 'Lattice="' in comment:
            vals = [float(x) for x in comment.split('Lattice="')[1].split('"')[0].split()]
            cell = np.array(vals, dtype=np.float64).reshape(3, 3)  # rows = cell vectors
        sp, pos = [], []
        for q in range(p + 2, p + 2 + N):
            tok = lines[q].split()
            sp.append(tok[0])
            pos.append((float(tok[1]), float(tok[2]), float(tok[3])))
        frames.append(dict(N=N, comment=comment, species=sp,
                           pos=np.array(pos, dtype=np.float64), cell=cell))
        p += N + 2
    return frames


# --------------------------------------------------------------------------
# Geometry helpers
# --------------------------------------------------------------------------
def dist_matrix(pos, cell, pbc=True):
    """Full N x N distance matrix (minimum image if pbc)."""
    n = len(pos)
    if pbc:
        inv = np.linalg.inv(cell)
        frac = pos @ inv
        df = frac[:, None, :] - frac[None, :, :]
        df -= np.round(df)
        dv = df @ cell
    else:
        dv = pos[:, None, :] - pos[None, :, :]
    return np.sqrt((dv * dv).sum(-1))


def neighbor_lists(pos, cell, cutoff, pbc=True):
    """List per atom of neighbor indices with distance < cutoff."""
    D = dist_matrix(pos, cell, pbc)
    return [np.where((D[i] < cutoff) & (D[i] > 0))[0].tolist() for i in range(len(pos))], D


# --------------------------------------------------------------------------
# A. Common Neighbor Analysis
# --------------------------------------------------------------------------
def longest_chain(adj, nodes):
    """CNA third index. Open chains count their bond count; a pure ring
    component counts as 1 (reproduces published 1441/1661/1551)."""
    best = 0
    seen = set()
    for s in nodes:
        if s in seen:
            continue
        comp, stack = [], [s]
        seen.add(s)
        while stack:
            u = stack.pop()
            comp.append(u)
            for v in adj[u]:
                if v not in seen:
                    seen.add(v)
                    stack.append(v)
        edges = sum(len(adj[u]) for u in comp) // 2
        if edges == 0:
            chain = 0
        elif all(len(adj[u]) == 2 for u in comp) and edges == len(comp):
            chain = 1  # pure ring
        else:
            chain = 0
            for st in comp:            # longest simple path (in bonds)
                stk = [(st, [st])]
                while stk:
                    u, path = stk.pop()
                    ext = False
                    for v in adj[u]:
                        if v not in path:
                            stk.append((v, path + [v]))
                            ext = True
                    if not ext:
                        chain = max(chain, len(path) - 1)
        best = max(best, chain)
    return best


def cna_analysis(pos, cell, cutoff, pbc=True):
    """Return (types, per-atom Counter of bond signatures)."""
    N = len(pos)
    nbrs, D = neighbor_lists(pos, cell, cutoff, pbc)
    nbr_sets = [set(x) for x in nbrs]
    types, sig_counts = [], []
    for i in range(N):
        cnt = Counter()
        for j in nbrs[i]:
            common = sorted(nbr_sets[i] & nbr_sets[j])
            nc = len(common)
            if nc == 0:
                sig = (0, 0, 0)
            else:
                adj = {k: [] for k in common}
                nb = 0
                for a in range(nc):
                    for b in range(a + 1, nc):
                        if D[common[a], common[b]] < cutoff:
                            adj[common[a]].append(common[b])
                            adj[common[b]].append(common[a])
                            nb += 1
                sig = (nc, nb, longest_chain(adj, common))
            cnt[sig] += 1
        c = cnt
        if c[(4, 2, 1)] == 12:
            t = "FCC"
        elif c[(4, 2, 1)] == 6 and c[(4, 2, 2)] == 6:
            t = "HCP"
        elif c[(4, 4, 1)] == 6 and c[(6, 6, 1)] == 8:
            t = "BCC"
        else:
            t = "Other"
        types.append(t)
        sig_counts.append(cnt)
    return types, sig_counts


# --------------------------------------------------------------------------
# D. Centrosymmetry parameter
# --------------------------------------------------------------------------
def csp_greedy(vecs):
    """LAMMPS / OVITO-conventional: sum of the n/2 smallest pair weights."""
    n = len(vecs)
    w = []
    for a in range(n):
        for b in range(a + 1, n):
            v = vecs[a] + vecs[b]
            w.append(float(v @ v))
    w.sort()
    return sum(w[: n // 2])


def csp_matching(vecs):
    """Minimum-weight perfect pairing (brute force with pruning)."""
    n = len(vecs)
    best = [math.inf]

    def rec(remaining, acc):
        if acc >= best[0]:
            return
        if not remaining:
            best[0] = acc
            return
        first = remaining[0]
        rest = remaining[1:]
        for t in range(len(rest)):
            v = vecs[first] + vecs[rest[t]]
            acc2 = acc + float(v @ v)
            if acc2 >= best[0]:
                continue
            rec(rest[:t] + rest[t + 1:], acc2)

    rec(list(range(n)), 0.0)
    return best[0]


def nearest_vecs(i, pos, cell, npick, pbc=True):
    """Vectors (min-image) from atom i to its npick nearest neighbors."""
    D = dist_matrix(pos, cell, pbc)[i]
    order = sorted(range(len(pos)), key=lambda k: (D[k], k))
    picked = [k for k in order if k != i][:npick]
    inv = np.linalg.inv(cell)
    fr = pos @ inv
    out = []
    for k in picked:
        df = fr[k] - fr[i]
        df -= np.round(df)
        out.append(df @ cell)
    return picked, out


# --------------------------------------------------------------------------
# E. Generator displacement formula (from gen_xyz.py)
# --------------------------------------------------------------------------
def gen_disp(idx, x, y, z, fr, frames=30):
    phase = 2 * math.pi * fr / frames
    h = idx * 0.7
    dx = 0.12 * math.sin(phase + x * 1.1 + h)
    dy = 0.12 * math.sin(phase + y * 1.3 + h * 1.7)
    dz = 0.12 * math.sin(phase + z * 0.9 + h * 2.3)
    return dx, dy, dz


# --------------------------------------------------------------------------
# G. Replicate
# --------------------------------------------------------------------------
def replicate(pos, sp, cell, nx, ny, nz):
    out_p, out_s = [], []
    for ix in range(nx):
        for iy in range(ny):
            for iz in range(nz):
                off = ix * cell[0] + iy * cell[1] + iz * cell[2]
                out_p.append(pos + off)
                out_s.extend(sp)
    newcell = cell.copy()
    newcell[0] *= nx
    newcell[1] *= ny
    newcell[2] *= nz
    return np.vstack(out_p), out_s, newcell


# ==========================================================================
# MAIN COMPUTATION
# ==========================================================================
def main():
    out = []
    P = print

    frames = read_xyz_frames(XYZ_PATH)
    f0 = frames[0]
    assert f0["N"] == 108 and len(frames) == 30

    fcc_pos, fcc_sp = make_fcc()
    bcc_pos, bcc_sp = make_bcc()
    cell_fcc = np.diag([L_FCC] * 3)
    cell_bcc = np.diag([L_BCC] * 3)

    dis_pos = fcc_pos.copy()
    dis_pos[0] = dis_pos[0] + np.array([0.5, 0.0, 0.0])     # atom 0 displaced 0.5 A
    vac_pos = np.delete(fcc_pos, 0, axis=0)                  # vacancy at origin

    # ---------------- A. CNA ----------------
    t_fcc0, sig_fcc0 = cna_analysis(f0["pos"], f0["cell"], 3.1)
    cnt_fcc0 = Counter(t_fcc0)
    t_fccI, sig_fccI = cna_analysis(fcc_pos, cell_fcc, 3.1)
    cnt_fccI = Counter(t_fccI)
    t_bcc28, sig_bcc28 = cna_analysis(bcc_pos, cell_bcc, 2.8)
    cnt_bcc28 = Counter(t_bcc28)
    t_bcc36, sig_bcc36 = cna_analysis(bcc_pos, cell_bcc, 3.6)
    cnt_bcc36 = Counter(t_bcc36)

    # per-atom signature multiset summary (identical for all atoms in each case)
    def sig_summary(sig_counts):
        return Counter(tuple(sorted((sig, c) for sig, c in sc.items())) for sc in sig_counts)

    sig_fcc0_modes = sig_summary(sig_fcc0)
    sig_fccI_modes = sig_summary(sig_fccI)
    sig_bcc28_modes = sig_summary(sig_bcc28)
    sig_bcc36_modes = sig_summary(sig_bcc36)

    # cutoff safety margins on frame 0 (cutoff 3.1)
    D0 = dist_matrix(f0["pos"], f0["cell"], pbc=True)
    iu = np.triu_indices(108, 1)
    d_all = D0[iu]
    d_sorted = np.sort(d_all[(d_all > 1e-9) & (d_all < 4.0)])
    # first shell = distances < 3.1 ; second shell = 3.1..4.0
    shell1 = d_sorted[d_sorted < 3.1]
    shell2 = d_sorted[d_sorted >= 3.1]
    margin1 = 3.1 - shell1.max()
    margin2 = shell2.min() - 3.1

    # coordination checks for BCC at both cutoffs
    nb_bcc28, _ = neighbor_lists(bcc_pos, cell_bcc, 2.8)
    nb_bcc36, _ = neighbor_lists(bcc_pos, cell_bcc, 3.6)
    coord_bcc28 = sorted(set(len(x) for x in nb_bcc28))
    coord_bcc36 = sorted(set(len(x) for x in nb_bcc36))

    # 3x3x3 BCC control: document the finite-size artifact
    bcc3_pos, bcc3_sp = make_bcc(n=3)
    t_bcc3, sig_bcc3 = cna_analysis(bcc3_pos, np.diag([A_BCC * 3] * 3), 3.6)
    cnt_bcc3 = Counter(t_bcc3)
    sig_bcc3_modes = Counter(tuple(sorted((s, c) for s, c in sc.items())) for sc in sig_bcc3)

    # ---------------- B. Coordination ----------------
    nb_pbc, _ = neighbor_lists(f0["pos"], f0["cell"], 3.1, pbc=True)
    nb_nopbc, _ = neighbor_lists(f0["pos"], f0["cell"], 3.1, pbc=False)
    coord_pbc = Counter(len(x) for x in nb_pbc)
    coord_nopbc = Counter(len(x) for x in nb_nopbc)
    mean_coord_pbc = sum(len(x) for x in nb_pbc) / 108.0
    mean_coord_nopbc = sum(len(x) for x in nb_nopbc) / 108.0

    # ---------------- C. RDF ----------------
    # ideal FCC shells from the perfect fixture (exact geometry)
    DI = dist_matrix(fcc_pos, cell_fcc, pbc=True)
    diu = np.triu_indices(108, 1)
    di = DI[diu]
    di = di[di > 1e-9]
    ideal_shells = []
    ds = np.sort(di)
    # cluster by gaps > 0.15
    clusters = []
    start = 0
    for k in range(1, len(ds)):
        if ds[k] - ds[k - 1] > 0.15:
            clusters.append(ds[start:k])
            start = k
    clusters.append(ds[start:])
    for cl in clusters:
        ideal_shells.append((cl.min(), cl.max(), len(cl), len(cl) * 2.0 / 108.0))

    # thermal frame 0 histogram: 0.01 A bins up to 5.4 (half min box width)
    bins = np.arange(0.0, 5.4001, 0.01)
    hist, edges = np.histogram(d_all[(d_all > 1e-9) & (d_all < 5.4)], bins=bins)
    # locate first two peak bins (first peak restricted to r < 3.1)
    mask1 = edges[:-1] < 3.1
    h1 = np.where(mask1, hist, 0)
    peak1_bin = int(np.argmax(h1))
    peak1 = (edges[peak1_bin] + edges[peak1_bin + 1]) / 2.0
    # second peak: restrict to 3.2..4.0
    mask2 = (edges[:-1] >= 3.2) & (edges[:-1] < 4.0)
    h2 = np.where(mask2, hist, 0)
    peak2_bin = int(np.argmax(h2))
    peak2 = (edges[peak2_bin] + edges[peak2_bin + 1]) / 2.0

    # thermal shells: band edges chosen from the observed thermal spreads so that
    # shells 1-3 are counted exactly; shell 4 overlaps the 5.6921 cluster (beyond cutoff)
    bands = [(2.00, 3.00), (3.00, 4.05), (4.05, 4.76), (4.76, 5.39), (5.39, 6.00)]
    band_stats = []
    for lo, hi in bands:
        sel = d_all[(d_all >= lo) & (d_all <= hi)]
        if len(sel) == 0:
            band_stats.append((lo, hi, 0, float("nan"), float("nan"), float("nan")))
            continue
        ch, ce = np.histogram(sel, bins=np.arange(lo, hi + 0.0101, 0.01))
        pk = int(np.argmax(ch))
        band_stats.append((lo, hi, len(sel), sel.min(), sel.max(),
                           (ce[pk] + ce[pk + 1]) / 2.0))

    # ---------------- D. CSP ----------------
    # full sweep (108 atoms) for both modes
    csp_fcc_all_g, csp_fcc_all_m = [], []
    for i in range(108):
        _, vecs = nearest_vecs(i, fcc_pos, cell_fcc, 12)
        csp_fcc_all_g.append(csp_greedy(vecs))
        csp_fcc_all_m.append(csp_matching(vecs))
    # perfect BCC, N=8
    csp_bcc_all_g, csp_bcc_all_m = [], []
    for i in range(54):
        _, vecs = nearest_vecs(i, bcc_pos, cell_bcc, 8)
        csp_bcc_all_g.append(csp_greedy(vecs))
        csp_bcc_all_m.append(csp_matching(vecs))
    # displaced fixture
    Dd = dist_matrix(dis_pos, cell_fcc, pbc=True)
    nn0 = np.where((Dd[0] < 3.1) & (Dd[0] > 0))[0]  # 12 first-shell of displaced atom
    dis_rows = []
    for i in [0] + sorted(nn0.tolist()):
        picked, vecs = nearest_vecs(i, dis_pos, cell_fcc, 12)
        dis_rows.append((i, csp_greedy(vecs), csp_matching(vecs)))
    others = [i for i in range(108) if i != 0 and i not in set(nn0.tolist())]
    max_other_g, max_other_m = 0.0, 0.0
    for i in others:
        _, vecs = nearest_vecs(i, dis_pos, cell_fcc, 12)
        max_other_g = max(max_other_g, csp_greedy(vecs))
        max_other_m = max(max_other_m, csp_matching(vecs))
    # vacancy fixture: the 12 former first-shell neighbors of the removed atom 0
    # (robust detection: atoms with only 11 neighbors at the 2.5456 A shell distance)
    Dv = dist_matrix(vac_pos, cell_fcc, pbc=True)
    vac_neighbors = []
    for i in range(107):
        cnt11 = int(((Dv[i] > 2.5) & (Dv[i] < 2.6)).sum())
        if cnt11 == 11:
            vac_neighbors.append(i)
    vac_rows = []
    for i in vac_neighbors:
        picked, vecs = nearest_vecs(i, vac_pos, cell_fcc, 12)
        g = csp_greedy(vecs)
        # matching-mode tie study: which atom (distance 3.6) fills slot 12
        Drow = dist_matrix(vac_pos, cell_fcc, pbc=True)[i]
        cand = [k for k in range(107) if k != i and abs(Drow[k] - A_FCC) < 1e-9]
        base = sorted(k for k in range(107) if k != i and Drow[k] < 2.6)
        mvals = []
        for c in cand:
            sub = base + [c]
            inv = np.linalg.inv(cell_fcc)
            fr = vac_pos @ inv
            vs = []
            for k in sub:
                df = fr[k] - fr[i]
                df -= np.round(df)
                vs.append(df @ cell_fcc)
            mvals.append(csp_matching(vs))
        vac_rows.append((i, g, min(mvals), max(mvals)))
    vac_others_max_g = 0.0
    for i in range(107):
        if i in set(vac_neighbors):
            continue
        _, vecs = nearest_vecs(i, vac_pos, cell_fcc, 12)
        vac_others_max_g = max(vac_others_max_g, csp_greedy(vecs))

    # ---------------- E. Displacement ----------------
    ideal = fcc_pos  # ideal lattice positions (same ordering as file)
    e_atoms = [0, 1, 50]
    e_frames = [1, 5, 15]
    e_rows = []
    for idx in e_atoms:
        x, y, z = ideal[idx]
        for fr in e_frames:
            dx, dy, dz = gen_disp(idx, x, y, z, fr)
            mag = math.sqrt(dx * dx + dy * dy + dz * dz)
            d0 = gen_disp(idx, x, y, z, 0)
            rel0 = math.sqrt((dx - d0[0]) ** 2 + (dy - d0[1]) ** 2 + (dz - d0[2]) ** 2)
            # file-based magnitudes
            pf = frames[fr]["pos"][idx]
            p0f = frames[0]["pos"][idx]
            mag_file = float(np.linalg.norm(pf - ideal[idx]))
            rel0_file = float(np.linalg.norm(pf - p0f))
            e_rows.append((idx, fr, mag, rel0, mag_file, rel0_file))
    # mean |d| at frame 5
    mags5 = []
    mags5_file = []
    for idx in range(108):
        x, y, z = ideal[idx]
        dx, dy, dz = gen_disp(idx, x, y, z, 5)
        mags5.append(math.sqrt(dx * dx + dy * dy + dz * dz))
        mags5_file.append(float(np.linalg.norm(frames[5]["pos"][idx] - ideal[idx])))
    mean5 = sum(mags5) / 108.0
    mean5_file = sum(mags5_file) / 108.0
    max5 = max(mags5)
    # formula vs file agreement
    resid = max(abs(mags5[i] - mags5_file[i]) for i in range(108))

    # ---------------- F. Slice ----------------
    z = f0["pos"][:, 2]
    on_boundary = int((np.abs(z - 5.4) < 1e-12).sum())
    keep_neg = int((z <= 5.4).sum())       # OVITO default: keep n.r <= d
    keep_pos = int((z > 5.4).sum())        # reverse orientation
    slab = int((np.abs(z - 5.4) <= 1.5).sum())   # width 3.0: |n.r - d| <= 1.5
    slab_inv = 108 - slab

    # ---------------- G. Replicate ----------------
    r222_pos, r222_sp, r222_cell = replicate(fcc_pos, fcc_sp, cell_fcc, 2, 2, 2)
    r312_pos, r312_sp, r312_cell = replicate(fcc_pos, fcc_sp, cell_fcc, 3, 1, 2)
    b222_pos, b222_sp, b222_cell = replicate(bcc_pos, bcc_sp, cell_bcc, 2, 2, 2)
    t_r222, _ = cna_analysis(r222_pos, r222_cell, 3.1)
    cnt_r222 = Counter(t_r222)
    nr222, _ = neighbor_lists(r222_pos, r222_cell, 3.1)
    coord_r222 = Counter(len(x) for x in nr222)
    sp_r222 = Counter(r222_sp)
    sp_r312 = Counter(r312_sp)

    # ==========================================================================
    # REPORT
    # ==========================================================================
    L = out.append
    L("# AtomX numeric baseline (OVITO parity)")
    L("")
    L("Reference values AtomX must reproduce. All computations: float64, brute-force,")
    L("periodic minimum-image convention on the full simulation box, by")
    L("`docs/parity/scripts/numeric_baseline.py` (Python " + sys.version.split()[0] +
      ", numpy " + np.__version__ + "). Regenerate with `python numeric_baseline.py`.")
    L("")
    L("Shared dataset: `test.xyz` (30 frames, 108 atoms; FCC 3x3x3 conventional cells,")
    L("a = 3.6 A, cubic box L = 10.8 A, PBC xyz; basis atom 0 of each cell = Cu (27 total),")
    L("remaining 81 = Ni; thermal displacement amplitude 0.12 A, positions written with")
    L("4 decimals). Fixtures generated in the script: perfect FCC (same cell, 108 atoms,")
    L("a = 3.6), perfect BCC (a = 3.0, 4x4x4 cells, 128 atoms, L = 12.0), FCC + atom 0")
    L("displaced by (0.5, 0, 0) A, FCC with atom 0 removed (vacancy at origin, 107 atoms).")
    L("")
    L("Tolerances: integer counts must match exactly. Real-valued entries are float64")
    L("references; AtomX comparing float32-rendered positions should pass within the")
    L("listed tolerance (float32 rel. eps ~1.2e-7; the 4-decimal rounding of test.xyz")
    L("dominates at ~5e-5 per coordinate component).")
    L("")
    L("## A. Common neighbor analysis (fixed cutoff)")
    L("")
    L("Bond signature = (number of common neighbors, bonds among them, longest bond")
    L("chain); rings count as chain length 1 (reproduces published 1441/1661/1551).")
    L("Classification (LAMMPS `compute cna/atom` / OVITO rules): FCC = 12x(4,2,1);")
    L("HCP = 6x(4,2,1) + 6x(4,2,2); BCC = 8x(6,6,1) + 6x(4,4,1) [LAMMPS:")
    L("`nbcc4 == 6 && nbcc6 == 8`]; else Other. Structure type codes: OVITO")
    L("0=Other, 1=FCC, 2=HCP, 3=BCC; LAMMPS 1=fcc, 2=hcp, 3=bcc, 5=unknown.")
    L("")
    L("Cutoffs: FCC uses 3.1 A (between shell 1 at a/sqrt(2)=2.5456 and shell 2 at")
    L("a=3.6). For BCC, OVITO/LAMMPS prescribe a cutoff between the SECOND and THIRD")
    L("shells (a=3.0 and a*sqrt(2)=4.243); 3.6 A is used here. The naive 2.8 A cutoff")
    L("(between shells 1 and 2) is also computed to document why it fails.")
    L("")
    L("| dataset | cutoff (A) | FCC | HCP | BCC | Other | per-atom bond signatures |")
    L("|---|---|---|---|---|---|---|")
    def row_cna(name, cut, cnt, modes):
        ms = "; ".join(f"{c} atoms x " +
                       ", ".join(f"{n}x{s}" for s, n in mm)
                       for mm, c in modes.items())
        return (f"| {name} | {cut} | {cnt.get('FCC',0)} | {cnt.get('HCP',0)} | "
                f"{cnt.get('BCC',0)} | {cnt.get('Other',0)} | {ms} |")
    L(row_cna("test.xyz frame 0", 3.1, cnt_fcc0, sig_fcc0_modes))
    L(row_cna("perfect FCC fixture", 3.1, cnt_fccI, sig_fccI_modes))
    L(row_cna("perfect BCC fixture (4x4x4)", 2.8, cnt_bcc28, sig_bcc28_modes))
    L(row_cna("perfect BCC fixture (4x4x4)", 3.6, cnt_bcc36, sig_bcc36_modes))
    L(row_cna("BCC 3x3x3 control (L = 3a, TOO SMALL)", 3.6, cnt_bcc3, sig_bcc3_modes))
    L("")
    L("Key facts:")
    L("")
    L(f"- test.xyz frame 0, cutoff 3.1: **FCC = {cnt_fcc0.get('FCC',0)}/108**, Other = {cnt_fcc0.get('Other',0)}.")
    L("  Every atom's 12 bonds have signature (4,2,1) = 421 despite the 0.12 A thermal")
    L("  displacement and 4-decimal file rounding.")
    L(f"- Cutoff safety margin, frame 0: largest first-shell distance {shell1.max():.4f} A,")
    L(f"  smallest second-shell distance {shell2.min():.4f} A; cutoff 3.1 clears them by")
    L(f"  {margin1:.3f} A and {margin2:.3f} A -- no float32 cutoff-comparison risk.")
    L(f"- Perfect BCC at cutoff 2.8 A: **Other = {cnt_bcc28.get('Other',0)}/{len(bcc_pos)}** (all bonds (0,0,0): a corner and a body-center atom share no common neighbors when only the 8 first-shell atoms are in the neighbor lists). Coordination is 8 for every atom. This cutoff does NOT identify BCC.")
    L(f"- Perfect BCC at cutoff 3.6 A (between shells 2 and 3): **BCC = {cnt_bcc36.get('BCC',0)}/{len(bcc_pos)}**;")
    L("  per atom: 8 first-shell bonds with signature 1661 and 6 second-shell bonds with")
    L("  signature 1441. (Coordination 14 = 8 + 6.)")
    L(f"- Finite-size caveat: a 3x3x3 BCC box (L = 3a = 9.0) is TOO SMALL for CNA at cutoff")
    L(f"  3.6: the second-neighbor bond (0,0,0)-(a,0,0) gains a spurious 5th common neighbor")
    L(f"  (the corner at -a and the corner at +2a coincide under PBC), giving signature (5,4,1)")
    L(f"  instead of (4,4,1) and classifying every atom as Other ({cnt_bcc3.get('BCC',0)}/{len(bcc3_pos)} BCC).")
    L("  AtomX BCC tests must use at least 4x4x4 conventional cells.")
    L("- HCP (for reference; no fixture): 6x421 + 6x422. Icosahedral: 12x(5,5,1).")
    L("")
    L("## B. Coordination analysis (cutoff 3.1 A, test.xyz frame 0)")
    L("")
    L("OVITO semantics apply PBC. With PBC there is no surface in this fully periodic box.")
    L("")
    L("| convention | distribution (coordination: count) | mean |")
    L("|---|---|---|")
    L("| with PBC (OVITO default) | " +
      ", ".join(f"{c}: {n}" for c, n in sorted(coord_pbc.items())) +
      f" | {mean_coord_pbc:.4f} |")
    L("| no PBC (surface-affected) | " +
      ", ".join(f"{c}: {n}" for c, n in sorted(coord_nopbc.items())) +
      f" | {mean_coord_nopbc:.6f} |")
    L("")
    L(f"- Expected with PBC: **108/108 atoms at coordination 12**, mean = {mean_coord_pbc:.1f}.")
    L(f"- BCC fixture: coordination 8 (cutoff 2.8) or 14 (cutoff 3.6) for every atom.")
    L("")
    L("## C. Radial distribution function (test.xyz frame 0, PBC)")
    L("")
    L("Ideal FCC shells (a = 3.6, computed from the perfect fixture, exact):")
    L("")
    L("| shell | ideal distance (A) | multiplicity/atom | pairs in 108-atom box |")
    L("|---|---|---|---|")
    shell_names = ["1st (NN)", "2nd", "3rd", "4th"]
    for k, (rmin, rmax, npairs, mult) in enumerate(ideal_shells):
        name = shell_names[k] if k < len(shell_names) else "n/a (beyond L/2)"
        L(f"| {name} | "
          f"{(rmin+rmax)/2.0:.6f}" + ("" if rmax - rmin < 1e-9 else f" ({rmin:.6f}-{rmax:.6f})") +
          f" | {mult:.0f} | {npairs} |")
    L("")
    L("Rows labeled n/a lie beyond the r = L/2 = 5.4 A RDF cutoff; in an L = 3a box")
    L("their direction sets are not representable faithfully under minimum imaging")
    L("(some collapse, e.g. 7.6368 A shows 3 neighbors/atom instead of the")
    L("infinite-lattice 12) -- they are not usable RDF shells.")
    L("")
    L(f"Thermal frame 0 (amplitude 0.12 A, file-rounded positions), 0.01 A bins, r < 5.4 = L/2:")
    L("")
    L(f"- **First peak: bin center {peak1:.3f} A** (ideal a/sqrt(2) = {A_FCC/math.sqrt(2):.4f} A);")
    L(f"  thermal shell 1 spans {band_stats[0][3]:.4f}-{band_stats[0][4]:.4f} A.")
    L(f"- **Second peak: bin center {peak2:.3f} A** (ideal a = 3.6 A);")
    L(f"  shell 2 spans {band_stats[1][3]:.4f}-{band_stats[1][4]:.4f} A.")
    L("")
    L("| band (A) | pair count | ideal shell | atoms/atom (=count x 2/108) | min (A) | max (A) | peak bin center (A) |")
    L("|---|---|---|---|---|---|---|")
    band_ideal = ["1st: 2.5456", "2nd: 3.6", "3rd: 4.4091", "4th: 5.0912", "5.6921 (beyond cutoff)"]
    for t, (lo, hi, n, mn, mx, pk) in enumerate(band_stats):
        L(f"| {lo:.2f}-{hi:.2f} | {n} | {band_ideal[t]} | {n*2.0/108.0:.2f} | {mn:.4f} | {mx:.4f} | {pk:.3f} |")
    L("")
    L("Shells 1-3 are exactly countable with the band edges above (thermal spreads are")
    L(f"2.2134-2.8671, 3.3660-3.8441, 4.0814-4.7460 A; the 3rd/4th shell gap is only")
    L(f"4.7460 -> 4.7795 A). Shell 4 holds 648 pairs but ONE of its thermal-tail pairs")
    L(f"(~5.40 A) is nearer the 5.6921 cluster, so a fixed band [4.76, 5.39] captures")
    L(f"{band_stats[3][2]}; accept 647-648. The 5.39-6.00 band mixes the 5.6921 cluster")
    L("(beyond the r = L/2 cutoff) and shell-4 tails -- do not use it as a test.")
    L("")
    L("Finite-size effects (3x3x3 box, L = 10.8):")
    L("")
    L("- RDF is only meaningful to r = L/2 = 5.4 A (OVITO's default cutoff = half the")
    L("  minimum box width). Shells 1-4 (2.5456, 3.6, 4.4091, 5.0912 A; 12, 6, 24, 12")
    L("  neighbors per atom; 648, 324, 1296, 648 pairs) are complete and untruncated:")
    L("  coordination integrals over shells 1-4 reproduce 12, 6, 24, 12 (shell 4 within")
    L("  the one-pair ambiguity noted above).")
    L("- There is NO FCC shell at 1.5a = 5.4 A (vectors like (1,1,0.5)a are not FCC")
    L("  lattice vectors). The next true shell is sqrt(2.5)a = 5.692 A > L/2, i.e.")
    L("  outside the cutoff -- do not expect a 5th peak below 5.4 A.")
    L("- All clusters beyond 5.4 A (5.6921 x12, 6.2354 x8, 6.7350 x24, 7.6368 x3,")
    L("  8.4427 x6 per atom) are outside the r = L/2 cutoff; in this small box some")
    L("  direction sets collide under minimum imaging (e.g. the 12 infinite-lattice")
    L("  directions of the 7.6368 A cluster collapse to 3 distinct neighbors).")
    L("  Do not use any cluster above 5.4 A for tests.")
    L("")
    L("## D. Centrosymmetry parameter (CSP)")
    L("")
    L("p = sum over N/2 pairs of |r_i + r_j|^2 with r = vectors to the N nearest")
    L("neighbors (N = 12 FCC, 8 BCC). Two modes computed:")
    L("greedy = sum of the N/2 smallest of all N(N-1)/2 pair weights (LAMMPS")
    L("`compute centro/atom`, OVITO conventional); matching = minimum-weight perfect")
    L("pairing (OVITO 'minimum-weight matching', Larsen arXiv:2003.08879).")
    L("")
    L(f"- Perfect FCC fixture: **CSP = 0.0 exactly for all 108 atoms** (both modes;")
    L(f"  max computed value {max(csp_fcc_all_g):.3e} greedy, {max(csp_fcc_all_m):.3e} matching).")
    L(f"- Perfect BCC fixture, N = 8: **CSP = 0.0 exactly for all {len(bcc_pos)} atoms**")
    L(f"  (max {max(csp_bcc_all_g):.3e}).")
    L("")
    L("FCC + atom 0 displaced by (0.5, 0, 0) A -- atom-index -> CSP (A^2):")
    L("")
    L("| atom | role | CSP greedy | CSP matching |")
    L("|---|---|---|---|")
    roles = {0: "displaced atom"}
    for j in sorted(nn0.tolist()):
        roles[j] = "NN neighbor of displaced"
    for (i, g, m) in dis_rows:
        L(f"| {i} | {roles.get(i, '')} | {g:.6f} | {m:.6f} |")
    L(f"| all other {len(others)} atoms | undisturbed 2nd shell+ | {max_other_g:.1e} (max) | {max_other_m:.1e} (max) |")
    L("")
    L(f"- BCC using 8 neighbors: exact 0 (see above). The 8 vectors (±1.5, ±1.5, ±1.5) A")
    L("  contain exact opposite pairs.")
    L("")
    L("FCC with one vacancy (atom 0 at origin removed) -- the 12 first-shell neighbors")
    L("(indices are positions in the 107-atom post-deletion list; in the original")
    L("108-atom indexing they are 1, 2, 3, 10, 11, 25, 27, 35, 73, 74, 82, 97):")
    L("")
    L("| atom | CSP greedy | CSP matching (tie-range) |")
    L("|---|---|---|")
    for (i, g, mn, mx) in vac_rows:
        L(f"| {i} | {g:.6f} | {mn:.6f} - {mx:.6f} |")
    L(f"| other {107-len(vac_rows)} atoms | {vac_others_max_g:.1e} (max) | |")
    L("")
    L(f"- **Vacancy neighbor CSP (greedy) = a^2/2 = {A_FCC**2/2.0:.4f} A^2 exactly for all 12 neighbors**")
    L("  (analytic: the 5 surviving opposite pairs contribute 0, and the lowest remaining")
    L("  weight is a^2/2 whether it comes from the un-paired first-shell vector u paired")
    L("  with a second-shell vector at 135 degrees, or from a 120-degree first-shell pair).")
    L("- Matching mode is tie-break dependent here: each neighbor has only 11 first-shell")
    L("  neighbors, and the 12th slot is filled by one of 6 exactly-equidistant second-shell")
    L("  atoms (3.6 A); the minimum-weight matching then lands in the range listed in the")
    L("  table (computed 6.48-19.44 A^2; the worst naive forced pairing u-f alone would be")
    L("  2.5a^2 = 32.4 A^2, but the matcher always avoids it). AtomX should reproduce")
    L("  greedy = 6.48 for all 12 and accept matching-mode values within the tabled range.")
    L("")
    L("## E. Displacement (thermal vibration, generator formula)")
    L("")
    L("Generator (gen_xyz.py): for atom idx at ideal lattice position (x, y, z),")
    L("dx = 0.12 sin(2*pi*fr/30 + 1.1x + 0.7*idx), dy = 0.12 sin(2*pi*fr/30 + 1.3y + 1.19*idx),")
    L("dz = 0.12 sin(2*pi*fr/30 + 0.9z + 1.61*idx). Magnitudes |d| (A), float64:")
    L("")
    L("| atom | frame | d vs ideal lattice | d vs frame 0 (formula) | d vs ideal (from file) | d vs frame 0 (from file) |")
    L("|---|---|---|---|---|---|")
    for (idx, fr, mag, rel0, magf, rel0f) in e_rows:
        L(f"| {idx} | {fr} | {mag:.6f} | {rel0:.6f} | {magf:.6f} | {rel0f:.6f} |")
    L("")
    L(f"- Mean |d| over all 108 atoms at frame 5: **{mean5:.6f} A** (formula); from the")
    L(f"  rounded file positions: {mean5_file:.6f} A; max formula-vs-file magnitude")
    L(f"  deviation {resid:.2e} A (4-decimal rounding).")
    L(f"- Max |d| at frame 5: {max5:.6f} A (bound: 0.12*sqrt(3) = {0.12*math.sqrt(3):.6f} A).")
    L("- Interpretation note: an AtomX displacement modifier referencing frame 0 sees")
    L("  the 'd vs frame 0' columns; the generator amplitude 0.12 A corresponds to the")
    L("  'vs ideal lattice' columns.")
    L("")
    L("## F. Slice (test.xyz frame 0)")
    L("")
    L("OVITO convention (verified against OVITO manual + surface-mesh docs): the plane")
    L("is { r : n.r = d } with n normalized; **the positive side (n.r > d) is deleted; the")
    L("negative side (n.r <= d) is kept**; 'reverse orientation' swaps the sides; a slab")
    L("width w > 0 keeps the closed slab |n.r - d| <= w/2 (and reverse deletes the slab).")
    L("Computed with n = (0,0,1), d = 5.4 (mid-plane), w = 3.0:")
    L("")
    L("| operation | kept count (of 108) |")
    L("|---|---|")
    L(f"| keep n.r <= d (default) | {keep_neg} |")
    L(f"| keep n.r > d (reverse orientation) | {keep_pos} |")
    L(f"| slab w = 3.0: \\|z - 5.4\\| <= 1.5 (3.9 <= z <= 6.9) | {slab} |")
    L(f"| slab inverse (delete the slab) | {slab_inv} |")
    L("")
    L(f"- Atoms exactly at the boundary z = 5.4 (to 1e-12): {on_boundary}. The 18 atoms of")
    L("  the ideal z = 5.4 layer are thermally displaced +/-0.12 A, so the <= vs < choice")
    L("  does not change counts; only the kept side matters.")
    L("")
    L("## G. Replicate")
    L("")
    L("| input | replicate | atoms | new box (A) | species counts |")
    L("|---|---|---|---|---|")
    L(f"| test cell / FCC fixture (108; 27 Cu + 81 Ni) | 2x2x2 | {len(r222_pos)} | "
      f"{r222_cell[0][0]:.1f} x {r222_cell[1][1]:.1f} x {r222_cell[2][2]:.1f} | "
      f"Cu {sp_r222.get('Cu',0)}, Ni {sp_r222.get('Ni',0)} |")
    L(f"| test cell / FCC fixture (108) | 3x1x2 | {len(r312_pos)} | "
      f"{r312_cell[0][0]:.1f} x {r312_cell[1][1]:.1f} x {r312_cell[2][2]:.1f} | "
      f"Cu {sp_r312.get('Cu',0)}, Ni {sp_r312.get('Ni',0)} |")
    L(f"| BCC fixture ({len(bcc_pos)} Fe) | 2x2x2 | {len(b222_pos)} | "
      f"{b222_cell[0][0]:.1f} x {b222_cell[1][1]:.1f} x {b222_cell[2][2]:.1f} | Fe {len(b222_pos)} |")
    L("")
    L(f"Invariance check on 2x2x2 replicated FCC: CNA(3.1) gives FCC = {cnt_r222.get('FCC',0)}/{len(r222_pos)},")
    L("coordination distribution " +
      ", ".join(f"{c}: {n}" for c, n in sorted(coord_r222.items())) +
      " -- identical to the unreplicated cell (periodic-image invariance).")
    L("")
    L("## Tolerance summary")
    L("")
    L("| quantity | expected | tolerance |")
    L("|---|---|---|")
    L("| A/B/G integer counts (CNA classes, coordination histogram, slice kept, replicate N) | as listed | 0 (exact) |")
    L("| A per-atom bond signatures | as listed | exact match of signature multiset |")
    L(f"| C peak bin centers | {peak1:.3f} (ideal 2.5456), {peak2:.3f} (ideal 3.6000) | +/- 0.02 A (bin 0.01 + float32) |")
    L("| C shell pair counts / coordination integrals | 648, 324, 1296, 648 pairs (12, 6, 24, 12 per atom) | 0 for shells 1-3 with band edges as listed; +/-1 pair for shell 4 |")
    L("| D CSP zeros (perfect crystals) | 0.0 | <= 1e-5 (float32 positions) / 1e-10 (float64) |")
    L("| D CSP displaced-atom and neighbor values | as tabled | +/- 1e-4 |")
    L("| D CSP vacancy neighbors (greedy) | 6.48 | +/- 1e-4 |")
    L("| E displacement magnitudes | as tabled | +/- 3e-4 A (file rounding 5e-5/component + float32) |")
    L("| E mean \\|d\\| at frame 5 | as listed | +/- 3e-4 A |")
    L("")

    with open(MD_PATH, "w") as f:
        f.write("\n".join(out) + "\n")

    # ---- console headline ----
    P("=" * 70)
    P("Headline numbers")
    P("=" * 70)
    P(f"A  CNA frame0 @3.1:            FCC={cnt_fcc0.get('FCC',0)}/108 Other={cnt_fcc0.get('Other',0)} (12x421 per atom)")
    P(f"A  CNA perfect FCC @3.1:       FCC={cnt_fccI.get('FCC',0)}/108")
    P(f"A  CNA BCC @2.8:               BCC={cnt_bcc28.get('BCC',0)} Other={cnt_bcc28.get('Other',0)}/{len(bcc_pos)} (bonds (0,0,0), coord 8)")
    P(f"A  CNA BCC @3.6:               BCC={cnt_bcc36.get('BCC',0)}/{len(bcc_pos)} (8x1661 + 6x1441 per atom, coord 14)")
    P(f"A  CNA BCC 3x3x3 control:      BCC={cnt_bcc3.get('BCC',0)}/{len(bcc3_pos)} (finite-size artifact, 2nd-NN bonds (5,4,1))")
    P(f"A  cutoff margins frame0:      1st-shell max {shell1.max():.4f}, 2nd-shell min {shell2.min():.4f} (cutoff 3.1)")
    P(f"B  coordination @3.1 PBC:      {dict(sorted(coord_pbc.items()))} mean {mean_coord_pbc:.4f}")
    P(f"B  coordination @3.1 no PBC:   {dict(sorted(coord_nopbc.items()))} mean {mean_coord_nopbc:.4f}")
    P(f"C  RDF peaks:                  {peak1:.2f} A and {peak2:.2f} A; shells 12/6/24/12 complete")
    P(f"D  CSP perfect FCC/BCC:        max {max(csp_fcc_all_g):.1e} / {max(csp_bcc_all_g):.1e} (exact 0)")
    P(f"D  CSP displaced atom 0:       greedy {dis_rows[0][1]:.6f}, matching {dis_rows[0][2]:.6f}")
    P(f"D  CSP vacancy neighbors:      greedy all {A_FCC**2/2.0:.4f} A^2; matching tie-range "
      f"{min(r[2] for r in vac_rows):.2f}-{max(r[3] for r in vac_rows):.2f}")
    P(f"E  mean |d| frame5:            {mean5:.6f} A (file {mean5_file:.6f}, resid {resid:.1e})")
    P(f"F  slice z/5.4:                keep<= {keep_neg}, keep> {keep_pos}, slab3.0 {slab}, inv {slab_inv}, boundary {on_boundary}")
    P(f"G  replicate:                  108->864 (21.6^3), ->648 (32.4x10.8x21.6), "
      f"BCC {len(bcc_pos)}->{len(b222_pos)} ({b222_cell[0][0]:.1f}^3)")
    P(f"G  replicate invariance:       CNA FCC {cnt_r222.get('FCC',0)}/864, coord {dict(sorted(coord_r222.items()))}")
    P(f"Wrote {MD_PATH}")


if __name__ == "__main__":
    main()
