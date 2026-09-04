// Placeholder. Replaced in Task 9. Exists so tools/build.sh can be exercised
// against the copied headers before any new code is written.
#include "config.h"
#if ENABLE_SERIAL_DEBUG
  #define LOG(...)    Serial.print(__VA_ARGS__)
  #define LOGLN(...)  Serial.println(__VA_ARGS__)
#else
  #define LOG(...)
  #define LOGLN(...)
#endif
#include "plc_tags.h"
#include "shared.h"
#include "thingProperties.h"
void setup() {}
void loop() {}
