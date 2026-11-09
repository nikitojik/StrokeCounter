# Written with assistance from Claude (Anthropic); see README for details
"""
Симулятор потокового алгоритма — точная копия того, что считает плата.

Зачем: прошивку нельзя отладить, не сходив в бассейн. А здесь мы
прогоняем ровно ту же логику по уже записанному сигналу и сразу
видим, где именно она ошибается. Подобрал параметры тут —
перенёс в main.cpp — залил один раз.

Использование:
    python simulate.py data/rec_0028.csv           — разбор записи
    python simulate.py data/rec_0028.csv --plot    — с графиком
    python simulate.py --eval                      — сверка с labels.csv

ВАЖНО: параметры ниже должны совпадать с main.cpp. Если меняешь
здесь — поменяй и там.
"""

import sys
import os
import math
import pandas as pd
import numpy as np

#  Параметры — копия из main.cpp
FS = 50.0

LOWPASS_HZ = 2.0

ENVELOPE_WINDOW_S = 2.0
ENVELOPE_SAMPLES = int(ENVELOPE_WINDOW_S * FS)
ACTIVITY_THRESHOLD = 100.0
MIN_GAP_S = 2.5
MIN_LAP_S = 4.0

MIN_STROKE_INTERVAL_S = 0.9
HEIGHT_RATIO = 0.45
RELEASE_RATIO = 0.60

AMP_TAU_S = 2.5
AMP_MIN = 120.0


#  Биквад — построчный перенос struct Biquad


class Biquad:
    """
    Фильтр второго порядка, обрабатывающий по одному отсчёту.

    В отличие от filtfilt он односторонний: смотрит только в прошлое,
    как и положено на плате. Поэтому вносит задержку ~100-150 мс.
    """

    def __init__(self, cutoff_hz, fs):
        q = 0.70710678
        w0 = 2.0 * math.pi * cutoff_hz / fs
        cs = math.cos(w0)
        sn = math.sin(w0)
        alpha = sn / (2.0 * q)

        a0 = 1.0 + alpha
        self.b0 = ((1.0 - cs) / 2.0) / a0
        self.b1 = (1.0 - cs) / a0
        self.b2 = self.b0
        self.a1 = (-2.0 * cs) / a0
        self.a2 = (1.0 - alpha) / a0

        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def process(self, x):
        y = (
            self.b0 * x
            + self.b1 * self.x1
            + self.b2 * self.x2
            - self.a1 * self.y1
            - self.a2 * self.y2
        )
        self.x2, self.x1 = self.x1, x
        self.y2, self.y1 = self.y1, y
        return y


class Envelope:
    """Скользящее среднее по кольцевому буферу — как на плате."""

    def __init__(self, n):
        self.buf = [0.0] * n
        self.n = n
        self.pos = 0
        self.sum = 0.0

    def process(self, x):
        self.sum -= self.buf[self.pos]
        self.buf[self.pos] = x
        self.sum += x
        self.pos = (self.pos + 1) % self.n
        return self.sum / self.n


# ============================================================
#  Сам алгоритм
# ============================================================


def run_streaming(gyro_mag, t_ms):
    """
    Прогоняет сигнал через потоковый алгоритм.

    Возвращает список отрезков. У каждого — момент начала и конца,
    и, главное, время каждого засчитанного гребка. Именно времена
    показывают, где берётся лишний пик: в начале отрезка (толчок
    от стенки) или в конце (касание и разворот).
    """
    lowpass = Biquad(LOWPASS_HZ, FS)
    envelope = Envelope(ENVELOPE_SAMPLES)

    amp = AMP_MIN
    amp_decay = math.exp(-1.0 / (AMP_TAU_S * FS))

    peak_above = False
    peak_val = 0.0
    peak_t = 0

    in_lap = False
    lap_start_ms = 0
    low_since_ms = 0
    last_stroke_ms = 0
    lap_stroke_times = []

    laps = []
    filtered = np.zeros(len(gyro_mag))
    env_trace = np.zeros(len(gyro_mag))
    amp_trace = np.zeros(len(gyro_mag))

    def finish_lap(end_ms):
        duration_ms = end_ms - lap_start_ms
        if duration_ms < MIN_LAP_S * 1000:
            return

        # Отбрасываем гребки, чья вершина пришлась на время после
        # конца отрезка. Отрезок кончается в момент падения
        # активности, а всё, что детектор поймал позже, — это
        # касание стенки и разворот.
        kept = [t for t in lap_stroke_times if t < end_ms]
        dropped = [t for t in lap_stroke_times if t >= end_ms]

        laps.append(
            {
                "start_ms": lap_start_ms,
                "end_ms": end_ms,
                "strokes": len(kept),
                "times": kept,
                "dropped": dropped,
            }
        )

    for i in range(len(gyro_mag)):
        t = int(t_ms[i])
        f = lowpass.process(float(gyro_mag[i]))
        env = envelope.process(f)

        amp = max(f, amp * amp_decay)
        if amp < AMP_MIN:
            amp = AMP_MIN

        filtered[i] = f
        env_trace[i] = env
        amp_trace[i] = amp

        # --- границы отрезков ---
        if not in_lap:
            if env > ACTIVITY_THRESHOLD:
                in_lap = True
                lap_start_ms = t
                lap_stroke_times = []
                low_since_ms = 0
                peak_above = False
                last_stroke_ms = 0
            continue

        if env <= ACTIVITY_THRESHOLD:
            if low_since_ms == 0:
                low_since_ms = t
            elif t - low_since_ms >= MIN_GAP_S * 1000:
                finish_lap(low_since_ms)
                in_lap = False
                continue
        else:
            low_since_ms = 0

        # --- поиск пиков ---
        enter = HEIGHT_RATIO * amp
        leave = RELEASE_RATIO * enter

        if not peak_above:
            if f > enter:
                peak_above = True
                peak_val = f
                peak_t = t
        else:
            if f > peak_val:
                peak_val = f
                peak_t = t
            if f < leave:
                peak_above = False
                far_enough = (
                    last_stroke_ms == 0
                    or peak_t - last_stroke_ms >= MIN_STROKE_INTERVAL_S * 1000
                )
                if far_enough:
                    lap_stroke_times.append(peak_t)
                    last_stroke_ms = peak_t

    # Запись кончилась посреди отрезка
    if in_lap:
        finish_lap(int(t_ms[-1]))

    return laps, filtered, env_trace, amp_trace


def load(path):
    df = pd.read_csv(path)
    df["gyro_mag"] = np.sqrt(df.gx**2 + df.gy**2 + df.gz**2)
    return df


def analyze(path, verbose=True):
    df = load(path)
    laps, filtered, env, amp = run_streaming(df.gyro_mag.values, df.t_ms.values)

    if verbose:
        print(path)
        for i, lap in enumerate(laps, 1):
            dur = (lap["end_ms"] - lap["start_ms"]) / 1000
            print(
                f"\n  отрезок {i}: {lap['strokes']} гребков, "
                f"{dur:.1f} с  "
                f"({lap['start_ms']/1000:.1f}-{lap['end_ms']/1000:.1f} с)"
            )

            # Времена гребков и промежутки между ними.
            # Аномально короткий или длинный промежуток в начале
            # или в конце выдаёт лишний пик.
            times = lap["times"]
            parts = []
            for j, t in enumerate(times):
                if j == 0:
                    parts.append(f"{t/1000:.1f}")
                else:
                    gap = (t - times[j - 1]) / 1000
                    parts.append(f"{t/1000:.1f} (+{gap:.1f})")
            print("    гребки, с: " + ", ".join(parts))

            if times:
                head = (times[0] - lap["start_ms"]) / 1000
                tail = (lap["end_ms"] - times[-1]) / 1000
                print(
                    f"    от начала до 1-го: {head:.1f} с, "
                    f"от последнего до конца: {tail:.1f} с"
                )

            if lap["dropped"]:
                drops = ", ".join(f"{t/1000:.1f}" for t in lap["dropped"])
                print(f"    отброшено после конца отрезка: {drops} с")

    return df, laps, filtered, env, amp


def evaluate(labels_path="data/labels.csv"):
    labels = pd.read_csv(labels_path)

    total = exact = within_one = 0

    for fname, group in labels.groupby("file", sort=True):
        path = os.path.join("data", fname + ".csv")
        if not os.path.exists(path):
            continue

        _, laps, _, _, _ = analyze(path, verbose=False)
        truth = group.sort_values("lap").strokes.tolist()

        print(f"\n{fname}: отрезков ожидалось {len(truth)}, " f"найдено {len(laps)}")

        for i in range(min(len(truth), len(laps))):
            t = truth[i]
            g = laps[i]["strokes"]
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
            print(
                f"  {mark} отрезок {i+1}: плата {g}, "
                f"вручную {t}  (ошибка {diff:+d})"
            )

    print("\n" + "=" * 50)
    if total:
        print(f"Отрезков:       {total}")
        print(f"Точно:          {exact} ({exact/total*100:.0f}%)")
        print(
            f"В пределах +-1: {exact + within_one} "
            f"({(exact+within_one)/total*100:.0f}%)"
        )


def plot(path):
    import matplotlib.pyplot as plt

    df, laps, filtered, env, amp = analyze(path, verbose=False)
    t = df.t_ms.values / 1000.0

    fig, ax = plt.subplots(figsize=(15, 6))

    ax.plot(t, df.gyro_mag, color="lightgray", linewidth=0.6, label="исходный")
    ax.plot(t, filtered, color="crimson", linewidth=1.0, label="фильтр (поток)")
    ax.plot(t, env, color="seagreen", linewidth=1.2, label="огибающая")
    ax.plot(
        t,
        HEIGHT_RATIO * amp,
        color="orange",
        linewidth=1.0,
        linestyle="--",
        label="порог входа",
    )
    ax.axhline(
        ACTIVITY_THRESHOLD,
        color="seagreen",
        linestyle=":",
        linewidth=1.0,
        label="порог активности",
    )

    for lap in laps:
        ax.axvspan(
            lap["start_ms"] / 1000, lap["end_ms"] / 1000, alpha=0.12, color="steelblue"
        )
        for st in lap["times"]:
            ax.axvline(st / 1000, color="darkblue", alpha=0.5, linewidth=1.2)
        # Отброшенные пики — серым пунктиром, чтобы было видно,
        # что именно алгоритм решил не считать
        for st in lap["dropped"]:
            ax.axvline(
                st / 1000, color="gray", alpha=0.7, linewidth=1.2, linestyle="--"
            )
        ax.text(
            lap["start_ms"] / 1000,
            ax.get_ylim()[1] * 0.92,
            str(lap["strokes"]),
            fontsize=13,
            color="darkblue",
        )

    ax.set_xlabel("время, с")
    ax.set_ylabel("|угл. скорость|, град/с")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.3)
    fig.suptitle(f"{path} — потоковый алгоритм (как на плате)")
    fig.tight_layout()

    out = path.replace(".csv", "_stream.png")
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
        analyze(sys.argv[1])


if __name__ == "__main__":
    main()
