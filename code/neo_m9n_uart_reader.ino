/*
 * NEO-M9N GNSS reader (UART)  --  limeskey-node / Seeed XIAO ESP32-C6
 *
 * The module runs in UART+I2C mode and streams UBX-NAV-PVT at 1 Hz, 115200 8N1.
 * This reads that stream, validates each UBX checksum, and prints a verbose,
 * human-readable position report once per navigation epoch.
 *
 * Wiring (these pins are shared with the SX1262 SPI bus):
 *   ESP32 GPIO20  <- NEO pin 20 (TXD)    [ESP32 RX]
 *   ESP32 GPIO18  -> NEO pin 21 (RXD)    [ESP32 TX]
 * Hold the radio's NSS high (LORA_NSS_PIN) so the SX1262 releases the shared
 * SDO/MISO line while the GNSS drives it.
 */

#include <HardwareSerial.h>

// ---------------- CONFIG ----------------
#define GNSS          Serial1   // ESP32-C6 UART1
#define GNSS_RX_PIN   20        // ESP32 receives NEO TXD
#define GNSS_TX_PIN   18        // ESP32 transmits to NEO RXD
#define GNSS_BAUD     115200
#define LORA_NSS_PIN  -1        // set to the SX1262 NSS GPIO; -1 to skip
#define PPS_PIN       -1        // set if PPS is broken out; -1 to skip
// ----------------------------------------

// ---- little-endian field readers ----
static inline uint32_t u32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline int32_t  i32(const uint8_t* p){ return (int32_t)u32(p); }
static inline uint16_t u16(const uint8_t* p){ return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }

// ---- transmit a UBX frame (used to poll MON-VER) ----
void sendUBX(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t ckA = 0, ckB = 0;
  auto ck = [&](uint8_t b){ ckA += b; ckB += ckA; };

  uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  ck(cls); ck(id); ck(len & 0xFF); ck(len >> 8);
  for (uint16_t i = 0; i < len; i++) ck(payload[i]);

  GNSS.write(hdr, 6);
  if (len && payload) GNSS.write(payload, len);
  GNSS.write(ckA);
  GNSS.write(ckB);
  GNSS.flush();
}

// ---- decoders ----
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

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== NEO-M9N UART reader (limeskey-node) ===");

  if (LORA_NSS_PIN >= 0) { pinMode(LORA_NSS_PIN, OUTPUT); digitalWrite(LORA_NSS_PIN, HIGH); }
  if (PPS_PIN >= 0) pinMode(PPS_PIN, INPUT);

  GNSS.setRxBufferSize(1024);   // must precede begin(); 1 Hz UBX bursts are large
  GNSS.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
  Serial.printf("Listening on UART1 @ %d 8N1   (RX=GPIO%d, TX=GPIO%d)\n",
                GNSS_BAUD, GNSS_RX_PIN, GNSS_TX_PIN);

  delay(200);
  sendUBX(0x0A, 0x04, nullptr, 0);   // request firmware version once
}

void loop() {
  while (GNSS.available()) ubxFeed((uint8_t)GNSS.read());
}
