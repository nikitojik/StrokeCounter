#include <M5Unified.h>
const uint32_t SCREEN_INTERVAL_MS = 100;

uint32_t last_screen_ms = 0;
uint32_t sample_count = 0;
uint32_t last_rate_ms = 0;
uint32_t measured_hz = 0;

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
        M5.Display.setCursor(0, 0);
        M5.Display.println("IMU NOT FOUND");
        while (true) {
            delay(1000);
        }
    }

    last_rate_ms = millis();
}

void loop() {
    M5.update();                        
    auto new_data = M5.Imu.update();

    if (new_data) {
        sample_count++;
    }

    uint32_t now = millis();

    if (now - last_rate_ms >= 1000) {
        measured_hz = sample_count;
        sample_count = 0;
        last_rate_ms = now;
    }

    if (now - last_screen_ms >= SCREEN_INTERVAL_MS) {
        last_screen_ms = now;

        auto data = M5.Imu.getImuData();

        M5.Display.setCursor(0, 0);

        M5.Display.printf("ACC g\n");
        M5.Display.printf("x %+6.2f\n", data.accel.x);
        M5.Display.printf("y %+6.2f\n", data.accel.y);
        M5.Display.printf("z %+6.2f\n", data.accel.z);

        M5.Display.printf("\nGYR dps\n");
        M5.Display.printf("x %+7.1f\n", data.gyro.x);
        M5.Display.printf("y %+7.1f\n", data.gyro.y);
        M5.Display.printf("z %+7.1f\n", data.gyro.z);

        M5.Display.printf("\n%lu Hz   ", measured_hz);

        Serial.printf("acc %+.3f %+.3f %+.3f | gyr %+.2f %+.2f %+.2f | %lu Hz\n",
                      data.accel.x, data.accel.y, data.accel.z,
                      data.gyro.x, data.gyro.y, data.gyro.z,
                      measured_hz);
    }
}