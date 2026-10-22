#!/usr/bin/env python3
import sys
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt


def load(path):
    df = pd.read_csv(path)

    # Время переводим в секунды — так удобнее читать график
    df["t"] = df["t_ms"] / 1000.0

    # Модуль вектора угловой скорости: sqrt(gx^2 + gy^2 + gz^2).
    # Одно число вместо трёх, и, что важнее, оно не зависит от того, как именно датчик повёрнут на руке.
    df["gyro_mag"] = np.sqrt(df.gx**2 + df.gy**2 + df.gz**2)

    # То же для ускорения. Вычитаем 1 g, чтобы убрать постоянную составляющую от гравитации и видеть только то ускорение, которое создаёт движение.
    df["acc_mag"] = np.sqrt(df.ax**2 + df.ay**2 + df.az**2) - 1.0

    return df


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    df = load(path)

    duration = df.t.iloc[-1]
    rate = len(df) / duration
    print(f"{path}")
    print(f"  сэмплов:     {len(df)}")
    print(f"  длительность: {duration:.1f} с")
    print(f"  частота:     {rate:.1f} Гц")
    print(
        f"  gyro_mag:    среднее {df.gyro_mag.mean():.1f}, "
        f"максимум {df.gyro_mag.max():.1f} град/с"
    )

    # Три графика один под другим с общей осью времени:
    # так видно, как события на разных датчиках совпадают по времени
    fig, axes = plt.subplots(3, 1, figsize=(14, 9), sharex=True)

    axes[0].plot(df.t, df.ax, label="ax", linewidth=0.8)
    axes[0].plot(df.t, df.ay, label="ay", linewidth=0.8)
    axes[0].plot(df.t, df.az, label="az", linewidth=0.8)
    axes[0].set_ylabel("ускорение, g")
    axes[0].legend(loc="upper right")
    axes[0].grid(alpha=0.3)

    axes[1].plot(df.t, df.gx, label="gx", linewidth=0.8)
    axes[1].plot(df.t, df.gy, label="gy", linewidth=0.8)
    axes[1].plot(df.t, df.gz, label="gz", linewidth=0.8)
    axes[1].set_ylabel("угл. скорость, град/с")
    axes[1].legend(loc="upper right")
    axes[1].grid(alpha=0.3)

    # Нижний график — главный. Именно по нему на шаге 6 будем искать пики и считать гребки.
    axes[2].plot(df.t, df.gyro_mag, color="crimson", linewidth=1.0)
    axes[2].set_ylabel("|угл. скорость|")
    axes[2].set_xlabel("время, с")
    axes[2].grid(alpha=0.3)

    fig.suptitle(path)
    fig.tight_layout()

    out = path.replace(".csv", ".png")
    fig.savefig(out, dpi=120)
    print(f"  график:      {out}")

    plt.show()


if __name__ == "__main__":
    main()
