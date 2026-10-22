#!/usr/bin/env python3
"""
Выгрузка записей с платы на компьютер.

Использование:
    python download.py list                 — показать файлы на плате
    python download.py get rec_0001.bin     — скачать один файл в CSV
    python download.py all                  — скачать все файлы

Требуется: pip install pyserial
"""

import sys
import time
import glob
import serial

BAUD = 115200
OUT_DIR = "data"


def find_port():
    """Ищем порт платы. На macOS это /dev/cu.usbmodem*"""
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        print("Плата не найдена. Подключена ли она?")
        sys.exit(1)
    if len(ports) > 1:
        print(f"Найдено несколько портов: {ports}, беру первый")
    return ports[0]


def open_board():
    port = find_port()
    print(f"Порт: {port}")
    ser = serial.Serial(port, BAUD, timeout=2)
    # Плата перезагружается при открытии порта — ждём, пока поднимется
    time.sleep(2)
    ser.reset_input_buffer()
    return ser


def send(ser, cmd):
    ser.write((cmd + "\n").encode())
    ser.flush()


def read_until(ser, marker, timeout=60):
    """Читаем строки, пока не встретим маркер или не выйдет время."""
    lines = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").strip()
        if line == marker:
            return lines
        lines.append(line)
    raise TimeoutError(f"Не дождались маркера {marker}")


def cmd_list(ser):
    send(ser, "list")
    for line in read_until(ser, "--- end ---"):
        print(line)


def cmd_get(ser, name):
    import os
    os.makedirs(OUT_DIR, exist_ok=True)

    send(ser, f"dump {name}")

    # Ждём BEGIN, всё до него — мусор
    read_until(ser, "BEGIN", timeout=10)
    rows = read_until(ser, "END", timeout=300)

    out_path = os.path.join(OUT_DIR, name.replace(".bin", ".csv"))
    with open(out_path, "w") as f:
        f.write("\n".join(rows) + "\n")

    # Первая строка — заголовок, поэтому вычитаем единицу
    print(f"{out_path}: {len(rows) - 1} сэмплов")


def cmd_all(ser):
    send(ser, "list")
    lines = read_until(ser, "--- end ---")
    names = [l.split()[0] for l in lines if l.endswith(".bin") is False and ".bin" in l]
    if not names:
        print("На плате нет файлов")
        return
    for name in names:
        cmd_get(ser, name)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    ser = open_board()
    try:
        action = sys.argv[1]
        if action == "list":
            cmd_list(ser)
        elif action == "get":
            cmd_get(ser, sys.argv[2])
        elif action == "all":
            cmd_all(ser)
        else:
            print(__doc__)
    finally:
        ser.close()


if __name__ == "__main__":
    main()