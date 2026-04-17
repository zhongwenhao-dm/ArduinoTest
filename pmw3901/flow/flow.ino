#include "Bitcraze_PMW3901.h"

// Using digital pin 10 for chip select
Bitcraze_PMW3901 flow(5);

void setup() {
  Serial.begin(115200);

  if (!flow.begin()) {
    Serial.println("Initialization of the flow sensor failed");
    while(1) { }
  }
}

int16_t deltaX,deltaY;

void loop() {
  // Get motion count since last call
  
  flow.readMotionCount(&deltaX, &deltaY);
  Serial.print(millis());
  Serial.print(",");
  Serial.print(deltaX);
  Serial.print(",");
  Serial.print(deltaY);
  Serial.print("\n");

  delay(50);
}
