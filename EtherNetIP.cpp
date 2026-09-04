// =====================================================================
//  EtherNetIP.cpp  -  implementation
// =====================================================================
#include "EtherNetIP.h"
#include "config.h"

// EtherNet/IP encapsulation commands
#define ENIP_CMD_REGISTER_SESSION    0x0065
#define ENIP_CMD_UNREGISTER_SESSION  0x0066
#define ENIP_CMD_SENDRRDATA          0x006F

// CPF (Common Packet Format) item type ids
#define CPF_ITEM_NULL                0x0000
#define CPF_ITEM_UNCONNECTED_DATA    0x00B2

// CIP services
#define CIP_SVC_READ_TAG             0x4C
#define CIP_SVC_WRITE_TAG            0x4D
#define CIP_SVC_MULTIPLE             0x0A
#define CIP_SVC_UNCONNECTED_SEND     0x52
#define CIP_REPLY_BIT                0x80

// CIP "Embedded service error" -- some sub-services failed but the reply
// still carries valid data for the ones that succeeded.
#define CIP_STATUS_EMBEDDED_ERR      0x1E

#define ENIP_HEADER_LEN              24
// Single-element atomic reads/writes + symbolic path stay well under this.
// Kept small on purpose: several of these live on the stack at once.
#define WORK_BUF                     256
// Larger ceiling for batched (MSP) messages. The buffers sized to this are
// static (reused), not on the stack, so the gateway's single sequential
// request path can use them safely.
#define EIP_MAX_MSG                  700

// ---- little-endian append helpers -----------------------------------
static inline void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static inline void put32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline uint16_t get16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

EtherNetIPClient::EtherNetIPClient(Client& transport, IPAddress plcIp, uint16_t port)
  : _t(transport), _ip(plcIp), _port(port) {}

bool EtherNetIPClient::connected() {
  return _session != 0 && _t.connected();
}

bool EtherNetIPClient::readExact(uint8_t* buf, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < n) {
    int avail = _t.available();
    if (avail > 0) {
      int c = _t.read();
      if (c < 0) return false;
      buf[got++] = (uint8_t)c;
      start = millis();
    } else {
      if (!_t.connected()) return false;
      if (millis() - start > timeoutMs) return false;
      delay(1);
    }
  }
  return true;
}

bool EtherNetIPClient::begin() {
  end();  // ensure clean state

  if (!_t.connect(_ip, _port)) {
    return false;
  }

  // ---- RegisterSession --------------------------------------------------
  uint8_t pkt[ENIP_HEADER_LEN + 4];
  memset(pkt, 0, sizeof(pkt));
  put16(pkt + 0, ENIP_CMD_REGISTER_SESSION);
  put16(pkt + 2, 4);          // data length
  // session handle (0), status (0), sender context (0), options (0) already zeroed
  put16(pkt + 24, 1);         // protocol version
  put16(pkt + 26, 0);         // options flags
  if (_t.write(pkt, sizeof(pkt)) != (int)sizeof(pkt)) { end(); return false; }

  uint8_t hdr[ENIP_HEADER_LEN];
  if (!readExact(hdr, ENIP_HEADER_LEN, PLC_IO_TIMEOUT_MS)) { end(); return false; }
  uint16_t cmd    = get16(hdr + 0);
  uint16_t len    = get16(hdr + 2);
  uint32_t status = get32(hdr + 8);
  if (cmd != ENIP_CMD_REGISTER_SESSION || status != 0) { end(); return false; }

  // discard the 4-byte body (version + options)
  uint8_t body[8];
  if (len > 0) {
    if (len > sizeof(body)) len = sizeof(body);
    if (!readExact(body, len, PLC_IO_TIMEOUT_MS)) { end(); return false; }
  }
  _session = get32(hdr + 4);
  return _session != 0;
}

void EtherNetIPClient::end() {
  if (_session != 0 && _t.connected()) {
    uint8_t pkt[ENIP_HEADER_LEN];
    memset(pkt, 0, sizeof(pkt));
    put16(pkt + 0, ENIP_CMD_UNREGISTER_SESSION);
    put16(pkt + 2, 0);
    put32(pkt + 4, _session);
    _t.write(pkt, sizeof(pkt));
  }
  _t.stop();
  _session = 0;
}

// ---- symbolic path ---------------------------------------------------
uint16_t EtherNetIPClient::buildSymbolicPath(const char* tag, uint8_t* out, size_t cap) {
  size_t n = 0;
  const char* p = tag;
  while (*p) {
    // read one segment name up to '.' or '[' or end
    char name[64];
    size_t nlen = 0;
    while (*p && *p != '.' && *p != '[') {
      if (nlen >= sizeof(name) - 1) return 0;
      name[nlen++] = *p++;
    }
    if (nlen == 0) return 0;
    // ANSI symbolic segment: 0x91, len, chars, pad to even
    if (n + 2 + nlen + 1 > cap) return 0;
    out[n++] = 0x91;
    out[n++] = (uint8_t)nlen;
    memcpy(out + n, name, nlen);
    n += nlen;
    if (nlen & 1) out[n++] = 0x00;  // pad

    // optional array index segments: [i] (possibly several, e.g. [1][2])
    while (*p == '[') {
      p++;
      uint32_t idx = 0;
      bool any = false;
      while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; any = true; }
      if (!any || *p != ']') return 0;
      p++;
      if (idx <= 0xFF) {
        if (n + 2 > cap) return 0;
        out[n++] = 0x28; out[n++] = (uint8_t)idx;
      } else if (idx <= 0xFFFF) {
        if (n + 4 > cap) return 0;
        out[n++] = 0x29; out[n++] = 0x00; put16(out + n, (uint16_t)idx); n += 2;
      } else {
        if (n + 6 > cap) return 0;
        out[n++] = 0x2A; out[n++] = 0x00; put32(out + n, idx); n += 4;
      }
    }
    if (*p == '.') p++;  // move to next member
  }
  return (uint16_t)n;
}

uint16_t EtherNetIPClient::buildCipRequest(uint8_t service, const char* tag,
                                           uint16_t dataType, const uint8_t* data,
                                           size_t dataLen, uint16_t elemCount,
                                           uint8_t* out, size_t cap) {
  uint8_t path[80];
  uint16_t pathBytes = buildSymbolicPath(tag, path, sizeof(path));
  if (pathBytes == 0 || (pathBytes & 1)) return 0;  // must be whole words

  size_t n = 0;
  if (n + 2 + pathBytes > cap) return 0;
  out[n++] = service;
  out[n++] = (uint8_t)(pathBytes / 2);  // request path size in words
  memcpy(out + n, path, pathBytes); n += pathBytes;

  if (service == CIP_SVC_WRITE_TAG) {
    if (n + 4 + dataLen > cap) return 0;
    put16(out + n, dataType); n += 2;
    put16(out + n, elemCount); n += 2;
    memcpy(out + n, data, dataLen); n += dataLen;
  } else {  // read
    if (n + 2 > cap) return 0;
    put16(out + n, elemCount); n += 2;
  }
  return (uint16_t)n;
}

uint16_t EtherNetIPClient::wrapUnconnectedSend(const uint8_t* cip, uint16_t cipLen,
                                               uint8_t* out, size_t cap) {
  size_t n = 0;
  // Unconnected Send request, addressed to the Connection Manager object.
  if (n + 8 > cap) return 0;
  out[n++] = CIP_SVC_UNCONNECTED_SEND;
  out[n++] = 0x02;                 // path size (words)
  out[n++] = 0x20; out[n++] = 0x06;  // 8-bit class id = 0x06 (Connection Manager)
  out[n++] = 0x24; out[n++] = 0x01;  // 8-bit instance id = 0x01
  out[n++] = 0x0A;                 // priority / time_tick
  out[n++] = 0x05;                 // timeout_ticks  (~5 s)

  if (n + 2 + cipLen > cap) return 0;
  put16(out + n, cipLen); n += 2;          // embedded message size
  memcpy(out + n, cip, cipLen); n += cipLen;
  if (cipLen & 1) { if (n + 1 > cap) return 0; out[n++] = 0x00; }  // pad

  // route path to the CPU
  if (n + 4 > cap) return 0;
  out[n++] = 0x01;                 // route path size (words)
  out[n++] = 0x00;                 // reserved
  out[n++] = PLC_ROUTE_PATH_PORT;  // port (1 = backplane)
  out[n++] = PLC_ROUTE_PATH_SLOT;  // link address (CPU slot)
  return (uint16_t)n;
}

bool EtherNetIPClient::sendRRData(const uint8_t* cip, uint16_t cipLen,
                                  uint8_t* reply, size_t replyCap, size_t& replyLen) {
  if (!connected()) return false;

  // CPF: null addr item + unconnected data item
  uint16_t dataLen = 4 + 2 + 2 + (2 + 2) + (2 + 2 + cipLen);
  //                 iface  to  cnt   item0(type+len)  item1(type+len+cip)
  // Static (not stacked): the gateway issues one request at a time.
  static uint8_t pkt[ENIP_HEADER_LEN + EIP_MAX_MSG];
  if (ENIP_HEADER_LEN + dataLen > sizeof(pkt)) return false;

  memset(pkt, 0, ENIP_HEADER_LEN);
  put16(pkt + 0, ENIP_CMD_SENDRRDATA);
  put16(pkt + 2, dataLen);
  put32(pkt + 4, _session);

  uint8_t* d = pkt + ENIP_HEADER_LEN;
  size_t k = 0;
  put32(d + k, 0); k += 4;          // interface handle (0 = CIP)
  put16(d + k, 5); k += 2;          // timeout (s)
  put16(d + k, 2); k += 2;          // item count
  put16(d + k, CPF_ITEM_NULL); k += 2;            // item0: null address
  put16(d + k, 0); k += 2;                        // item0 length
  put16(d + k, CPF_ITEM_UNCONNECTED_DATA); k += 2;// item1: unconnected data
  put16(d + k, cipLen); k += 2;                   // item1 length
  memcpy(d + k, cip, cipLen); k += cipLen;

  size_t total = ENIP_HEADER_LEN + dataLen;
  if (_t.write(pkt, total) != (int)total) return false;

  // ---- read encapsulation header ----
  uint8_t hdr[ENIP_HEADER_LEN];
  if (!readExact(hdr, ENIP_HEADER_LEN, PLC_IO_TIMEOUT_MS)) return false;
  if (get16(hdr + 0) != ENIP_CMD_SENDRRDATA) return false;
  if (get32(hdr + 8) != 0) return false;           // encapsulation status
  uint16_t bodyLen = get16(hdr + 2);
  if (bodyLen < 4 + 2 + 2 + 4 + 4) return false;

  static uint8_t body[EIP_MAX_MSG];
  if (bodyLen > sizeof(body)) return false;
  if (!readExact(body, bodyLen, PLC_IO_TIMEOUT_MS)) return false;

  // parse CPF
  size_t j = 0;
  j += 4;                       // interface handle
  j += 2;                       // timeout
  uint16_t items = get16(body + j); j += 2;
  if (items < 2) return false;
  // item0 (address)
  j += 2;                       // type
  uint16_t len0 = get16(body + j); j += 2;
  j += len0;
  // item1 (data)
  uint16_t type1 = get16(body + j); j += 2;
  uint16_t len1  = get16(body + j); j += 2;
  if (type1 != CPF_ITEM_UNCONNECTED_DATA) return false;
  if (j + len1 > bodyLen || len1 > replyCap) return false;

  memcpy(reply, body + j, len1);
  replyLen = len1;
  return true;
}

bool EtherNetIPClient::parseCipReply(const uint8_t* reply, size_t replyLen,
                                     uint8_t expectedService,
                                     const uint8_t** dataOut, size_t* dataLenOut) {
  if (replyLen < 4) return false;
  uint8_t service   = reply[0];     // = expectedService | 0x80
  // reply[1] reserved
  uint8_t genStatus = reply[2];
  uint8_t extWords  = reply[3];
  _lastCipStatus = genStatus;
  _lastExtLen    = extWords;

  if ((service & 0x7F) != (expectedService & 0x7F)) return false;

  size_t off = 4 + (size_t)extWords * 2;
  if (off > replyLen) return false;

  // If this was an Unconnected Send, the embedded reply carries its own
  // status; status 0x00 from the wrapper means "routed OK" and the actual
  // tag-service reply data follows. We already validated the service code
  // matches the embedded service because the controller returns the
  // embedded service reply directly in the data item.
  if (genStatus != 0x00) return false;

  if (dataOut)    *dataOut = reply + off;
  if (dataLenOut) *dataLenOut = replyLen - off;
  return true;
}

bool EtherNetIPClient::readRaw(const char* tag, uint16_t& dataType,
                               uint8_t* buf, size_t bufCap, size_t& outLen) {
  uint8_t cip[WORK_BUF];
  uint16_t cipLen = buildCipRequest(CIP_SVC_READ_TAG, tag, 0, nullptr, 0, 1, cip, sizeof(cip));
  if (cipLen == 0) return false;

#if USE_UNCONNECTED_SEND
  uint8_t wrapped[WORK_BUF];
  uint16_t wl = wrapUnconnectedSend(cip, cipLen, wrapped, sizeof(wrapped));
  if (wl == 0) return false;
  const uint8_t* msg = wrapped; uint16_t msgLen = wl;
#else
  const uint8_t* msg = cip; uint16_t msgLen = cipLen;
#endif

  uint8_t reply[WORK_BUF]; size_t replyLen = 0;
  if (!sendRRData(msg, msgLen, reply, sizeof(reply), replyLen)) return false;

  const uint8_t* data = nullptr; size_t dataLen = 0;
  if (!parseCipReply(reply, replyLen, CIP_SVC_READ_TAG, &data, &dataLen)) return false;
  if (dataLen < 2) return false;

  dataType = get16(data);
  size_t valLen = dataLen - 2;
  if (valLen > bufCap) valLen = bufCap;
  memcpy(buf, data + 2, valLen);
  outLen = valLen;
  return true;
}

bool EtherNetIPClient::writeRaw(const char* tag, uint16_t dataType,
                                const uint8_t* data, size_t dataLen) {
  uint8_t cip[WORK_BUF];
  uint16_t cipLen = buildCipRequest(CIP_SVC_WRITE_TAG, tag, dataType, data, dataLen, 1,
                                    cip, sizeof(cip));
  if (cipLen == 0) return false;

#if USE_UNCONNECTED_SEND
  uint8_t wrapped[WORK_BUF];
  uint16_t wl = wrapUnconnectedSend(cip, cipLen, wrapped, sizeof(wrapped));
  if (wl == 0) return false;
  const uint8_t* msg = wrapped; uint16_t msgLen = wl;
#else
  const uint8_t* msg = cip; uint16_t msgLen = cipLen;
#endif

  uint8_t reply[WORK_BUF]; size_t replyLen = 0;
  if (!sendRRData(msg, msgLen, reply, sizeof(reply), replyLen)) return false;
  return parseCipReply(reply, replyLen, CIP_SVC_WRITE_TAG, nullptr, nullptr);
}

// ---- typed helpers ---------------------------------------------------
bool EtherNetIPClient::readReal(const char* tag, float& out) {
  uint16_t type; uint8_t buf[8]; size_t n;
  if (!readRaw(tag, type, buf, sizeof(buf), n)) return false;
  if (type == CIP_TYPE_REAL && n >= 4) { memcpy(&out, buf, 4); return true; }
  if (type == CIP_TYPE_DINT && n >= 4) { int32_t v; memcpy(&v, buf, 4); out = (float)v; return true; }
  if (type == CIP_TYPE_INT  && n >= 2) { int16_t v; memcpy(&v, buf, 2); out = (float)v; return true; }
  return false;
}

bool EtherNetIPClient::readDint(const char* tag, int32_t& out) {
  uint16_t type; uint8_t buf[8]; size_t n;
  if (!readRaw(tag, type, buf, sizeof(buf), n)) return false;
  if ((type == CIP_TYPE_DINT || type == CIP_TYPE_UDINT) && n >= 4) { memcpy(&out, buf, 4); return true; }
  if (type == CIP_TYPE_INT  && n >= 2) { int16_t v; memcpy(&v, buf, 2); out = v; return true; }
  return false;
}

bool EtherNetIPClient::readInt(const char* tag, int16_t& out) {
  uint16_t type; uint8_t buf[8]; size_t n;
  if (!readRaw(tag, type, buf, sizeof(buf), n)) return false;
  if ((type == CIP_TYPE_INT || type == CIP_TYPE_UINT) && n >= 2) { memcpy(&out, buf, 2); return true; }
  return false;
}

bool EtherNetIPClient::readBool(const char* tag, bool& out) {
  uint16_t type; uint8_t buf[8]; size_t n;
  if (!readRaw(tag, type, buf, sizeof(buf), n)) return false;
  if (n >= 1) { out = (buf[0] != 0); return true; }
  return false;
}

bool EtherNetIPClient::writeReal(const char* tag, float value) {
  uint8_t d[4]; memcpy(d, &value, 4);
  return writeRaw(tag, CIP_TYPE_REAL, d, 4);
}
bool EtherNetIPClient::writeDint(const char* tag, int32_t value) {
  uint8_t d[4]; memcpy(d, &value, 4);
  return writeRaw(tag, CIP_TYPE_DINT, d, 4);
}
bool EtherNetIPClient::writeInt(const char* tag, int16_t value) {
  uint8_t d[2]; memcpy(d, &value, 2);
  return writeRaw(tag, CIP_TYPE_INT, d, 2);
}
bool EtherNetIPClient::writeBool(const char* tag, bool value) {
  uint8_t d[1]; d[0] = value ? 0xFF : 0x00;  // Logix BOOL written as a byte
  return writeRaw(tag, CIP_TYPE_BOOL, d, 1);
}

// ---- Multiple Service Packet (batched reads) -------------------------
//
// CIP layout built here (the request, before any Unconnected Send wrap):
//   0x0A  path_size=2  20 02 24 01      <- service + path to Message Router
//   uint16 count
//   uint16 offset[count]                <- bytes from `count` to each sub-req
//   <embedded Read Tag request>[count]
//
uint16_t EtherNetIPClient::buildReadMSP(const char* const* tags, size_t count,
                                        uint8_t* out, size_t cap) {
  if (count == 0 || count > EIP_MAX_MSP_TAGS) return 0;

  size_t n = 0;
  if (n + 6 > cap) return 0;
  out[n++] = CIP_SVC_MULTIPLE;
  out[n++] = 0x02;                          // path size (words)
  out[n++] = 0x20; out[n++] = 0x02;         // class 0x02 = Message Router
  out[n++] = 0x24; out[n++] = 0x01;         // instance 1

  const size_t base = n;                    // start of the `count` word
  const size_t header = 2 + 2 * count;      // count word + offset table
  size_t pos = base + header;

  uint16_t offsets[EIP_MAX_MSP_TAGS];
  for (size_t i = 0; i < count; i++) {
    uint16_t len = buildCipRequest(CIP_SVC_READ_TAG, tags[i], 0, nullptr, 0, 1,
                                   out + pos, cap - pos);
    if (len == 0) return 0;
    offsets[i] = (uint16_t)(pos - base);    // relative to the `count` word
    pos += len;
  }

  put16(out + base, (uint16_t)count);
  for (size_t i = 0; i < count; i++) put16(out + base + 2 + 2 * i, offsets[i]);
  return (uint16_t)pos;
}

bool EtherNetIPClient::readRealsMSP(const char* const* tags, float* out,
                                    bool* ok, size_t count) {
  for (size_t i = 0; i < count; i++) ok[i] = false;
  if (count == 0) return true;
  if (count > EIP_MAX_MSP_TAGS) return false;

  static uint8_t cip[EIP_MAX_MSG];
  uint16_t cipLen = buildReadMSP(tags, count, cip, sizeof(cip));
  if (cipLen == 0) return false;

#if USE_UNCONNECTED_SEND
  static uint8_t wrapped[EIP_MAX_MSG];
  uint16_t wl = wrapUnconnectedSend(cip, cipLen, wrapped, sizeof(wrapped));
  if (wl == 0) return false;
  const uint8_t* msg = wrapped; uint16_t msgLen = wl;
#else
  const uint8_t* msg = cip; uint16_t msgLen = cipLen;
#endif

  static uint8_t reply[EIP_MAX_MSG]; size_t replyLen = 0;
  if (!sendRRData(msg, msgLen, reply, sizeof(reply), replyLen)) return false;
  if (replyLen < 4) return false;

  // MSP reply header
  uint8_t service   = reply[0];          // expect 0x8A
  uint8_t genStatus = reply[2];
  uint8_t extWords  = reply[3];
  _lastCipStatus = genStatus;
  _lastExtLen    = extWords;
  if ((service & 0x7F) != CIP_SVC_MULTIPLE) return false;
  // 0x00 = all OK, 0x1E = some sub-services errored (data still present)
  if (genStatus != 0x00 && genStatus != CIP_STATUS_EMBEDDED_ERR) return false;

  const uint8_t* d = reply + 4 + (size_t)extWords * 2;
  if (d + 2 > reply + replyLen) return false;
  size_t dLen = replyLen - (4 + (size_t)extWords * 2);

  uint16_t rc = get16(d);
  if (rc == 0 || rc > count) return false;

  for (size_t i = 0; i < rc; i++) {
    uint16_t start = get16(d + 2 + 2 * i);
    uint16_t end   = (i + 1 < rc) ? get16(d + 2 + 2 * (i + 1)) : (uint16_t)dLen;
    if (start >= dLen || end > dLen || end < start + 4) continue;

    const uint8_t* seg = d + start;
    size_t segLen      = end - start;
    uint8_t  estatus   = seg[2];
    uint8_t  eext      = seg[3];
    size_t   voff      = 4 + (size_t)eext * 2;
    if (estatus != 0x00 || voff + 2 > segLen) continue;

    uint16_t type = get16(seg + voff);
    const uint8_t* v = seg + voff + 2;
    size_t vlen = segLen - (voff + 2);

    if (type == CIP_TYPE_REAL && vlen >= 4) { memcpy(&out[i], v, 4); ok[i] = true; }
    else if (type == CIP_TYPE_DINT && vlen >= 4) { int32_t x; memcpy(&x, v, 4); out[i] = (float)x; ok[i] = true; }
    else if (type == CIP_TYPE_INT  && vlen >= 2) { int16_t x; memcpy(&x, v, 2); out[i] = (float)x; ok[i] = true; }
    //  BOOL as 0.0/1.0 so discrete state (valves, pumps) can ride the same
    //  batched sweep as the analog points. Without this branch a BOOL tag
    //  decodes to nothing and ok[i] stays false -- the read looks like a
    //  comms failure when in fact the value arrived intact.
    else if (type == CIP_TYPE_BOOL && vlen >= 1) { out[i] = v[0] ? 1.0f : 0.0f; ok[i] = true; }
  }
  return true;
}

size_t EtherNetIPClient::readRealsMultiple(const char* const* tags, float* out,
                                           bool* ok, size_t count) {
  size_t done = 0, good = 0;
  while (done < count) {
    size_t chunk = count - done;
    if (chunk > EIP_MAX_MSP_TAGS) chunk = EIP_MAX_MSP_TAGS;
    if (readRealsMSP(tags + done, out + done, ok + done, chunk)) {
      for (size_t i = 0; i < chunk; i++) if (ok[done + i]) good++;
    } else {
      for (size_t i = 0; i < chunk; i++) ok[done + i] = false;
    }
    done += chunk;
  }
  return good;
}
