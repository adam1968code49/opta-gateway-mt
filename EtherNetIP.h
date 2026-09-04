// =====================================================================
//  EtherNetIP.h  -  minimal EtherNet/IP + CIP client for Arduino
// =====================================================================
//
//  Speaks the SAME protocol as pycomm3's LogixDriver in plclog15.py:
//  symbolic (named-tag) access to an Allen-Bradley Logix controller over
//  EtherNet/IP explicit messaging (TCP 44818).
//
//  Flow:
//    connect TCP -> RegisterSession (0x0065) -> SendRRData (0x006F)
//    carrying a CIP Read Tag (0x4C) or Write Tag (0x4D) request, optionally
//    wrapped in an Unconnected Send (0x52) routed to the CPU.
//
//  Scope: single-element reads/writes of atomic types (REAL, DINT, INT,
//  BOOL). That covers every tag used by plclog15.py and the control
//  setpoints. Arrays / UDT members are supported in the symbolic path
//  parser ("Tag.Member", "Tag[3]") but only as single elements.
//
//  Interface-agnostic: pass any Arduino Client (EthernetClient or
//  WiFiClient). The gateway uses Ethernet for the PLC by default.
//
#pragma once
#include <Arduino.h>
#include <Client.h>
#include <IPAddress.h>

// CIP elementary data type codes (little-endian on the wire)
#define CIP_TYPE_BOOL   0x00C1
#define CIP_TYPE_SINT   0x00C2
#define CIP_TYPE_INT    0x00C3
#define CIP_TYPE_DINT   0x00C4
#define CIP_TYPE_LINT   0x00C5
#define CIP_TYPE_USINT  0x00C6
#define CIP_TYPE_UINT   0x00C7
#define CIP_TYPE_UDINT  0x00C8
#define CIP_TYPE_REAL   0x00CA
#define CIP_TYPE_LREAL  0x00CB

// Max tags per Multiple Service Packet. Kept conservative so one request +
// its reply stay inside the unconnected-message size limit (~500 bytes).
#define EIP_MAX_MSP_TAGS  12

class EtherNetIPClient {
public:
  EtherNetIPClient(Client& transport, IPAddress plcIp, uint16_t port = 44818);

  // Open TCP + RegisterSession. Returns true once a session handle is held.
  bool begin();
  // UnRegisterSession + close TCP.
  void end();
  bool connected();

  // Typed single-element helpers. Return true on CIP success.
  bool readReal(const char* tag, float& out);
  bool readDint(const char* tag, int32_t& out);
  bool readInt (const char* tag, int16_t& out);
  bool readBool(const char* tag, bool& out);

  bool writeReal(const char* tag, float value);
  bool writeDint(const char* tag, int32_t value);
  bool writeInt (const char* tag, int16_t value);
  bool writeBool(const char* tag, bool value);

  // Generic read: returns the CIP data type and copies raw value bytes.
  bool readRaw(const char* tag, uint16_t& dataType, uint8_t* buf,
               size_t bufCap, size_t& outLen);
  // Generic write: caller supplies CIP type and raw little-endian bytes.
  bool writeRaw(const char* tag, uint16_t dataType,
                const uint8_t* data, size_t dataLen);

  // ---- batched reads (CIP Multiple Service Packet, service 0x0A) ----
  // Read up to EIP_MAX_MSP_TAGS tags in ONE request. out[i]/ok[i] are filled
  // per tag (REAL/DINT/INT are coerced to float). Returns true if the packet
  // exchange itself succeeded; individual tags may still report ok[i]=false.
  bool readRealsMSP(const char* const* tags, float* out, bool* ok, size_t count);

  // Reads `count` tags by chunking into as many MSP requests as needed.
  // Returns how many tags were read successfully.
  size_t readRealsMultiple(const char* const* tags, float* out, bool* ok, size_t count);

  // Diagnostics from the last CIP exchange.
  uint8_t lastCipStatus() const { return _lastCipStatus; }
  uint8_t lastExtStatusLen() const { return _lastExtLen; }

private:
  Client&   _t;
  IPAddress _ip;
  uint16_t  _port;
  uint32_t  _session = 0;
  uint8_t   _lastCipStatus = 0;
  uint8_t   _lastExtLen = 0;

  // Build an ANSI-symbolic CIP request path from a tag string.
  // Supports "Tag", "Tag.Member", "Tag[idx]" (idx < 65536).
  // Returns number of bytes written, or 0 on error.
  uint16_t buildSymbolicPath(const char* tag, uint8_t* out, size_t cap);

  // Build a CIP Read/Write Tag service request (no encapsulation).
  uint16_t buildCipRequest(uint8_t service, const char* tag,
                           uint16_t dataType, const uint8_t* data,
                           size_t dataLen, uint16_t elemCount,
                           uint8_t* out, size_t cap);

  // Optionally wrap a CIP request in an Unconnected Send (0x52).
  uint16_t wrapUnconnectedSend(const uint8_t* cip, uint16_t cipLen,
                               uint8_t* out, size_t cap);

  // Build a Multiple Service Packet (0x0A) of Read Tag requests.
  uint16_t buildReadMSP(const char* const* tags, size_t count,
                        uint8_t* out, size_t cap);

  // Send one CIP message via SendRRData and return the CIP reply payload
  // (starting at the CIP reply service byte). Validates encapsulation.
  bool sendRRData(const uint8_t* cip, uint16_t cipLen,
                  uint8_t* reply, size_t replyCap, size_t& replyLen);

  // Low-level socket helpers with timeout.
  bool readExact(uint8_t* buf, size_t n, uint32_t timeoutMs);

  // Parse a CIP reply payload, returning the data section for reads.
  bool parseCipReply(const uint8_t* reply, size_t replyLen,
                     uint8_t expectedService,
                     const uint8_t** dataOut, size_t* dataLenOut);
};
