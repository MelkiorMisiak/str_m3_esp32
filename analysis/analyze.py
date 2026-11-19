#!/usr/bin/env python3
"""
analyze.py - analises obrigatorias (secoes 6, 7 e 8 do enunciado) a partir dos CSV.

Uso:
  python analysis/analyze.py --data data --out results \
         --cov-line 100 --cov-branch 100 --mk-required 18,20

Entradas : data/etapa{1..4}.csv (ativacao,tempo_us) e data/meta.json (D_us).
Saidas   : results/resultados.md (tabelas), results/tabela_sintese.csv e figuras PNG.
"""
import argparse
import json
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

NOMES = {1: "Tarefa", 2: "Comportamental", 3: "Intertarefas", 4: "Sistema"}
KS = (10, 20, 50)


# ----------------------------------------------------------------------------
# Estatisticas basicas (secao 6.1)
# ----------------------------------------------------------------------------
def hwm_pct(x, drop_frac=0.01):
    """HWM(99%): maximo depois de descartar o 1% maior das medicoes."""
    s = np.sort(x)
    drop = int(np.floor(len(s) * drop_frac))
    return int(s[len(s) - drop - 1])


def stats(x, D):
    return {
        "n": len(x), "min": int(x.min()), "media": float(x.mean()),
        "hwm100": int(x.max()), "hwm99": hwm_pct(x),
        "pct_prazo": 100.0 * float(np.mean(x <= D)),
        "std": float(x.std()), "p50": float(np.median(x)),
    }


# ----------------------------------------------------------------------------
# Perdas de deadline (secao 6.3)
# ----------------------------------------------------------------------------
def miss_indices(x, D):
    """Indices das ativacoes que PERDERAM o deadline (tempo > D)."""
    return np.flatnonzero(x > D)


def miss_distances(idx):
    """Distancias entre perdas consecutivas (1 = ativacoes vizinhas)."""
    return np.diff(idx)


def skip_factor(idx):
    """S = menor distancia entre duas perdas (None se houver < 2 perdas)."""
    d = miss_distances(idx)
    return int(d.min()) if len(d) else None


def bursts(idx):
    """Agrupa perdas consecutivas. Retorna (isoladas, [tamanhos das rajadas >= 2])."""
    if len(idx) == 0:
        return 0, []
    grupos = np.split(idx, np.flatnonzero(np.diff(idx) != 1) + 1)
    tam = [len(g) for g in grupos]
    return sum(1 for t in tam if t == 1), [t for t in tam if t >= 2]


# ----------------------------------------------------------------------------
# Janela deslizante e (m,k)-firme (secao 6.4)
# ----------------------------------------------------------------------------
def window_misses(x, D, k):
    """Perdas em cada janela de k ativacoes consecutivas (None se n < k)."""
    if len(x) < k:
        return None
    return np.convolve((x > D).astype(int), np.ones(k, dtype=int), mode="valid")


def mk_firm(x, D, k):
    """Retorna (m, k, perdas_max): o sistema cumpriu >= m deadlines em TODA janela de k."""
    w = window_misses(x, D, k)
    if w is None:
        return None
    pm = int(w.max())
    return k - pm, k, pm


def fmt_mk(r):
    return "n/a" if r is None else f"({r[0]},{r[1]})"


# ----------------------------------------------------------------------------
# Utilidades de saida
# ----------------------------------------------------------------------------
def md_table(headers, rows):
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out) + "\n"


def f1(v):
    return f"{v:.1f}"


# ----------------------------------------------------------------------------
# Graficos (secoes 6.2 e 6.4, experimento B)
# ----------------------------------------------------------------------------
def fig_hist(s, x, D, st, out):
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.hist(x, bins=80, color="#4C78A8")
    ax.set_yscale("log")  # escala log deixa a cauda visivel
    ax.axvline(st["media"], color="green", ls=":", label=f"media {st['media']:.0f} us")
    ax.axvline(st["hwm99"], color="orange", ls="--", label=f"HWM(99%) {st['hwm99']} us")
    ax.axvline(st["hwm100"], color="red", label=f"HWM(100%) {st['hwm100']} us")
    ax.axvline(D, color="black", ls="-.", label=f"D = {D} us")
    ax.set_xlabel("tempo de resposta (us)"); ax.set_ylabel("ativacoes (escala log)")
    ax.set_title(f"Etapa {s} - {NOMES[s]}: histograma")
    ax.legend(fontsize=8)
    fig.tight_layout(); fig.savefig(os.path.join(out, f"fig_hist_etapa{s}.png"), dpi=130); plt.close(fig)


def fig_time(s, x, D, out):
    fig, ax = plt.subplots(figsize=(9, 3.6))
    ax.plot(np.arange(len(x)), x, lw=0.5, color="#4C78A8")
    sp = miss_indices(x, D)
    ax.scatter(sp, x[sp], s=8, color="red", label=f"perdas ({len(sp)})", zorder=3)
    ax.axhline(D, color="black", ls="-.", label=f"D = {D} us")
    ax.set_xlabel("numero da ativacao"); ax.set_ylabel("tempo de resposta (us)")
    ax.set_title(f"Etapa {s} - {NOMES[s]}: tempo de resposta x ativacao")
    ax.legend(fontsize=8)
    fig.tight_layout(); fig.savefig(os.path.join(out, f"fig_tempo_etapa{s}.png"), dpi=130); plt.close(fig)


def fig_window(s, x, D, out):
    fig, axs = plt.subplots(len(KS), 1, figsize=(9, 7), sharex=True)
    for ax, k in zip(axs, KS):
        w = window_misses(x, D, k)
        if w is None:
            ax.set_title(f"k={k}: amostras insuficientes"); continue
        ax.plot(w, lw=0.7, color="#4C78A8")
        ax.axhline(w.max(), color="red", ls="--")
        ax.set_ylabel("perdas/janela")
        ax.set_title(f"k={k}: max {w.max()} perdas -> {fmt_mk(mk_firm(x, D, k))}-firme", fontsize=9)
    axs[-1].set_xlabel("posicao inicial da janela (ativacao)")
    fig.suptitle(f"Etapa {s} - {NOMES[s]}: deadlines perdidos por janela deslizante (D={D} us)")
    fig.tight_layout(); fig.savefig(os.path.join(out, f"fig_janela_etapa{s}.png"), dpi=130); plt.close(fig)


def fig_convergence(data, D, out):
    fig, ax = plt.subplots(figsize=(8, 4.2))
    for s, x in data.items():
        ax.plot(np.arange(1, len(x) + 1), np.maximum.accumulate(x), label=f"Etapa {s} - {NOMES[s]}")
    ax.axhline(D, color="black", ls="-.", label=f"D = {D} us")
    ax.set_xscale("log")
    ax.set_xlabel("numero de amostras"); ax.set_ylabel("HWM acumulado (us)")
    ax.set_title("Experimento B: convergencia do HWM")
    ax.legend(fontsize=8)
    fig.tight_layout(); fig.savefig(os.path.join(out, "fig_convergencia_hwm.png"), dpi=130); plt.close(fig)


# ----------------------------------------------------------------------------
# Experimento D: menor D que satisfaz um (m,k) exigido
# ----------------------------------------------------------------------------
def min_D_for_mk(x, m, k):
    """Menor D tal que max perdas em qualquer janela de k <= k-m.
    As perdas diminuem monotonicamente com D, entao basta busca binaria
    sobre os valores distintos medidos."""
    cand = np.unique(x)
    lo, hi = 0, len(cand) - 1
    if k - m < 0:
        return None
    while lo < hi:
        mid = (lo + hi) // 2
        r = mk_firm(x, int(cand[mid]), k)
        if r is not None and r[2] <= k - m:
            hi = mid
        else:
            lo = mid + 1
    return int(cand[lo])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="results")
    ap.add_argument("--d-factor", type=float, default=1.2, help="usado so se meta.json nao tiver D_us")
    ap.add_argument("--cov-line", default="n/d", help="cobertura de linha (%%) do experimento A")
    ap.add_argument("--cov-branch", default="n/d", help="cobertura de desvio (%%) do experimento A")
    ap.add_argument("--mk-required", default="18,20", help="(m,k) exigido no experimento D, ex.: 18,20")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    data = {}
    for s in range(1, 5):
        p = os.path.join(a.data, f"etapa{s}.csv")
        if os.path.exists(p):
            data[s] = np.genfromtxt(p, delimiter=",", skip_header=1, dtype=np.int64)[:, 1]
        else:
            print(f"[aviso] {p} nao encontrado; etapa {s} ignorada")
    if 1 not in data:
        raise SystemExit("etapa1.csv e obrigatoria (define D).")

    meta_path = os.path.join(a.data, "meta.json")
    meta = json.load(open(meta_path)) if os.path.exists(meta_path) else {}
    D = int(meta.get("D_us", np.ceil(data[1].max() * a.d_factor)))
    st = {s: stats(x, D) for s, x in data.items()}
    md = [f"# Resultados (D fixo = {D} us)\n"]

    # ---- Tabela-sintese (secao 8) + figuras por etapa ----
    rows, csv_rows = [], []
    for s, x in data.items():
        idx = miss_indices(x, D)
        S = skip_factor(idx)
        mk = {k: fmt_mk(mk_firm(x, D, k)) for k in KS}
        cov = f"{a.cov_line}% / {a.cov_branch}%" if s == 1 else "-"
        row = [f"{s}. {NOMES[s]}", cov, st[s]["min"], f1(st[s]["media"]), st[s]["hwm100"], st[s]["hwm99"],
               f"{st[s]['pct_prazo']:.2f}%", S if S is not None else "n/a", mk[10], mk[20], mk[50]]
        rows.append(row); csv_rows.append(row)
        fig_hist(s, x, D, st[s], a.out); fig_time(s, x, D, a.out); fig_window(s, x, D, a.out)
    hdr = ["Etapa", "Cobertura (linha/desvio)", "Min (us)", "Media (us)", "HWM(100%) (us)", "HWM(99%) (us)",
           "% no prazo", "S", "(m,k) k=10", "(m,k) k=20", "(m,k) k=50"]
    md.append("## Tabela-sintese\n\n" + md_table(hdr, rows))
    with open(os.path.join(a.out, "tabela_sintese.csv"), "w") as f:
        f.write(",".join(hdr) + "\n")
        for r in csv_rows:
            f.write(",".join(str(c) for c in r) + "\n")

    # ---- 6.3 Perdas, distancias, rajadas ----
    md.append("## 6.3 Perdas de deadline (D fixo)\n")
    rows = []
    for s, x in data.items():
        idx = miss_indices(x, D); d = miss_distances(idx); iso, rj = bursts(idx)
        rows.append([s, len(idx), "n/a" if len(d) == 0 else int(d.min()), iso, len(rj), max(rj) if rj else (1 if len(idx) else 0)])
    md.append(md_table(["Etapa", "#perdas", "S", "perdas isoladas", "#rajadas (>=2 seguidas)", "maior sequencia"], rows))
    for s, x in data.items():
        idx = miss_indices(x, D)
        if len(idx):
            d = miss_distances(idx)
            md.append(f"- Etapa {s}: primeiras distancias entre perdas = {d[:25].tolist()}{' ...' if len(d) > 25 else ''}\n")
    md.append("\n")

    # ---- 6.4 (m,k) ----
    md.append("## 6.4 (m,k)-firme por janela deslizante\n")
    rows = []
    for s, x in data.items():
        for k in KS:
            r = mk_firm(x, D, k)
            rows.append([s, k, "n/a" if r is None else r[2], fmt_mk(r)])
    md.append(md_table(["Etapa", "k", "perdas maximas na janela", "(m,k)-firme"], rows))
    md.append("> O (m,k) acima descreve o comportamento OBSERVADO nas medicoes. Nao e garantia formal: "
              "uma janela pior pode existir em cenarios de interferencia nao exercitados.\n\n")

    # ---- Experimento A ----
    md.append("## Experimento A - cobertura x HWM\n")
    md.append(f"Cobertura da etapa 1 (gcov, no PC): linha {a.cov_line}%, desvio {a.cov_branch}%.\n\n")
    md.append(md_table(["Etapa", "HWM(100%) us", "razao vs etapa 1"],
                       [[s, st[s]["hwm100"], f"{st[s]['hwm100'] / st[1]['hwm100']:.2f}x"] for s in data]))

    # ---- Experimento B ----
    md.append("## Experimento B - convergencia do HWM\n")
    cps = [10, 20, 100, 1000, 10000]
    rows = []
    for s, x in data.items():
        cm = np.maximum.accumulate(x)
        vals = [int(cm[c - 1]) if c <= len(x) else "-" for c in cps]
        last = int(np.flatnonzero(np.diff(cm) > 0).max() + 1) + 1 if np.any(np.diff(cm) > 0) else 1
        rows.append([s] + vals + [last, f"{100.0 * (cm[-1] - cm[min(99, len(x) - 1)]) / cm[-1]:.1f}%"])
    md.append(md_table(["Etapa"] + [f"HWM@{c}" for c in cps] + ["amostra do ultimo salto",
                                                                   "% do HWM final surgido apos a amostra 100"], rows))
    fig_convergence(data, D, a.out)

    # ---- Experimento C ----
    md.append("## Experimento C - margem de seguranca sobre o HWM da etapa 1\n")
    h1 = st[1]["hwm100"]
    rows = []
    for s in data:
        row = [s, st[s]["hwm100"]]
        for mg in (0.10, 0.25, 0.50):
            lim = h1 * (1 + mg)
            row.append(f"{lim:.0f} us -> {'OK' if st[s]['hwm100'] <= lim else 'INSUFICIENTE'}")
        rows.append(row)
    md.append(md_table(["Etapa", "HWM(100%) us", "margem 10%", "margem 25%", "margem 50%"], rows))
    md.append("Variabilidade (desvio-padrao / media e HWM / mediana) por etapa:\n\n")
    md.append(md_table(["Etapa", "desvio/media", "HWM/mediana"],
                       [[s, f"{st[s]['std'] / st[s]['media']:.3f}", f"{st[s]['hwm100'] / st[s]['p50']:.2f}"] for s in data]))

    # ---- Experimento D ----
    md.append("## Experimento D - diferentes deadlines (etapas 3 e 4)\n")
    last = max(data)
    d_a = st[1]["hwm100"]
    d_c = st[last]["hwm99"]
    d_b = int((d_a + d_c) / 2)
    ds = sorted({d_a, d_b, d_c})
    rows = []
    for s in (3, 4):
        if s not in data:
            continue
        x = data[s]
        for d in ds:
            idx = miss_indices(x, d); S = skip_factor(idx); iso, rj = bursts(idx)
            rows.append([s, d, f"{100.0 * np.mean(x <= d):.2f}%", len(idx), S if S is not None else "n/a",
                         fmt_mk(mk_firm(x, d, 20)), iso, len(rj), max(rj) if rj else (1 if len(idx) else 0)])
    md.append(md_table(["Etapa", "D (us)", "% no prazo", "#perdas", "S", "(m,k) k=20",
                        "isoladas", "#rajadas", "maior rajada"], rows))
    m_req, k_req = (int(v) for v in a.mk_required.split(","))
    if 4 in data:
        dmin = min_D_for_mk(data[4], m_req, k_req)
        md.append(f"Menor D que torna a etapa 4 ({m_req},{k_req})-firme: **{dmin} us** "
                  f"(D fixo = {D} us; HWM(100%) da etapa 4 = {st[4]['hwm100']} us; "
                  f"{100.0 * dmin / st[4]['hwm100']:.1f}% do HWM da etapa 4).\n\n")

    with open(os.path.join(a.out, "resultados.md"), "w") as f:
        f.write("\n".join(md))
    print("\n".join(md))
    print(f"\nFiguras e tabelas gravadas em {a.out}/")


if __name__ == "__main__":
    main()
