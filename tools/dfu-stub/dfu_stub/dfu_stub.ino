// Minimal stub for nice!nano: USB CDC only (1200-baud touch -> serial DFU).
// Does NOT use InternalFS (0xED000), so it never touches the Matter settings
// partition of the lightbar-matter firmware.
#include <Adafruit_TinyUSB.h>

void setup() { Serial.begin(115200); }
void loop() { delay(100); }
