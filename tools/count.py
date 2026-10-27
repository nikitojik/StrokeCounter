#!/usr/bin/env python3
"""
Подсчёт гребков: разбивка записи на отрезки и поиск пиков внутри каждого.

Использование:
    python count.py data/rec_0006.csv        — разобрать одну запись
    python count.py --eval                   — прогнать все файлы из labels.csv
    python count.py data/rec_0006.csv --plot — с графиком и отмеченными пиками

Требуется: pip install pandas numpy scipy matplotlib
"""

# Параметры подобраны на 10 размеченных отрезках (rec_0005-0008, брасс,
# 25 ярдов). Результат на этом наборе: 100% в пределах +-1 гребка,
# 50% точно, все отрезки найдены верно.
# ВНИМАНИЕ: выборка мала, возможна переподгонка. Следующая проверка -
# новые данные БЕЗ изменения параметров.    

import sys
import os
import pandas as pd
import numpy as np
from scipy.signal import butter, filtfilt, find_peaks

FS = 50.0                  # частота записи, Гц

# Фильтр низких частот: всё, что быстрее этой частоты, считаем шумом.
# Гребок брассом — это 0.4-0.7 Гц. Берём 2 Гц, чтобы сохранить форму
# пика (его фронты содержат более высокие частоты), но убрать
# дребезг от брызг и мелких рывков.
LOWPASS_HZ = 2.0

# --- разбивка на отрезки ---

# Окно сглаживания для "огибающей активности".
# Длинное: нас интересует не отдельный гребок, а факт "плывёт / отдыхает".
ENVELOPE_WINDOW_S = 2.0

# Порог активности, град/с. Выше — плывёт, ниже — пауза.
ACTIVITY_THRESHOLD = 100.0

# Паузы короче этой склеиваем: внутри отрезка бывают провалы
# (например, фаза проката в брассе), и они не должны его разрывать.
MIN_GAP_S = 2.5

# Отрезки короче этого выбрасываем: это возня на бортике,
# а не проплыв.
MIN_LAP_S = 4.0

# --- поиск пиков ---

# Минимальное расстояние между гребками.
# Физически гребок брассом не бывает чаще, чем раз в ~0.9 с
# даже на соревновательном темпе.
MIN_STROKE_INTERVAL_S = 0.9

# Насколько пик должен выделяться на фоне соседей.
# Берём долю от характерной амплитуды отрезка, а не абсолютное
# число: на быстром темпе пики выше, на спокойном ниже,
# и фиксированный порог работал бы только на одном из них.
PROMINENCE_RATIO = 0.30
MIN_HEIGHT_RATIO = 0.45


#  Обработка сигнала
def load(path):
    df = pd.read_csv(path)
    df["t"] = df["t_ms"] / 1000.0
    df["gyro_mag"] = np.sqrt(df.gx**2 + df.gy**2 + df.gz**2)
    return df


def lowpass(signal, cutoff_hz, fs, order=4):
    """
    Фильтр Баттерворта + filtfilt.

    filtfilt прогоняет фильтр дважды — вперёд и назад по времени.
    Обычный фильтр сдвигает сигнал во времени (задержка), а двойной
    прогон这 компенсирует: пики остаются на своих местах.
    Для нас это критично — мы потом считаем интервалы между ними.
    """
    nyquist = fs / 2
    b, a = butter(order, cutoff_hz / nyquist, btype="low")
    return filtfilt(b, a, signal)


def moving_average(signal, window_samples):
    """Скользящее среднее через свёртку с прямоугольным окном."""
    kernel = np.ones(window_samples) / window_samples
    return np.convolve(signal, kernel, mode="same")


#  Разбивка на отрезки
def find_laps(signal, fs):
    """
    Находим участки, где человек плывёт.

    Логика в три шага:
      1. строим огибающую — насколько активно движение в каждый момент
      2. помечаем всё выше порога как "активность"
      3. склеиваем короткие паузы и выбрасываем короткие куски
    """
    envelope = moving_average(signal, int(ENVELOPE_WINDOW_S * fs))
    active = envelope > ACTIVITY_THRESHOLD

    # Находим границы участков активности.
    # diff по булеву массиву даёт +1 там, где началось,
    # и -1 там, где закончилось.
    edges = np.diff(active.astype(int))
    starts = list(np.where(edges == 1)[0] + 1)
    ends = list(np.where(edges == -1)[0] + 1)

    # Запись могла начаться или закончиться прямо посреди активности
    if active[0]:
        starts.insert(0, 0)
    if active[-1]:
        ends.append(len(active))

    segments = list(zip(starts, ends))

    # Склеиваем сегменты, между которыми слишком короткая пауза
    min_gap = int(MIN_GAP_S * fs)
    merged = []
    for seg in segments:
        if merged and seg[0] - merged[-1][1] < min_gap:
            merged[-1] = (merged[-1][0], seg[1])
        else:
            merged.append(seg)

    # Выбрасываем слишком короткие
    min_len = int(MIN_LAP_S * fs)
    return [s for s in merged if s[1] - s[0] >= min_len]


#  Подсчёт гребков внутри отрезка
def count_strokes(signal, fs):
    """
    Ищем пики. Возвращаем их индексы.

    Два условия отбора:
      distance   — пики не ближе, чем физически возможный интервал
      prominence — пик должен возвышаться над окружением,
                   а не быть мелкой рябью на склоне большого пика
    """
    if len(signal) < int(MIN_STROKE_INTERVAL_S * fs):
        return np.array([])

    # Характерная амплитуда отрезка. 95-й процентиль, а не максимум:
    # максимум может оказаться случайным выбросом и задрать порог.
    scale = np.percentile(signal, 95)
    prominence = PROMINENCE_RATIO * scale

    peaks, _ = find_peaks(
        signal,
        distance=int(MIN_STROKE_INTERVAL_S * fs),
        prominence=prominence,
        height=MIN_HEIGHT_RATIO * scale,
    )
    return peaks

#  Разбор одной записи
def analyze(path, verbose=True):
    df = load(path)
    raw = df.gyro_mag.values
    filtered = lowpass(raw, LOWPASS_HZ, FS)

    laps = find_laps(filtered, FS)

    results = []
    for i, (start, end) in enumerate(laps, 1):
        segment = filtered[start:end]
        peaks = count_strokes(segment, FS)

        duration = (end - start) / FS
        results.append({
            "lap": i,
            "start_s": start / FS,
            "end_s": end / FS,
            "duration_s": duration,
            "strokes": len(peaks),
            "peaks_abs": peaks + start,
        })

        if verbose:
            rate = len(peaks) / duration * 60 if duration > 0 else 0
            print(f"  отрезок {i}: {len(peaks)} гребков, "
                  f"{duration:.1f} с  ({results[-1]['start_s']:.1f}-"
                  f"{results[-1]['end_s']:.1f} с), "
                  f"{rate:.0f} гребков/мин")

    return df, filtered, results


#  Сверка с ручным подсчётом
def evaluate(labels_path="data/labels.csv"):
    labels = pd.read_csv(labels_path)

    total = 0
    exact = 0
    within_one = 0
    lap_count_ok = 0
    files = 0

    for fname, group in labels.groupby("file", sort=True):
        path = os.path.join("data", fname + ".csv")
        if not os.path.exists(path):
            print(f"{fname}: файла нет, пропускаю")
            continue

        files += 1
        _, _, results = analyze(path, verbose=False)

        expected_laps = len(group)
        got_laps = len(results)
        laps_match = (expected_laps == got_laps)
        if laps_match:
            lap_count_ok += 1

        print(f"\n{fname}: отрезков ожидалось {expected_laps}, "
              f"найдено {got_laps}" + ("" if laps_match else "  <-- НЕ СОВПАЛО"))

        truth = group.sort_values("lap").strokes.tolist()
        for i in range(max(len(truth), len(results))):
            t = truth[i] if i < len(truth) else None
            g = results[i]["strokes"] if i < len(results) else None

            if t is None:
                print(f"  отрезок {i+1}: лишний, насчитано {g}")
                continue
            if g is None:
                print(f"  отрезок {i+1}: пропущен, должно быть {t}")
                total += 1
                continue

            diff = g - t
            total += 1
            if diff == 0:
                exact += 1
                mark = "+"
            elif abs(diff) <= 1:
                within_one += 1
                mark = "~"
            else:
                mark = "!"
            print(f"  {mark} отрезок {i+1}: насчитано {g}, "
                  f"вручную {t}  (ошибка {diff:+d})")

    print("\n" + "=" * 50)
    if total:
        print(f"Отрезков всего:        {total}")
        print(f"Точно:                 {exact} ({exact/total*100:.0f}%)")
        print(f"В пределах +-1:        {exact + within_one} "
              f"({(exact+within_one)/total*100:.0f}%)")
        print(f"Файлов с верным числом отрезков: {lap_count_ok}/{files}")
    print("Цель: +-1 гребок в 90% случаев, 100% отрезков найдены")


#  График
def plot(path):
    import matplotlib.pyplot as plt

    df, filtered, results = analyze(path, verbose=False)

    fig, ax = plt.subplots(figsize=(15, 5))

    ax.plot(df.t, df.gyro_mag, color="lightgray", linewidth=0.6,
            label="исходный")
    ax.plot(df.t, filtered, color="crimson", linewidth=1.0,
            label=f"фильтр {LOWPASS_HZ} Гц")

    for r in results:
        # Полупрозрачная заливка — найденный отрезок
        ax.axvspan(r["start_s"], r["end_s"], alpha=0.12, color="steelblue")
        # Точки — найденные гребки
        ax.plot(df.t.values[r["peaks_abs"]], filtered[r["peaks_abs"]],
                "o", color="darkblue", markersize=6)
        ax.text(r["start_s"], ax.get_ylim()[1] * 0.92,
                f"{r['strokes']}", fontsize=13, color="darkblue")

    ax.set_xlabel("время, с")
    ax.set_ylabel("|угл. скорость|, град/с")
    ax.legend(loc="upper right")
    ax.grid(alpha=0.3)
    fig.suptitle(f"{path}  —  цифрами показано число найденных гребков")
    fig.tight_layout()

    out = path.replace(".csv", "_peaks.png")
    fig.savefig(out, dpi=120)
    print(f"график: {out}")
    plt.show()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == "--eval":
        evaluate()
    elif "--plot" in sys.argv:
        plot(sys.argv[1])
    else:
        path = sys.argv[1]
        print(path)
        analyze(path)


if __name__ == "__main__":
    main()