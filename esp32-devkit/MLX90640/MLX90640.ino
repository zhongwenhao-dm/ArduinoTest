#include <Wire.h>
#include <Adafruit_MLX90640.h>

Adafruit_MLX90640 mlx;
float frame[32 * 24];

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);

  if (!mlx.begin(0x33, &Wire)) {
    Serial.println("MLX90640 not found");
    while (1) delay(10);
  }

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_4_HZ);

  Serial.println("START");
}

void loop() {
  if (mlx.getFrame(frame) != 0) {
    Serial.println("ERR");
    delay(100);
    return;
  }

  for (int i = 0; i < 32 * 24; i++) {
    Serial.print(frame[i], 2);
    if (i < 32 * 24 - 1) {
      Serial.print(",");
    }
  }
  Serial.println();

  delay(50);
}
