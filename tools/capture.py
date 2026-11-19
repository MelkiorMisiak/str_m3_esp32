#!/usr/bin/env python3
"""
capture.py - captura a saida serial do ESP32 e gera os CSV de cada etapa.

Modos:
  # ao vivo (reinicia a placa via RTS para pegar a execucao desde o inicio)
  python tools/capture.py --port /dev/ttyUSB0 --baud 921600 --out data

  # offline, a partir de um log salvo (ex.: idf.py monitor | tee log.txt)
  python tools/capture.py --log log.txt --out data

Saidas em --out:
  etapa1.csv .. etapa4.csv   colunas: ativacao,tempo_us
  meta.json                  metadados (plataforma, D_us, HWM1, resumos do firmware)
  serial_raw.log             copia bruta da serial (ao vivo)
"""
import argparse
import csv
import json
import os
import re
import sys
import time


def parse_lines(lines, out_dir):
    meta = {"summaries": {}}
    stage = None
    rows = []
    done = False
    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#BEGIN"):
            kv = dict(re.findall(r"(\w+)=(\S+)", line))
            stage = int(kv["stage"])
            meta["D_us"] = int(kv["D_us"])
            rows = []
        elif line.startswith("#END"):
            if stage is not None:
                path = os.path.join(out_dir, f"etapa{stage}.csv")
                with open(path, "w", newline="") as f:
                    w = csv.writer(f)
                    w.writerow(["ativacao", "tempo_us"])
                    w.writerows(rows)
                print(f"[ok] etapa {stage}: {len(rows)} amostras -> {path}")
            stage = None
        elif line.startswith("#META"):
            for k, v in re.findall(r"(\w+)=(\S+)", line):
                meta[k] = v
        elif line.startswith("#SUMMARY"):
            kv = dict(re.findall(r"(\w+)=(\S+)", line))
            meta["summaries"][kv.get("stage", "?")] = kv
        elif line.startswith("#ALLDONE"):
            done = True
        elif line.startswith("#"):
            continue  # #STAGE, #LOAD etc.
        elif stage is not None and line[0].isdigit():
            parts = line.split(",")
            if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
                rows.append((int(parts[0]), int(parts[1])))
    for key in ("D_us", "hwm1_us"):
        if key in meta:
            meta[key] = int(meta[key])
    with open(os.path.join(out_dir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)
    return done


def live(port, baud, out_dir, reset):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("Instale pyserial: pip install pyserial")
    ser = serial.Serial(port, baud, timeout=1)
    if reset:  # sequencia padrao de reset do ESP32 (EN via RTS)
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
    raw_path = os.path.join(out_dir, "serial_raw.log")
    lines = []
    print("Capturando... (Ctrl-C interrompe; termina sozinho em #ALLDONE)")
    print("A etapa 1 leva ~1 min antes do primeiro CSV; as linhas #META aparecem logo no inicio.")
    t_start = time.time()
    last_warn = t_start
    got_hash = False
    nbytes = 0
    with open(raw_path, "w") as raw:
        try:
            while True:
                b = ser.readline()
                now = time.time()
                if not b:
                    if not got_hash and now - last_warn > 10:
                        last_warn = now
                        print(f"[aviso] {int(now - t_start)} s sem nenhuma linha '#'. Bytes recebidos: {nbytes}. "
                              "Se for >0, o baud da placa provavelmente difere de --baud "
                              "(confira CONFIG_ESP_CONSOLE_UART_BAUDRATE no sdkconfig; tente --baud 115200). "
                              "Se for 0, confira a porta/cabo.")
                    continue
                nbytes += len(b)
                s = b.decode("utf-8", errors="replace")
                raw.write(s)
                lines.append(s)
                if s.startswith("#"):
                    got_hash = True
                    if not s.startswith("#LOAD"):
                        print(s.strip())
                if s.startswith("#ALLDONE"):
                    break
        except KeyboardInterrupt:
            print("\nInterrompido; processando o que foi capturado.")
    ser.close()
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", help="processa um log ja salvo em vez de abrir a serial")
    ap.add_argument("--out", default="data")
    ap.add_argument("--no-reset", action="store_true", help="nao reinicia a placa ao abrir a porta")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    if a.log:
        with open(a.log, errors="replace") as f:
            lines = f.readlines()
    elif a.port:
        lines = live(a.port, a.baud, a.out, not a.no_reset)
    else:
        ap.error("informe --port ou --log")

    done = parse_lines(lines, a.out)
    print("Execucao completa (#ALLDONE)." if done else "Aviso: #ALLDONE nao foi visto; dados podem estar incompletos.")


if __name__ == "__main__":
    main()
