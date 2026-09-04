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
#include "stack_watch.h"
#include "wd_feeder.h"
#include <PortentaEthernet.h>
#include "EtherNetIP.h"
EthernetClient plcTransport;
IPAddress plcIp(PLC_IP_OCTET_0, PLC_IP_OCTET_1, PLC_IP_OCTET_2, PLC_IP_OCTET_3);
EtherNetIPClient eip(plcTransport, plcIp, PLC_ENIP_PORT);
#include "plc_thread.h"
static StackWatch swMain;
static_assert(sizeof(StackWatch) <= 16, "StackWatch is three words; keep it a value type");
void setup() {}
void loop() {}
