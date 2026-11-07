# StrokeCounter

A wrist-worn device that counts swimming strokes per length of the pool, built from off-the-shelf parts for about $50.

Swimmers use **strokes per length** as a measure of technique efficiency: fewer strokes over the same distance means less drag and better energy conservation. The problem is that counting them while swimming is distracting and unreliable — you are also breathing, pacing and turning — and a coach watching a full lane cannot count for everyone. Commercial devices that do this automatically (Garmin swim watches, Form goggles) cost $150–250 and keep the data inside their own apps.

This device records arm motion during a swim, detects each stroke, splits the session into lengths, and reports the count.

---

## Where the idea came from

I was given a team project in Foundations of Engineering — pick an engineering problem, write a project management report around it. I am a competitive breaststroker, so I proposed a stroke counter.

The assignment explicitly did not require building anything. But once the idea existed, it seemed a waste not to find out whether it actually works, so I ordered the hardware and started building it myself.

**The two things are separate.** The course project is a planning document written by a team of four. This repository is a device I am building alone, on my own time, with my own hardware. They share an origin and nothing else — the design decisions here were driven by what the data turned out to look like, not by what the report says.

I am also submitting this project as my **CS50 final project**.

---

## Status

| Stage | State |
| --- | --- |
| 1. Hardware bring-up | Done |
| 2. Logging IMU data to the device | Done |
| 3. Waterproof housing verification | Done — 3/3 leak tests passed |
| 4. First pool session | Done |
| 5. Labeled dataset | 16 lengths collected |
| 6. Counting algorithm (Python) | Done — see results below |
| 7. Real-time algorithm on the device | In progress |
| 8. Upload to a server | Not started |

### Algorithm results

Validated on **16 labeled lengths** of breaststroke in a 25-yard pool, across easy, mixed and race pace:

- **100%** of lengths counted within ±1 stroke (target was 90%)
- **62%** counted exactly
- **100%** of length boundaries detected correctly

The validation matters more than the numbers: six of those lengths were recorded *after* the parameters were fixed, and the algorithm was run on them **without changing a single constant**. Five of the six were exact. That rules out the obvious failure mode, where parameters are quietly tuned until they fit the data they were tuned on.

The remaining ±1 errors are not necessarily algorithm errors. Manual counting underwater, late in a set, is itself imperfect — part of the disagreement is the reference being wrong, not the detector.

---

## Hardware

| Part | Component | Price |
| --- | --- | --- |
| Controller | M5Stack StickS3 (ESP32-S3, 6-axis IMU, 8 MB flash, 250 mAh) | $21.50 |
| Housing | SwimCell IPX8 waterproof pouch with armband | $26.00 |
| Strap | M5Stack Stick Watch Accessory Kit | $3.95 |
| Insert | Foam, cut to hold the board in a fixed orientation | — |

No custom electronics. The StickS3 was chosen because a single 48 × 24 × 15 mm board carries everything the measurement needs: processor, IMU, storage for a full session, a display for reading the count at the wall, and a battery.

---

## How it works

### The signal

The gyroscope measures **angular velocity** — how fast the board rotates about each axis. A swimming stroke is fundamentally a rotation of the arm, so each stroke produces a burst in the signal. Unlike the accelerometer, the gyroscope does not sense gravity, so its baseline is zero when the arm is still.

The algorithm works with the **magnitude** of the angular velocity vector, √(gx² + gy² + gz²). Reducing three axes to one number makes the signal independent of how the device happens to be oriented on the arm, so a slightly different mounting between sessions does not change the result.

In the water, strokes appear as peaks of 300–1000 °/s against a resting baseline near 0.5 °/s.

### Detection

1. **Filter.** A 2 Hz low-pass filter removes splash noise and case vibration. A breaststroke cycle is 0.4–0.7 Hz, so the stroke passes through untouched.
2. **Split into lengths.** A 2-second moving average gives an activity envelope. Sustained activity is a length; the quiet interval at the wall separates one from the next.
3. **Count strokes.** Within each length, peaks are detected subject to a minimum spacing of 0.9 s — no breaststroke cycle is faster — and a height threshold set as a fraction of *that length's own* amplitude rather than a fixed number, so the same settings work at easy and race pace.

### Storage

2.4 GHz radio is absorbed within centimetres of water, so nothing can be transmitted during a swim. The device is a logger first: IMU samples are written to onboard flash at 50 Hz in a 28-byte binary record, which fits about 58 minutes of continuous recording in the 4.9 MB data partition.

---

## Repository layout

```
├── platformio.ini      PlatformIO config (pioarduino platform, custom partitions)
├── partitions.csv      Flash layout — 3 MB firmware, 4.9 MB data
├── src/
│   └── main.cpp        Firmware: IMU logging + real-time counting
├── tools/
│   ├── download.py     Pull recordings off the device over USB serial
│   ├── plot.py         Plot a recording
│   └── count.py        Counting algorithm and validation against manual counts
└── data/
    ├── labels.csv      Manual stroke counts — the ground truth
    └── rec_*.csv       Recorded sessions
```

---

## Usage

Build and flash:

```
pio run -t upload
```

On the device: button A starts and stops recording. Between lengths the display shows the stroke count for the last completed length.

Pull a session off the device and analyze it:

```
python tools/download.py list
python tools/download.py get rec_0010.bin
python tools/count.py data/rec_0010.csv --plot
```

Validate the algorithm against the manual counts in `data/labels.csv`:

```
python tools/count.py --eval
```

---

## Known limitations

**One wrist sees one arm.** Breaststroke and butterfly move both arms together, so a single wrist sensor captures the complete stroke cycle. Front crawl and backstroke alternate, so the count would need doubling, which assumes symmetric technique. All validation so far is breaststroke.

**Battery.** The 250 mAh cell is small, and the board draws measurable current even when told to sleep. In practice the device needs charging before each session.

**The housing is a pouch, not an enclosure.** It passed leak testing and works, but a printed enclosure sized to the board would mount more rigidly and look less improvised.

---

## What's next

- Port the algorithm to run on the device in real time (in progress)
- Collect a larger dataset, including other strokes
- Upload sessions over Wi-Fi to a server that computes stroke rate, distance per stroke and SWOLF
- A printed enclosure
