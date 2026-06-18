/*
 * NEO-M9N GNSS reader (SPI)  --  limeskey-node / Seeed XIAO ESP32-C6
 *
 * Use once D_SEL is bridged to GND (SPI selected). Shares the SPI bus with the
 * SX1262, so each device has its own chip select; the radio's NSS is held high
 * here so it releases MISO while the GNSS is addressed.
 *
 * Reads the module by clocking 0xFF and streaming the reply into a
 * checksum-validated UBX state machine. 0xFF is the module's idle byte but can
 * also appear inside a payload, so nothing is filtered: the parser only acts on
 * frames whose Fletcher checksum matches, and a drain stops only when 0xFF runs
 * appear at a packet boundary (buffer genuinely empty).
 *
 * Datasheet limits (UBX-19014285): SPI max clock 5.5 MHz, but max DATA rate only
 * 125 kB/s. 1 MHz sits on that ceiling and invites mid-frame 0xFF stuffing, so
 * this runs at 250 kHz. CPOL = 0, CPHA = 0 => SPI_MODE0.
 *
 * Wiring (matches the SPI bring-up net list):
 *   SCK    -> GPIO19      MISO -> GPIO20      MOSI -> GPIO18
 *   CS_GPS -> GPIO2  (through R3)
 */

#include <SPI.h>

// ---------------- CONFIG ----------------
#define CS_PIN        2        // CS_GPS (GPIO2, through R3)
#define SCK_PIN       19
#define MISO_PIN      20
#define MOSI_PIN      18
#define SPI_HZ        250000   // 250 kHz: under the 125 kB/s data ceiling for contiguous frames
#define LORA_NSS_PIN  -1       // set to the SX1262 NSS GPIO so it releases the shared MISO; -1 to skip
#define PPS_PIN       -1       // set if PPS is broken out; -1 to skip
#define DRAIN_MS      250      // how often to drain the module's SPI buffer
#define DRAIN_MAX     4096     // safety cap on bytes clocked per drain
#define IDLE_RUN      4        // consecutive 0xFF at a packet boundary => buffer empty
// ----------------------------------------

SPISettings gnssSpi(SPI_HZ, MSBFIRST, SPI_MODE0);   // u-blox: CPOL=0, CPHA=0

static inline void csLow()  { digitalWrite(CS_PIN, LOW);  }
static inline void csHigh() { digitalWrite(CS_PIN, HIGH); }

// ---- little-endian field readers ----
static inline uint32_t u32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline int32_t  i32(const uint8_t* p){ return (int32_t)u32(p); }
static inline uint16_t u16(const uint8_t* p){ return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }

bool gotData = false;   // set once any valid UBX frame is decoded

// ---- decoders (identical to the UART reader) ----
const char* fixName(uint8_t t) {
  switch (t) {
    case 0:  return "no fix";
    case 1:  return "dead reckoning";
    case 2:  return "2D";
    case 3:  return "3D";
    case 4:  return "GNSS + dead reckoning";
    case 5:  return "time only";
    default: return "unknown";
  }
}

// UBX-NAV-PVT (class 0x01 id 0x07, 92-byte payload)
void printNavPvt(const uint8_t* p) {
  uint16_t year   = u16(p + 4);
  uint8_t  mon    = p[6],  day = p[7];
  uint8_t  hh     = p[8],  mm  = p[9],  ss = p[10];
  uint8_t  valid  = p[11];
  uint8_t  fixType= p[20], flags = p[21], numSV = p[23];
  bool     fixOK  = flags & 0x01;
  int32_t  lon    = i32(p + 24), lat   = i32(p + 28);
  int32_t  hEllip = i32(p + 32), hMSL  = i32(p + 36);
  uint32_t hAcc   = u32(p + 40), vAcc  = u32(p + 44);
  int32_t  gSpeed = i32(p + 60), headMot = i32(p + 64);
  uint32_t sAcc   = u32(p + 68);
  uint16_t pDOP   = u16(p + 76);

  bool vDate = valid & 0x01, vTime = valid & 0x02, resolved = valid & 0x04;

  Serial.println();
  Serial.printf("[GNSS] %04u-%02u-%02u %02u:%02u:%02u UTC   (date %s, time %s%s)\n",
                year, mon, day, hh, mm, ss,
                vDate ? "valid" : "invalid",
                vTime ? "valid" : "invalid",
                resolved ? ", fully resolved" : "");

  Serial.printf("       Fix: %s%s   satellites: %u   pDOP: %.2f\n",
                fixName(fixType), fixOK ? "" : " (not usable)", numSV, pDOP / 100.0);

  if (fixOK) {
    Serial.printf("       Position: %.7f, %.7f\n", lat * 1e-7, lon * 1e-7);
    Serial.printf("       Altitude: %.2f m MSL   (%.2f m ellipsoid)\n",
                  hMSL / 1000.0, hEllip / 1000.0);
    Serial.printf("       Motion:   %.2f m/s   heading %.1f deg\n",
                  gSpeed / 1000.0, headMot * 1e-5);
    Serial.printf("       Accuracy: horiz %.2f m   vert %.2f m   speed %.2f m/s\n",
                  hAcc / 1000.0, vAcc / 1000.0, sAcc / 1000.0);
  } else {
    Serial.println("       Position: not available (waiting on satellites / antenna)");
  }
}

// UBX-MON-VER (class 0x0A id 0x04): 30-byte SW, 10-byte HW, then 30-byte extensions
void printMonVer(const uint8_t* p, uint16_t len) {
  char buf[31];
  Serial.println("\n[GNSS] Firmware (UBX-MON-VER):");
  if (len >= 30) { memcpy(buf, p,      30); buf[30] = 0; Serial.printf("       SW: %s\n", buf); }
  if (len >= 40) { memcpy(buf, p + 30, 10); buf[10] = 0; Serial.printf("       HW: %s\n", buf); }
  for (uint16_t o = 40; o + 30 <= len; o += 30) {
    memcpy(buf, p + o, 30); buf[30] = 0;
    if (buf[0]) Serial.printf("       %s\n", buf);
  }
}

void onUbxMessage(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len) {
  gotData = true;
  if      (cls == 0x01 && id == 0x07 && len >= 92) printNavPvt(p);
  else if (cls == 0x0A && id == 0x04)              printMonVer(p, len);
  else if (cls == 0x05 && len >= 2)                                    // ACK / NAK
    Serial.printf("[GNSS] %s for cls=0x%02X id=0x%02X\n",
                  id == 0x01 ? "ACK" : "NAK", p[0], p[1]);
}

// ---- UBX receive parser (running Fletcher checksum; only valid frames dispatched) ----
enum UbxState { WAIT_S1, WAIT_S2, GET_CLS, GET_ID, LEN_LO, LEN_HI, PAYLOAD, CK_A, CK_B };
static UbxState st = WAIT_S1;
static uint8_t  msgCls, msgId, ckA, ckB, rxCkA;
static uint16_t msgLen, payIdx;
static uint8_t  payload[256];   // NAV-PVT is 92; MON-VER with extensions can reach ~220

static inline bool ubxIdle() { return st == WAIT_S1; }

void ubxFeed(uint8_t b) {
  switch (st) {
    case WAIT_S1: if (b == 0xB5) st = WAIT_S2; break;
    case WAIT_S2: st = (b == 0x62) ? GET_CLS : WAIT_S1; break;
    case GET_CLS: msgCls = b; ckA = b; ckB = b; st = GET_ID; break;
    case GET_ID:  msgId  = b; ckA += b; ckB += ckA; st = LEN_LO; break;
    case LEN_LO:  msgLen = b; ckA += b; ckB += ckA; st = LEN_HI; break;
    case LEN_HI:  msgLen |= (uint16_t)b << 8; ckA += b; ckB += ckA;
                  payIdx = 0; st = msgLen ? PAYLOAD : CK_A; break;
    case PAYLOAD: if (payIdx < sizeof(payload)) payload[payIdx] = b;
                  ckA += b; ckB += ckA;
                  if (++payIdx >= msgLen) st = CK_A; break;
    case CK_A:    rxCkA = b; st = CK_B; break;
    case CK_B:    st = WAIT_S1;
                  if (rxCkA == ckA && b == ckB && msgLen <= sizeof(payload))
                    onUbxMessage(msgCls, msgId, payload, msgLen);
                  break;
  }
}

// ---- transmit a UBX frame over SPI; reply bytes clocked back are parsed too ----
void sendUBX(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len) {
  uint8_t ckA = 0, ckB = 0;
  auto ck = [&](uint8_t b){ ckA += b; ckB += ckA; };
  uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  ck(cls); ck(id); ck(len & 0xFF); ck(len >> 8);
  for (uint16_t i = 0; i < len; i++) ck(p[i]);

  SPI.beginTransaction(gnssSpi);
  csLow();
  for (int i = 0; i < 6; i++)            ubxFeed(SPI.transfer(hdr[i]));
  for (uint16_t i = 0; i < len; i++)     ubxFeed(SPI.transfer(p[i]));
  ubxFeed(SPI.transfer(ckA));
  ubxFeed(SPI.transfer(ckB));
  csHigh();
  SPI.endTransaction();
}

// ---- clock the module's buffer until it goes idle at a packet boundary ----
void drainGNSS() {
  SPI.beginTransaction(gnssSpi);
  csLow();
  uint16_t total = 0, idle = 0;
  while (total < DRAIN_MAX) {
    uint8_t b = SPI.transfer(0xFF);
    ubxFeed(b);
    total++;
    if (b == 0xFF && ubxIdle()) { if (++idle >= IDLE_RUN) break; }
    else idle = 0;
  }
  csHigh();
  SPI.endTransaction();
}

unsigned long lastDrain = 0, startMs = 0;
bool warned = false;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== NEO-M9N SPI reader (limeskey-node) ===");

  if (LORA_NSS_PIN >= 0) { pinMode(LORA_NSS_PIN, OUTPUT); digitalWrite(LORA_NSS_PIN, HIGH); }
  if (PPS_PIN >= 0) pinMode(PPS_PIN, INPUT);

  pinMode(CS_PIN, OUTPUT);
  csHigh();
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  Serial.printf("SPI up: SCK=%d MISO=%d MOSI=%d CS=%d @ %d Hz, MODE0\n",
                SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN, SPI_HZ);

  delay(200);
  sendUBX(0x0A, 0x04, nullptr, 0);   // request firmware version once
  startMs = millis();
}

void loop() {
  if (millis() - lastDrain >= DRAIN_MS) {
    lastDrain = millis();
    drainGNSS();
  }

  if (!gotData && !warned && millis() - startMs > 6000) {
    warned = true;
    Serial.println("\n[!] 6 s and no UBX frame decoded: bus is returning only 0xFF.");
    Serial.println("    Confirm D_SEL = GND (SPI selected) and power-cycle the module,");
    Serial.println("    and that the SX1262 NSS is held high so it isn't driving MISO.");
  }
}
