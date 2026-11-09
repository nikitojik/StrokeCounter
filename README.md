# StrokeCounter

#### Video Demo: `<ВСТАВЬ ССЫЛКУ НА YOUTUBE>`

#### Description

StrokeCounter is a wrist-worn device that automatically counts swimming strokes on each length of the pool. It is built from off-the-shelf parts for about $50 and runs a stroke-detection algorithm in real time on the device itself, with no phone and no connection to anything while you swim.

Swimmers use **strokes per length** as a measure of technique efficiency: covering the same distance in fewer strokes means less drag and better energy conservation, which is why coaches ask swimmers to count. The problem is that counting while swimming is unreliable — you are simultaneously breathing, pacing yourself and turning at the wall, and the count falls apart exactly when it matters most, late in a hard set when technique is degrading. A coach cannot do it either: with a full lane in the water, they can follow one swimmer at a time. Devices that automate this exist — Garmin swim watches, Form smart goggles — but they cost $150–250 and keep the data locked inside the manufacturer's own app.

I am a competitive breaststroker, so this is a problem I have personally. This device records arm motion during a swim, detects each stroke, splits the session into lengths, and shows the count on its display at the wall.

## Where the idea came from

The idea started in a different class. I was given a team project in Foundations of Engineering: pick an engineering problem and write a project management report around it. I proposed a stroke counter because I swim.

That assignment explicitly did not require building anything. But once the idea existed it seemed a waste not to find out whether it actually works, so I ordered hardware and built it myself. **The two are separate projects.** The course assignment is a planning document written by a team of four; this repository is a device I built alone, on my own time, with my own hardware. They share an origin and nothing else — every design decision here was driven by what the recorded signal turned out to look like, not by what the report says.

## How it works

The board carries a 6-axis IMU: a 3-axis accelerometer measuring linear acceleration and a 3-axis gyroscope measuring angular velocity. The algorithm uses the gyroscope, because a swimming stroke is fundamentally a **rotation of the arm**, and because the gyroscope — unlike the accelerometer — does not sense gravity, so its baseline is zero when the arm is still. Measured in the water, strokes appear as peaks of 300–1000 °/s against a resting baseline near 0.5 °/s.

Detection runs in three stages. A 2 Hz low-pass filter removes splash noise and case vibration while leaving the stroke itself untouched (a breaststroke cycle is 0.4–0.7 Hz). A 2-second moving average produces an "activity envelope" that separates swimming from the rest interval at the wall, which is how the session is divided into lengths. Within each length, individual strokes are found as peaks subject to two conditions: a minimum spacing of 0.9 s, since no breaststroke cycle is faster than that, and a height threshold set as a fraction of *that length's own* amplitude rather than a fixed number, so the same settings work at easy pace and at race pace.

Nothing is transmitted during the swim: 2.4 GHz radio is absorbed within centimetres of water. The device is a logger first, writing IMU samples to onboard flash at 50 Hz.

## Files

**`src/main.cpp`** — the firmware. Reads the IMU at a precisely held 50 Hz, writes raw samples to flash in a 28-byte binary record, and runs the counting algorithm in real time. It contains the streaming versions of every processing stage: a biquad low-pass filter, a circular-buffer envelope, a leaky peak-hold amplitude estimator, and a two-state machine that detects stroke peaks with hysteresis. It also drives the display, handles the buttons, and implements serial commands for listing, dumping and deleting recordings.

**`platformio.ini`** — build configuration. Uses the pioarduino fork of the ESP32 platform, because the board is new enough that the official PlatformIO platform has no definition for it.

**`partitions.csv`** — a custom flash layout. The stock layout reserves two application slots for over-the-air updates and leaves very little room for files; since this device is flashed over USB, one slot is enough, and the freed space gives 4.9 MB for data — about 58 minutes of continuous recording.

**`tools/download.py`** — pulls recordings off the device over USB serial and converts the binary log into CSV.

**`tools/plot.py`** — plots a recording: raw accelerometer and gyroscope axes, and the angular-velocity magnitude the algorithm actually works with.

**`tools/count.py`** — the offline reference implementation of the algorithm in Python, plus `--eval`, which runs it over every recording and compares the result against the manual counts in `data/labels.csv`.

**`tools/simulate.py`** — a faithful port of the *on-device* algorithm to Python. This is the most useful tool in the project; see below.

**`data/labels.csv`** — the ground truth: strokes counted aloud and written down at the wall after each length, with the pace of each length.

**`data/rec_*.csv`** — recorded sessions.

## Design decisions

**Magnitude instead of individual axes.** The algorithm works with √(gx² + gy² + gz²) rather than one axis. Collapsing three axes into one number makes the signal independent of how the device happens to sit on the arm, so a slightly different mounting between sessions does not change the result. An early version used a single axis, which looked cleaner on dry-land test data but would have required mounting the board identically every time.

**Binary on the device, CSV at export.** One record is exactly 28 bytes, versus 50–60 as text, and writing it costs no number formatting on the microcontroller. Samples are buffered in RAM and flushed in 2 KB blocks, because flash writes are slow and doing one per sample would starve the sampler.

**Thresholds relative to the signal, not absolute.** Peak height is judged against a running estimate of the current amplitude. A fixed threshold would work at one pace and fail at the other; a relative one adapts.

**A simulator instead of pool trips.** The on-device algorithm cannot use the same tools as the offline one: `filtfilt` runs a filter forwards and backwards over the whole recording, and a percentile needs the whole length in memory. Neither exists when samples arrive one at a time. Rewriting these as streaming equivalents introduced bugs that only showed up in the water, and each debugging cycle cost a trip to the pool. So I ported the streaming algorithm back to Python, line for line, and ran it over recordings I already had. That turned a multi-day loop into seconds and found the real bug immediately: strokes detected *after* a length had ended — the swimmer touching the wall and turning — were being added to that length's count. The fix is now in the firmware.

**Validation on data the algorithm had not seen.** After the parameters were fixed, I recorded new lengths and ran the algorithm on them **without changing a single constant**. This matters more than the accuracy figure itself: tuning parameters until they fit the data they were tuned on proves nothing.

## Results

On 24 labeled lengths of breaststroke in a 25-yard pool, across easy, moderate and race pace, the on-device algorithm counts **within ±1 stroke on 96%** of lengths and **exactly on 50%**, and finds every length boundary correctly. The offline Python version is exact on about 70%.

Some of the remaining ±1 disagreements are not algorithm errors. Counting strokes in your head underwater at race pace is itself imperfect, and on several lengths the stroke timing recovered from the signal is more regular than my own count implies. Resolving this would need video ground truth, which the project does not have yet.

## Known limitations

**One wrist sees one arm.** Breaststroke and butterfly move both arms together, so a single wrist sensor captures the complete stroke cycle. Front crawl and backstroke alternate, so the count would need doubling, which assumes symmetric technique. All validation so far is breaststroke.

**Lengths, not turns.** The device segments a session by activity, so it detects *blocks of continuous swimming*, not individual lengths within them. This is correct for the repeats it was tested on, but a continuous 100 would be reported as one block rather than four lengths. Turn detection is the next piece of work.

**Battery.** The 250 mAh cell is small and the device needs charging before each session.

**The housing is a pouch, not an enclosure.** It passed leak testing and works, but a printed enclosure sized to the board would mount more rigidly.

## Use of AI

I used Claude (Anthropic) throughout this project, which CS50 permits for the final project. It helped me figure out the necessary libraries, and I asked it to explain each piece rather than just hand me files with code. The hardware selection, the physical build, all data collection and testing, and the direction of the work were mine, as was the domain knowledge the algorithm depends on — which stroke to start with, what stroke rates are physically plausible, and how to design the pool sessions so the data would be useful. Individual source files carry a note where AI assistance was substantial.

## Hardware

| Part | Component | Price |
| --- | --- | --- |
| Controller | M5Stack StickS3 (ESP32-S3, 6-axis IMU, 8 MB flash, 250 mAh) | $21.50 |
| Housing | SwimCell IPX8 waterproof pouch with armband | $26.00 |
| Strap | M5Stack Stick Watch Accessory Kit | $3.95 |
| Insert | Foam, cut to hold the board in a fixed orientation | — |

No custom electronics. The StickS3 was chosen because one 48 × 24 × 15 mm board carries everything the measurement needs: processor, IMU, storage for a full session, a display for reading the count at the wall, and a battery.

## Usage

Build and flash:

```
pio run -t upload
```

On the device: button A starts and stops recording; holding button B powers it off. Between lengths the display shows the stroke count for the last completed length.

Pull a session off the device and analyze it:

```
python tools/download.py list
python tools/download.py get rec_0028.bin
python tools/count.py data/rec_0028.csv --plot
```

Validate against the manual counts, offline algorithm and on-device algorithm respectively:

```
python tools/count.py --eval
python tools/simulate.py --eval
```

## What's next

- Turn detection, so continuous swims are split into lengths
- Uploading sessions to a server that computes stroke rate, distance per stroke and SWOLF
- A larger dataset, including other strokes
- A printed enclosure