#!/usr/bin/env python3
"""relatorio_analise.py - figuras e numeros do relatorio M3 (reusa analysis/analyze.py).
Uso: python3 relatorio_analise.py --repo str_m3_esp32 --out saida"""
import argparse, json, os, sys
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("--repo", required=True); ap.add_argument("--out", required=True)
a = ap.parse_args()
sys.path.insert(0, os.path.join(a.repo, "analysis"))
import analyze as A

os.makedirs(a.out, exist_ok=True)
data = {s: np.genfromtxt(os.path.join(a.repo, "data", f"etapa{s}.csv"), delimiter=",",
                         skip_header=1, dtype=np.int64)[:, 1] for s in range(1, 5)}
meta = json.load(open(os.path.join(a.repo, "data", "meta.json")))
D = int(meta["D_us"])
NM = {1: "Etapa 1 – Tarefa", 2: "Etapa 2 – Comportamental", 3: "Etapa 3 – Intertarefas", 4: "Etapa 4 – Sistema"}

BLUE, ORANGE, RED, GRAY, INK = "#2a78d6", "#eb6834", "#e34948", "#52514e", "#0b0b0b"
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 7.5, "axes.titlesize": 8,
                     "axes.spines.top": False, "axes.spines.right": False,
                     "axes.edgecolor": "#8a8984", "axes.labelcolor": GRAY, "xtick.color": GRAY,
                     "ytick.color": GRAY, "axes.grid": True, "grid.color": "#e6e5e1", "grid.linewidth": .5,
                     "legend.frameon": False, "savefig.dpi": 220})
def save(fig, n):
    fig.savefig(os.path.join(a.out, n), bbox_inches="tight"); plt.close(fig)

R = {"D": D, "stats": {}, "mk": {}, "perdas": {}, "conv": {}, "margem": {}, "expD": [], "minD": []}
for s, x in data.items():
    st = A.stats(x, D); idx = A.miss_indices(x, D); dist = A.miss_distances(idx); iso, rj = A.bursts(idx)
    R["stats"][s] = dict(n=len(x), min=st["min"], media=round(st["media"], 1), hwm100=st["hwm100"],
                         hwm99=st["hwm99"], pct=round(st["pct_prazo"], 2), p50=st["p50"],
                         cv=round(st["std"] / st["media"], 4), hwm_med=round(st["hwm100"] / st["p50"], 3))
    R["perdas"][s] = dict(n=int(len(idx)), S=A.skip_factor(idx), iso=int(iso), rajadas=len(rj),
                          maior=max(rj) if rj else (1 if len(idx) else 0), dist=dist[:30].tolist(),
                          primeiras=idx[:30].tolist(), dist_med=float(np.median(dist)) if len(dist) else None)
    R["mk"][s] = {k: A.mk_firm(x, D, k)[0:3] for k in A.KS}
    np.savetxt(os.path.join(a.out, f"perdas_etapa{s}.csv"), idx, fmt="%d", header="ativacao_com_deadline_perdido", comments="")
    cm = np.maximum.accumulate(x)
    R["conv"][s] = {c: int(cm[c - 1]) for c in (10, 20, 100, 1000, 10000) if c <= len(x)}
    R["conv"][s]["saltos"] = [(i + 1, int(v)) for i, v in enumerate(cm) if i == 0 or v > cm[i - 1]]
h1 = R["stats"][1]["hwm100"]
for s in data:
    R["margem"][s] = {m: (int(round(h1 * (1 + m / 100))), bool(R["stats"][s]["hwm100"] <= h1 * (1 + m / 100))) for m in (10, 25, 50)}
    R["margem"][s]["necessaria_pct"] = round(100 * (R["stats"][s]["hwm100"] / h1 - 1), 1)

# Exp. D
d_a, d_c = h1, R["stats"][4]["hwm99"]; d_b = (d_a + d_c) // 2
Ds = [d_a, D, d_b, d_c, R["stats"][4]["hwm100"]]
for s in (3, 4):
    x = data[s]
    for d in Ds:
        idx = A.miss_indices(x, d); iso, rj = A.bursts(idx); mk = A.mk_firm(x, d, 20)
        R["expD"].append(dict(s=s, D=d, pct=round(100 * float(np.mean(x <= d)), 2), n=int(len(idx)),
                              S=A.skip_factor(idx), mk=(mk[0], mk[1]), iso=int(iso), raj=len(rj),
                              maior=max(rj) if rj else (1 if len(idx) else 0)))
for (m, k) in [(10, 20), (14, 20), (18, 20), (19, 20), (20, 20)]:
    d = A.min_D_for_mk(data[4], m, k)
    R["minD"].append(dict(m=m, k=k, D=d, pct_hwm4=round(100 * d / R["stats"][4]["hwm100"], 1),
                          pct_hit=round(100 * float(np.mean(data[4] <= d)), 2)))
json.dump(R, open(os.path.join(a.out, "resultados.json"), "w"), indent=1, default=int)

# ---------- Figuras ----------
# F1 histogramas
fig, axs = plt.subplots(2, 2, figsize=(7.2, 4.4))
for ax, (s, x) in zip(axs.ravel(), data.items()):
    st = R["stats"][s]
    ax.hist(x, bins=70, color=BLUE, linewidth=0); ax.set_yscale("log")
    ax.axvline(st["media"], color=GRAY, ls=":", lw=1, label=f"média {st['media']:.0f}")
    ax.axvline(st["hwm99"], color=GRAY, ls="--", lw=1, label=f"HWM(99%) {st['hwm99']}")
    ax.axvline(st["hwm100"], color=RED, lw=1.3, label=f"HWM(100%) {st['hwm100']}")
    ax.axvline(D, color=ORANGE, ls="-.", lw=1.5, label=f"D = {D}")
    ax.set_title(NM[s], loc="left"); ax.legend(fontsize=6.3, loc={1:"center right",2:"center right",3:"center",4:"upper right"}[s])
    ax.set_xlabel("tempo de resposta (µs)"); ax.set_ylabel("ativações (log)")
fig.tight_layout(); save(fig, "f_hist.png")

# F2 temporal
fig, axs = plt.subplots(2, 2, figsize=(7.2, 4.2))
for ax, (s, x) in zip(axs.ravel(), data.items()):
    sp = A.miss_indices(x, D)
    ax.plot(x, lw=.35, color=BLUE)
    if len(sp): ax.scatter(sp, x[sp], s=1.2, color=RED, zorder=3, label=f"perdas ({len(sp)})", linewidths=0)
    ax.axhline(D, color=ORANGE, ls="-.", lw=1.2, label=f"D = {D}")
    top = np.argsort(x)[-1]; ax.annotate(f"HWM {x[top]}", (top, x[top]), fontsize=6.3, color=RED,
                                         xytext=(4, 2), textcoords="offset points")
    ax.set_title(NM[s], loc="left"); ax.set_xlabel("ativação"); ax.set_ylabel("tempo (µs)")
    ax.legend(fontsize=6.3, loc="upper right")
fig.tight_layout(); save(fig, "f_tempo.png")

# F3 convergencia
fig, ax = plt.subplots(figsize=(3.5, 2.7))
cols = {1: BLUE, 2: "#1baf7a", 3: "#eda100", 4: RED}
for s, x in data.items():
    ax.step(np.arange(1, len(x) + 1), np.maximum.accumulate(x), where="post", color=cols[s], lw=1.4, label=NM[s].replace("Etapa ", "E"))
ax.axhline(D, color=ORANGE, ls="-.", lw=1.2, label=f"D = {D}")
ax.set_xscale("log"); ax.set_xlabel("nº de amostras"); ax.set_ylabel("HWM acumulado (µs)")
ax.legend(fontsize=6, loc="upper left"); fig.tight_layout(); save(fig, "f_conv.png")

# F4 janela deslizante (3 k x 4 etapas)
fig, axs = plt.subplots(3, 4, figsize=(7.2, 4.3), sharex="col")
for j, (s, x) in enumerate(data.items()):
    for i, k in enumerate(A.KS):
        ax = axs[i, j]; w = A.window_misses(x, D, k)
        ax.plot(w, lw=.5, color=BLUE); ax.set_ylim(-.04 * k, k * 1.05)
        ax.axhline(w.max(), color=RED, ls="--", lw=.9)
        ax.text(.98, .06, f"máx {w.max()} → ({k - w.max()},{k})", transform=ax.transAxes, ha="right", fontsize=6.2, color=INK)
        if j == 0: ax.set_ylabel(f"k={k}\nperdas/janela")
        if i == 0: ax.set_title(NM[s].replace("Etapa ", "E"), fontsize=7, loc="left")
        if i == 2: ax.set_xlabel("início da janela")
fig.tight_layout(); save(fig, "f_janela.png")

# F5 distancias entre perdas (etapas 3,4 com D) + janela k=20 da etapa 4 para 3 D
fig, axs = plt.subplots(1, 3, figsize=(7.2, 2.2))
for ax, s in zip(axs[:2], (3, 4)):
    dist = A.miss_distances(A.miss_indices(data[s], D)); mx = int(dist.max())
    ax.bar(*np.unique(dist, return_counts=True), color=BLUE, width=.6)
    ax.set_title(f"{NM[s]}: distâncias (D={D})", fontsize=7, loc="left"); ax.set_xlabel("distância entre perdas"); ax.set_ylabel("ocorrências")
    ax.set_xticks(range(1, mx + 1))
ax = axs[2]
for d, c, ls in [(D, ORANGE, "-"), (d_b, BLUE, "-"), (d_c, "#1baf7a", "-")]:
    w = A.window_misses(data[4], d, 20); ax.plot(w, lw=.5, color=c, label=f"D={d} → máx {w.max()}")
ax.set_title("Etapa 4: perdas em janela k=20", fontsize=7, loc="left"); ax.set_xlabel("início da janela"); ax.set_ylabel("perdas/20")
ax.set_ylim(0, 36); ax.legend(fontsize=5.6, loc="upper left", ncol=1, handlelength=1.2, borderpad=0.1, labelspacing=0.2)
fig.tight_layout(); save(fig, "f_dist.png")
print(json.dumps({k: R[k] for k in ("stats", "mk", "margem", "expD", "minD")}, default=int)[:3000])
