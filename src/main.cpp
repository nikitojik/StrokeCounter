#include <LittleFS.h>
#include <M5Unified.h>

//  Настройки

// Частота записи. IMU отдаёт ~170 Гц, но писать столько не нужно:
// гребок — это примерно 1 Гц, 50 Гц даёт более чем достаточный запас.
const uint32_t SAMPLE_INTERVAL_US = 20000;  // 20 000 мкс = 50 Гц

// Как часто перерисовываем экран. Отрисовка медленная,поэтому делаем это редко и независимо от записи.
const uint32_t SCREEN_INTERVAL_MS = 250;  // 4 раза в секунду

// Размер буфера в оперативной памяти.
// Кратен размеру одной записи (28 байт): 28 * 72 = 2016
const size_t BUFFER_SIZE = 2016;

//  Формат одной записи

// pack(1) запрещает компилятору вставлять выравнивающие байты
// между полями. Без этого структура заняла бы больше 28 байт,
// и Python не смог бы разобрать файл по фиксированной схеме.
#pragma pack(push, 1)
struct Sample {
  uint32_t t_ms;     // время от старта записи, мс
  float ax, ay, az;  // ускорение, g
  float gx, gy, gz;  // угловая скорость, градусы/с
};
#pragma pack(pop)

// 4 + 6*4 = 28 байт. Проверяем на этапе компиляции:
// если структура вдруг окажется другого размера, сборка упадёт
// здесь, а не позже при разборе данных.
static_assert(sizeof(Sample) == 28, "Sample must be exactly 28 bytes");

//  Состояние
bool recording = false;
File rec_file;
char rec_name[32] = "";

uint8_t buffer[BUFFER_SIZE];
size_t buffer_used = 0;

uint32_t next_sample_us = 0;  // когда писать следующий сэмпл
uint32_t rec_start_ms = 0;    // момент старта записи
uint32_t sample_total = 0;    // сколько записано в текущей сессии
uint32_t dropped = 0;         // сколько потеряно из-за ошибок записи

uint32_t last_screen_ms = 0;

//  Работа с файлами

// Ищем первое свободное имя: /rec_0001.bin, /rec_0002.bin, ...
// Так каждая сессия попадает в отдельный файл и ничего не затирается при повторном старте.
void makeNextName(char* out, size_t out_size) {
  for (int i = 1; i < 10000; i++) {
    snprintf(out, out_size, "/rec_%04d.bin", i);
    if (!LittleFS.exists(out)) {
      return;
    }
  }
  snprintf(out, out_size, "/rec_overflow.bin");
}

// Сбрасываем накопленный буфер на флеш.
// Пишем крупными кусками, а не по 28 байт: у флеш-памяти заметная задержка на каждую операцию записи.
bool flushBuffer() {
  if (buffer_used == 0) {
    return true;
  }
  size_t written = rec_file.write(buffer, buffer_used);
  bool ok = (written == buffer_used);
  if (!ok) {
    dropped++;
  }
  buffer_used = 0;
  return ok;
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

  buffer_used = 0;
  sample_total = 0;
  dropped = 0;
  rec_start_ms = millis();
  next_sample_us = micros();
  recording = true;

  M5.Display.fillScreen(TFT_BLACK);
}

void stopRecording() {
  flushBuffer();
  rec_file.close();
  recording = false;
  M5.Display.fillScreen(TFT_BLACK);
}

//  Команды через Serial (работают только когда запись выключена)

void cmdList() {
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  Serial.println("--- files ---");
  while (f) {
    // Размер в байтах делим на 28 — получаем число сэмплов,
    // делим на 50 — получаем секунды записи
    size_t bytes = f.size();
    Serial.printf("%s  %u bytes  %.1f s\n", f.name(), (unsigned)bytes,
                  bytes / 28.0 / 50.0);
    f = root.openNextFile();
  }
  Serial.println("--- end ---");
}

// Читаем бинарный файл и печатаем его как CSV.
// Конвертация происходит здесь, а не при записи:
// во время плавания важна скорость, а при выгрузке — удобство.
void cmdDump(const char* name) {
  File f = LittleFS.open(name, FILE_READ);
  if (!f) {
    Serial.println("ERR: no such file");
    return;
  }

  Serial.println("BEGIN");
  Serial.println("t_ms,ax,ay,az,gx,gy,gz");

  Sample s;
  while (f.read((uint8_t*)&s, sizeof(s)) == sizeof(s)) {
    Serial.printf("%lu,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f\n", (unsigned long)s.t_ms,
                  s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
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
  if (!Serial.available()) {
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();

  if (line == "list") {
    cmdList();
  } else if (line.startsWith("dump ")) {
    String name = line.substring(5);
    name.trim();
    if (!name.startsWith("/")) {
      name = "/" + name;
    }
    cmdDump(name.c_str());
  } else if (line == "delete_all") {
    cmdDeleteAll();
  } else if (line.length() > 0) {
    Serial.println("commands: list | dump <file> | delete_all");
  }
}

//  Экран

void drawScreen() {
  M5.Display.setCursor(0, 0);

  if (recording) {
    uint32_t sec = (millis() - rec_start_ms) / 1000;

    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.printf("* REC       \n\n");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.printf("%s\n", rec_name + 1);
    M5.Display.printf("%02lu:%02lu       \n", sec / 60, sec % 60);
    M5.Display.printf("%lu smp    \n", (unsigned long)sample_total);
    if (dropped > 0) {
      M5.Display.printf("DROP %lu   \n", (unsigned long)dropped);
    }
  } else {
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.printf("READY       \n\n");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.printf("BtnA = start\n\n");

    // Сколько места осталось под данные
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    size_t free_bytes = total - used;
    M5.Display.printf("free %u KB  \n", (unsigned)(free_bytes / 1024));
    M5.Display.printf("~%u min     \n", (unsigned)(free_bytes / 1400 / 60));

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

  // true = отформатировать, если файловая система не смонтировалась.
  // Нужно при первом запуске на чистом разделе.
  if (!LittleFS.begin(true)) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.println("FS FAILED");
    while (true) delay(1000);
  }
}

void loop() {
  M5.update();

  // Кнопка A (передняя, с логотипом M5) — старт/стоп
  if (M5.BtnA.wasPressed()) {
    if (recording) {
      stopRecording();
    } else {
      startRecording();
    }
  }

  // Забираем новые данные с IMU всегда, независимо от режима.
  // Это держит внутренний буфер датчика пустым.
  M5.Imu.update();

  if (recording) {
    uint32_t now_us = micros();

    // Сравнение через вычитание со знаком корректно работает при переполнении счётчика micros() (каждые ~70 минут)
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

      memcpy(buffer + buffer_used, &s, sizeof(s));
      buffer_used += sizeof(s);
      sample_total++;

      if (buffer_used >= BUFFER_SIZE) {
        flushBuffer();
      }
    }
  } else {
    // Команды принимаем только когда не пишем,чтобы работа с Serial не тормозила запись
    handleSerial();
  }

  uint32_t now_ms = millis();
  if (now_ms - last_screen_ms >= SCREEN_INTERVAL_MS) {
    last_screen_ms = now_ms;
    drawScreen();
  }
}