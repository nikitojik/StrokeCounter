// Written with assistance from Claude (Anthropic); see README for details
#include <LittleFS.h>
#include <M5Unified.h>
#include <math.h>

//  Настройки записи
const uint32_t SAMPLE_INTERVAL_US = 20000;  // 20 000 мкс = 50 Гц
const float FS = 50.0f;                     // та же частота, числом

const uint32_t SCREEN_INTERVAL_MS = 250;
const size_t BUFFER_SIZE = 2016;
const uint32_t SLEEP_HOLD_MS = 1500;

//  Параметры алгоритма
//  Те же значения, что в tools/count.py — чтобы можно было сравнивать плату с Python.

const float LOWPASS_HZ = 2.0f;

const float ENVELOPE_WINDOW_S = 2.0f;
const int ENVELOPE_SAMPLES = (int)(ENVELOPE_WINDOW_S * FS);  // 100
const float ACTIVITY_THRESHOLD = 100.0f;
const float MIN_GAP_S = 2.5f;
const float MIN_LAP_S = 4.0f;

// Минимальный интервал между гребками.
//
// Физически гребок брассом не бывает чаще, чем раз в ~0.9 с даже на соревновательном темпе: на замерах самая высокая частота дала 1.3 с между гребками, так что запас есть.
// Пробовали 0.6 — на спокойном темпе длинный цикл с фазой проката начинал дробиться надвое и давал лишние гребки.
const float MIN_STROKE_INTERVAL_S = 0.9f;
const float HEIGHT_RATIO = 0.45f;

// Гистерезис: пик считается завершённым, когда сигнал упал ниже этой доли от порога срабатывания. Без гистерезиса дрожание сигнала у порога дало бы кучу ложных пиков.
const float RELEASE_RATIO = 0.60f;

// Постоянная времени для бегущей оценки амплитуды.
// Заменяет 95-й процентиль из Python, который требует видеть весь отрезок целиком.
//
// Было 5 с, но при таком медленном спаде один высокий пик
// задирал порог на несколько секунд вперёд, и следующие
// гребки до него не доставали (rec_0026: 5 вместо 9).
const float AMP_TAU_S = 2.5f;

// Нижняя граница амплитуды. Не даёт порогу уползти в ноль на паузе, когда сигнала почти нет.
const float AMP_MIN = 120.0f;

//  Формат записи сырого сигнала
#pragma pack(push, 1)
struct Sample {
  uint32_t t_ms;
  float ax, ay, az;
  float gx, gy, gz;
};
#pragma pack(pop)

static_assert(sizeof(Sample) == 28, "Sample must be exactly 28 bytes");

//  Фильтр низких частот (биквад)

// Односторонний фильтр второго порядка.
//
// В Python мы использовали filtfilt — прогон вперёд и назад,
// который не сдвигает сигнал во времени. На плате будущего нет, поэтому фильтр может быть только односторонним, и он вносит
// задержку порядка 100-150 мс. Для подсчёта гребков это не важно: нас интересует их количество, а не точное время.
struct Biquad {
  float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  // Коэффициенты считаем один раз при старте.
  // Формулы стандартные (RBJ cookbook), Q = 1/sqrt(2)
  // даёт характеристику Баттерворта — максимально плоскую в полосе пропускания.
  void design(float cutoff_hz, float fs) {
    const float q = 0.70710678f;
    float w0 = 2.0f * (float)M_PI * cutoff_hz / fs;
    float cs = cosf(w0);
    float sn = sinf(w0);
    float alpha = sn / (2.0f * q);

    float a0 = 1.0f + alpha;
    b0 = ((1.0f - cs) / 2.0f) / a0;
    b1 = (1.0f - cs) / a0;
    b2 = b0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
  }

  float process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }

  void reset() { x1 = x2 = y1 = y2 = 0; }
};

Biquad lowpass;

//  Огибающая активности

// Скользящее среднее по кольцевому буферу.
// Храним сумму и обновляем её при каждом сэмпле, а не пересчитываем заново — иначе на каждый сэмпл была бы сотня сложений.
struct Envelope {
  float buf[ENVELOPE_SAMPLES] = {0};
  int pos = 0;
  float sum = 0;

  float process(float x) {
    sum -= buf[pos];
    buf[pos] = x;
    sum += x;
    pos = (pos + 1) % ENVELOPE_SAMPLES;
    return sum / ENVELOPE_SAMPLES;
  }

  void reset() {
    for (int i = 0; i < ENVELOPE_SAMPLES; i++) buf[i] = 0;
    pos = 0;
    sum = 0;
  }
};

Envelope envelope;

//  Состояние алгоритма

float amp = AMP_MIN;  // бегущая оценка амплитуды
float amp_decay = 0;  // считается в setup по AMP_TAU_S

bool peak_above = false;      // сейчас внутри пика?
float peak_val = 0;           // максимум текущего пика
uint32_t peak_t = 0;          // когда он был
uint32_t last_stroke_ms = 0;  // время последнего засчитанного гребка

bool in_lap = false;
uint32_t lap_start_ms = 0;
uint32_t low_since_ms = 0;  // когда огибающая ушла ниже порога (0 = не ушла)
uint32_t lap_strokes = 0;

// Гребки, засчитанные уже ПОСЛЕ падения активности.
//
// Отрезок закрывается не сразу: после падения огибающей мы ждём
// MIN_GAP_S, чтобы отличить настоящую паузу от провала внутри
// заплыва. Всё это время детектор продолжает работать, и касание
// стенки с разворотом попадает в счётчик — отсюда брался лишний
// гребок на каждом отрезке. Считаем их отдельно и вычитаем, если пауза подтвердилась.
uint32_t strokes_after_low = 0;

uint32_t lap_count = 0;  // отрезков за сессию
uint32_t last_lap_strokes = 0;
uint32_t total_strokes = 0;

//  Состояние записи

bool recording = false;
File rec_file;
File lap_file;
char rec_name[32] = "";

uint8_t buffer[BUFFER_SIZE];
size_t buffer_used = 0;

uint32_t next_sample_us = 0;
uint32_t rec_start_ms = 0;
uint32_t sample_total = 0;
uint32_t dropped = 0;

uint32_t last_screen_ms = 0;

//  Файлы
void makeNextName(char* out, size_t out_size) {
  for (int i = 1; i < 10000; i++) {
    snprintf(out, out_size, "/rec_%04d.bin", i);
    if (!LittleFS.exists(out)) return;
  }
  snprintf(out, out_size, "/rec_overflow.bin");
}

bool flushBuffer() {
  if (buffer_used == 0) return true;
  size_t written = rec_file.write(buffer, buffer_used);
  bool ok = (written == buffer_used);
  if (!ok) dropped++;
  buffer_used = 0;
  return ok;
}

// Сбрасываем алгоритм в исходное состояние.
// Нужно при старте каждой сессии, иначе остатки от прошлой записи повлияют на первые секунды новой.
void resetAlgorithm() {
  lowpass.reset();
  envelope.reset();
  amp = AMP_MIN;
  peak_above = false;
  peak_val = 0;
  peak_t = 0;
  last_stroke_ms = 0;
  in_lap = false;
  lap_start_ms = 0;
  low_since_ms = 0;
  lap_strokes = 0;
  strokes_after_low = 0;
  lap_count = 0;
  last_lap_strokes = 0;
  total_strokes = 0;
}

void startRecording() {
  makeNextName(rec_name, sizeof(rec_name));

  rec_file = LittleFS.open(rec_name, FILE_WRITE);
  if (!rec_file) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setCursor(0, 0);
    M5.Display.println("OPEN FAILED");
    delay(2000);
    return;
  }

  // Рядом с сырым сигналом кладём текстовый файл с итогами по отрезкам: /rec_0007.bin -> /lap_0007.csv
  char lap_name[32];
  snprintf(lap_name, sizeof(lap_name), "/lap_%s", rec_name + 5);
  char* dot = strstr(lap_name, ".bin");
  if (dot) strcpy(dot, ".csv");

  lap_file = LittleFS.open(lap_name, FILE_WRITE);
  if (lap_file) {
    lap_file.println("lap,strokes,duration_s");
  }

  buffer_used = 0;
  sample_total = 0;
  dropped = 0;
  rec_start_ms = millis();
  next_sample_us = micros();
  resetAlgorithm();
  recording = true;

  M5.Display.fillScreen(TFT_BLACK);
}

void finishLap(uint32_t end_ms) {
  uint32_t duration_ms = end_ms - lap_start_ms;
  if (duration_ms < (uint32_t)(MIN_LAP_S * 1000)) {
    return;  // слишком короткий — это возня на бортике
  }

  // Отбрасываем гребки, случившиеся после конца отрезка.
  // Отрезок заканчивается в момент падения активности, а всё,
  // что детектор поймал позже, относится к стенке и развороту.
  uint32_t counted = lap_strokes;
  if (strokes_after_low <= counted) {
    counted -= strokes_after_low;
  }

  lap_count++;
  last_lap_strokes = counted;
  total_strokes += counted;

  if (lap_file) {
    lap_file.printf("%lu,%lu,%.1f\n", (unsigned long)lap_count,
                    (unsigned long)counted, duration_ms / 1000.0);
    lap_file.flush();
  }
}

// Если запись закончилась посреди отрезка — закрываем его
void stopRecording() {
  if (in_lap) {
    finishLap(millis() - rec_start_ms);
    in_lap = false;
  }

  flushBuffer();
  rec_file.close();
  if (lap_file) lap_file.close();
  recording = false;

  M5.Display.fillScreen(TFT_BLACK);
}

//  Алгоритм: один сэмпл на вход

void processSample(float gyro_mag, uint32_t t_ms) {
  float f = lowpass.process(gyro_mag);
  float env = envelope.process(f);

  // бегущая амплитуда
  // Пиковый детектор с утечкой: держим недавний максимум,
  // который медленно спадает. Это потоковая замена 95-му процентилю из Python-версии.
  amp = fmaxf(f, amp * amp_decay);
  if (amp < AMP_MIN) amp = AMP_MIN;

  // границы отрезков
  if (!in_lap) {
    if (env > ACTIVITY_THRESHOLD) {
      in_lap = true;
      lap_start_ms = t_ms;
      lap_strokes = 0;
      strokes_after_low = 0;
      low_since_ms = 0;
      peak_above = false;
      last_stroke_ms = 0;
    }
    return;  // вне отрезка гребки не считаем
  }

  if (env <= ACTIVITY_THRESHOLD) {
    if (low_since_ms == 0) {
      low_since_ms = t_ms;  // активность только что упала
    } else if (t_ms - low_since_ms >= (uint32_t)(MIN_GAP_S * 1000)) {
      // Тишина держится достаточно долго — отрезок закончился.
      // Концом считаем момент, когда активность упала, а не текущий: пауза в отрезок не входит.
      finishLap(low_since_ms);
      in_lap = false;
      return;
    }
  } else {
    // Активность вернулась — значит это был провал внутри заплыва, а не пауза. Гребки, посчитанные за это время, настоящие, поэтому счётчик обнуляем вместе с меткой.
    low_since_ms = 0;
    strokes_after_low = 0;
  }

  // поиск пиков
  // Автомат на два состояния вместо find_peaks, которому
  // нужен весь массив сразу.
  float enter = HEIGHT_RATIO * amp;
  float leave = RELEASE_RATIO * enter;

  if (!peak_above) {
    if (f > enter) {
      peak_above = true;
      peak_val = f;
      peak_t = t_ms;
    }
  } else {
    if (f > peak_val) {
      peak_val = f;  // вершина пика пока растёт
      peak_t = t_ms;
    }
    if (f < leave) {
      peak_above = false;
      // Гребок засчитываем в момент спада, но датируем вершиной — так интервалы между гребками считаются корректно.
      bool far_enough =
          (last_stroke_ms == 0) ||
          (peak_t - last_stroke_ms >= (uint32_t)(MIN_STROKE_INTERVAL_S * 1000));
      if (far_enough) {
        lap_strokes++;
        last_stroke_ms = peak_t;

        // Вершина пика пришлась на время после падения активности — гребок под вопросом. Окончательно решим, когда станет ясно, пауза это или провал внутри отрезка.
        //
        // Сравниваем именно peak_t, а не текущий момент:
        // пик мог начаться до падения активности, а засчитаться после, и такой гребок настоящий.
        if (low_since_ms != 0 && peak_t >= low_since_ms) {
          strokes_after_low++;
        }
      }
    }
  }
}

//  Команды через Serial
void cmdList() {
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  Serial.println("--- files ---");
  while (f) {
    Serial.printf("%s  %u bytes\n", f.name(), (unsigned)f.size());
    f = root.openNextFile();
  }
  Serial.println("--- end ---");
}

void cmdDump(const char* name) {
  File f = LittleFS.open(name, FILE_READ);
  if (!f) {
    Serial.println("ERR: no such file");
    return;
  }

  Serial.println("BEGIN");

  // Текстовые файлы (итоги по отрезкам) отдаём как есть, бинарные разбираем в CSV
  if (strstr(name, ".bin") == nullptr) {
    while (f.available()) {
      Serial.write(f.read());
    }
  } else {
    Serial.println("t_ms,ax,ay,az,gx,gy,gz");
    Sample s;
    while (f.read((uint8_t*)&s, sizeof(s)) == sizeof(s)) {
      Serial.printf("%lu,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f\n",
                    (unsigned long)s.t_ms, s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
    }
  }

  f.close();
  Serial.println("END");
}

void cmdDeleteAll() {
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    String name = String("/") + f.name();
    f = root.openNextFile();
    LittleFS.remove(name);
  }
  Serial.println("OK: all deleted");
}

void handleSerial() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();

  if (line == "list") {
    cmdList();
  } else if (line.startsWith("dump ")) {
    String name = line.substring(5);
    name.trim();
    if (!name.startsWith("/")) name = "/" + name;
    cmdDump(name.c_str());
  } else if (line == "delete_all") {
    cmdDeleteAll();
  } else if (line.length() > 0) {
    Serial.println("commands: list | dump <file> | delete_all");
  }
}

//  Выключение

void powerOff() {
  if (recording) stopRecording();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("OFF");
  delay(800);

  M5.Display.sleep();
  M5.Power.powerOff();
}

//  Экран

void drawScreen() {
  M5.Display.setCursor(0, 0);

  if (recording) {
    uint32_t sec = (millis() - rec_start_ms) / 1000;

    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.printf("* REC  %02lu:%02lu\n\n", sec / 60, sec % 60);

    // Самое крупное на экране — гребки за последний
    // законченный отрезок. Это то, что смотрят на бортике.
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.printf("%2lu  \n", (unsigned long)last_lap_strokes);
    M5.Display.setTextSize(1);

    M5.Display.printf("\nlap %lu    \n", (unsigned long)lap_count);

    if (in_lap) {
      M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
      M5.Display.printf("now: %lu    \n", (unsigned long)lap_strokes);
    } else {
      M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
      M5.Display.printf("rest       \n");
    }

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.printf("tot %lu     \n", (unsigned long)total_strokes);

    if (dropped > 0) {
      M5.Display.setTextColor(TFT_RED, TFT_BLACK);
      M5.Display.printf("DROP %lu   \n", (unsigned long)dropped);
    }
  } else {
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.printf("READY       \n\n");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.printf("A = start   \n");
    M5.Display.printf("B hold = off\n\n");

    size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    M5.Display.printf("free %u KB  \n", (unsigned)(free_bytes / 1024));
    M5.Display.printf("bat %d%%     \n", M5.Power.getBatteryLevel());
  }
}

//  setup / loop

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);

  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  if (!M5.Imu.isEnabled()) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.println("IMU NOT FOUND");
    while (true) delay(1000);
  }

  if (!LittleFS.begin(true)) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.println("FS FAILED");
    while (true) delay(1000);
  }

  lowpass.design(LOWPASS_HZ, FS);

  // За AMP_TAU_S секунд амплитуда спадает в e раз.
  // Переводим это в коэффициент на один сэмпл.
  amp_decay = expf(-1.0f / (AMP_TAU_S * FS));
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    if (recording)
      stopRecording();
    else
      startRecording();
  }

  if (M5.BtnB.pressedFor(SLEEP_HOLD_MS)) {
    powerOff();
  }

  M5.Imu.update();

  if (recording) {
    uint32_t now_us = micros();

    if ((int32_t)(now_us - next_sample_us) >= 0) {
      next_sample_us += SAMPLE_INTERVAL_US;

      auto d = M5.Imu.getImuData();

      Sample s;
      s.t_ms = millis() - rec_start_ms;
      s.ax = d.accel.x;
      s.ay = d.accel.y;
      s.az = d.accel.z;
      s.gx = d.gyro.x;
      s.gy = d.gyro.y;
      s.gz = d.gyro.z;

      // Сырой сигнал пишем как раньше — он нужен, чтобы дома сравнить счёт платы с Python-версией
      memcpy(buffer + buffer_used, &s, sizeof(s));
      buffer_used += sizeof(s);
      sample_total++;

      if (buffer_used >= BUFFER_SIZE) flushBuffer();

      // И тут же считаем в реальном времени
      float gyro_mag = sqrtf(s.gx * s.gx + s.gy * s.gy + s.gz * s.gz);
      processSample(gyro_mag, s.t_ms);
    }
  } else {
    handleSerial();
  }

  uint32_t now_ms = millis();
  if (now_ms - last_screen_ms >= SCREEN_INTERVAL_MS) {
    last_screen_ms = now_ms;
    drawScreen();
  }
}