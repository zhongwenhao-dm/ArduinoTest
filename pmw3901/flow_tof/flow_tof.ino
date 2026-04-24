#include "Bitcraze_PMW3901.h"
#include <Wire.h>
#include <VL53L1X.h>

// Using digital pin 5 for chip select
Bitcraze_PMW3901 flow(5);
VL53L1X tof;

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);

  if (!flow.begin()) {
    Serial.println("Initialization of the flow sensor failed");
    while(1) { }
  }

  if (!tof.init()) {
    Serial.println("VL53L1X initialization failed");
    while(1) { }
  }

  tof.setTimeout(500);
  tof.setDistanceMode(VL53L1X::Short);
  tof.setMeasurementTimingBudget(50000);
  tof.startContinuous(50);

  Serial.println("Sensors ready!");
  Serial.println("Time(ms), DeltaX, DeltaY, Distance(mm)");
}

int16_t deltaX,deltaY;
uint16_t distance;

void loop() {
  // Get motion count since last call
  
  flow.readMotionCount(&deltaX, &deltaY);

  distance = tof.read();
  
  Serial.print(millis());
  Serial.print(",");
  Serial.print(deltaX);
  Serial.print(",");
  Serial.print(deltaY);
  Serial.print(",");
  Serial.print(distance);
  Serial.print("\n");

  delay(50);
}
