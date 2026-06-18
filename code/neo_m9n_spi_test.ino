/*
 * NEO-M9N GNSS bring-up / test sketch  -- SPI mode (D_SEL = GND)
 * Target: Seeed XIAO ESP32-C6  (limeskey-node)
 *
 * What this does:
 *   - Opens SPI to the NEO-M9N
 *   - Polls the module's SPI buffer (u-blox SPI has no addressing; you clock
 *     0xFF out and the module streams UBX/NMEA back, 0xFF = "nothing to send")
 *   - Sends a UBX-MON-VER poll so we can confirm two-way comms + read FW version
 *   - Parses and prints NMEA sentences, decodes RMC/GGA for a fix
 *   - Dumps satellite counts, fix type, HDOP, lat/lon/alt
 *   - Prints raw bytes if you enable RAW_DUMP so you can see if the bus is alive
 *
 * Wiring assumptions (CONFIRM against your board):
 *   SCK    -> GPIO19 (D8)
 *   MISO   -> GPIO20 (D9)
 *   MOSI   -> GPIO18 (D10)
 *   CS_GPS -> GPIO21 (D3)   <-- VERIFY. Defaulted; change CS_PIN if wrong.
 *   PPS    -> optional, set PPS_PIN if you broke it out and want to watch it
 *
 *   D_SEL strapped to GND on the board => SPI selected. If you ever read all
 *   0xFF forever, first thing to suspect is D_SEL not actually low, or CS_PIN
 *   wrong, or VCC/V_BCKP missing.
 */

#include <SPI.h>

// ---------------- USER CONFIG ----------------
#define CS_PIN     2      // CS_GPS. VERIFY against schematic net "CS_GPS".
#define SCK_PIN    19
#define MISO_PIN   20
#define MOSI_PIN   18
#define PPS_PIN    -1      // set to the GPIO if PPS is wired, else -1 to skip

#define SPI_HZ     1000000 // NEO-M9N max SPI clock 5.5 MHz; 1 MHz is safe for bring-up
#define RAW_DUMP   1       // 1 = also print raw hex of every byte read (very noisy)
#define POLL_MS    1000    // how often to poll the buffer
// ---------------------------------------------

SPISettings gnssSpi(SPI_HZ, MSBFIRST, SPI_MODE0); // u-blox SPI: CPOL=0, CPHA=0

// ---- small helpers ----
static inline void csLow()  { digitalWrite(CS_PIN, LOW);  }
static inline void csHigh() { digitalWrite(CS_PIN, HIGH); }

// Line buffer for NMEA assembly
char   nmeaBuf[120];
uint8_t nmeaLen = 0;

// Stats
unsigned long bytesRead = 0;
unsigned long ffRun     = 0;   // consecutive 0xFF count (idle indicator)
unsigned long nmeaCount = 0;
unsigned long ubxCount  = 0;

// ---- UBX frame builder (for MON-VER poll) ----
void sendUBX(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t ckA = 0, ckB = 0;
  auto ck = [&](uint8_t b){ ckA += b; ckB += ckA; };

  uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  ck(cls); ck(id); ck(len & 0xFF); ck(len >> 8);
  for (uint16_t i = 0; i < len; i++) ck(payload[i]);

  SPI.beginTransaction(gnssSpi);
  csLow();
  for (int i = 0; i < 6; i++)  SPI.transfer(hdr[i]);
  for (uint16_t i = 0; i < len; i++) SPI.transfer(payload[i]);
  SPI.transfer(ckA);
  SPI.transfer(ckB);
  csHigh();
  SPI.endTransaction();
}

// ---- NMEA helpers ----
bool nmeaChecksumOK(const char* s) {
  // s starts at '$', ends before CR/LF; format $....*HH
  const char* star = strchr(s, '*');
  if (!star) return false;
  uint8_t cs = 0;
  for (const char* p = s + 1; p < star; p++) cs ^= (uint8_t)*p;
  unsigned int given = strtoul(star + 1, nullptr, 16);
  return cs == (given & 0xFF);
}

void decodeRMC(char* s) {
  // $..RMC,time,status,lat,N/S,lon,E/W,speed,course,date,...
  char* tok; char* save;
  int f = 0;
  char status = '?'; char latH[16]="", ns=' ', lonH[16]="", ew=' ';
  char timeH[16]="", dateH[16]="", spd[16]="";
  for (tok = strtok_r(s, ",", &save); tok; tok = strtok_r(nullptr, ",", &save), f++) {
    switch (f) {
      case 1: strncpy(timeH, tok, 15); break;
      case 2: status = tok[0]; break;
      case 3: strncpy(latH, tok, 15); break;
      case 4: ns = tok[0]; break;
      case 5: strncpy(lonH, tok, 15); break;
      case 6: ew = tok[0]; break;
      case 7: strncpy(spd, tok, 15); break;
      case 9: strncpy(dateH, tok, 15); break;
    }
  }
  Serial.printf("   [RMC] status=%c (A=valid,V=no fix)  time=%s date=%s\n",
                status, timeH, dateH);
  if (status == 'A') {
    Serial.printf("   [RMC] lat=%s %c   lon=%s %c   speed(kn)=%s\n",
                  latH, ns, lonH, ew, spd);
  }
}

void decodeGGA(char* s) {
  // $..GGA,time,lat,N/S,lon,E/W,fixqual,numSV,HDOP,alt,M,...
  char* tok; char* save; int f = 0;
  char fixq[8]="", sv[8]="", hdop[8]="", alt[16]="";
  for (tok = strtok_r(s, ",", &save); tok; tok = strtok_r(nullptr, ",", &save), f++) {
    switch (f) {
      case 6: strncpy(fixq, tok, 7);  break;
      case 7: strncpy(sv,   tok, 7);  break;
      case 8: strncpy(hdop, tok, 7);  break;
      case 9: strncpy(alt,  tok, 15); break;
    }
  }
  Serial.printf("   [GGA] fixQual=%s (0=none,1=GPS,2=DGPS)  satsUsed=%s  HDOP=%s  alt=%s m\n",
                fixq, sv, hdop, alt);
}

void handleNMEA(char* s) {
  nmeaCount++;
  bool ok = nmeaChecksumOK(s);
  Serial.printf(" NMEA%s: %s\n", ok ? "" : " (BAD CKSUM)", s);
  if (!ok) return;
  if (strstr(s, "RMC")) { char tmp[120]; strncpy(tmp,s,119); tmp[119]=0; decodeRMC(tmp); }
  if (strstr(s, "GGA")) { char tmp[120]; strncpy(tmp,s,119); tmp[119]=0; decodeGGA(tmp); }
}

// Read whatever is in the module's SPI buffer right now.
void pollBuffer() {
  SPI.beginTransaction(gnssSpi);
  csLow();
  // Read a generous chunk. The module returns 0xFF when empty.
  for (int i = 0; i < 256; i++) {
    uint8_t b = SPI.transfer(0xFF);
    bytesRead++;

    if (b == 0xFF) { ffRun++; continue; }
    ffRun = 0;

    if (RAW_DUMP) Serial.printf("%02X ", b);

    // UBX frame start?
    if (b == 0xB5) {
      uint8_t b2 = SPI.transfer(0xFF); bytesRead++;
      if (b2 == 0x62) {
        ubxCount++;
        uint8_t cls = SPI.transfer(0xFF);
        uint8_t id  = SPI.transfer(0xFF);
        uint8_t lo  = SPI.transfer(0xFF);
        uint8_t hi  = SPI.transfer(0xFF);
        uint16_t len = lo | (hi << 8);
        bytesRead += 4;
        Serial.printf(" UBX  cls=0x%02X id=0x%02X len=%u", cls, id, len);
        // MON-VER (0x0A 0x04) payload is ASCII version strings
        if (cls == 0x0A && id == 0x04) {
          Serial.print("  -> MON-VER: ");
          for (uint16_t k = 0; k < len && k < 200; k++) {
            uint8_t c = SPI.transfer(0xFF); bytesRead++;
            if (c >= 32 && c < 127) Serial.write(c);
          }
        } else {
          for (uint16_t k = 0; k < len; k++) { SPI.transfer(0xFF); bytesRead++; }
        }
        SPI.transfer(0xFF); SPI.transfer(0xFF); bytesRead += 2; // 2 ck bytes
        Serial.println();
      }
      continue;
    }

    // NMEA assembly
    if (b == '$') { nmeaLen = 0; nmeaBuf[nmeaLen++] = b; }
    else if (nmeaLen > 0) {
      if (b == '\r' || b == '\n') {
        if (nmeaLen > 6) { nmeaBuf[nmeaLen] = 0; handleNMEA(nmeaBuf); }
        nmeaLen = 0;
      } else if (nmeaLen < sizeof(nmeaBuf) - 1) {
        nmeaBuf[nmeaLen++] = b;
      } else {
        nmeaLen = 0; // overflow, drop
      }
    }
  }
  csHigh();
  SPI.endTransaction();
  if (RAW_DUMP) Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== NEO-M9N SPI bring-up test (limeskey-node) ===");

  pinMode(CS_PIN, OUTPUT);
  csHigh();
  if (PPS_PIN >= 0) pinMode(PPS_PIN, INPUT);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  Serial.printf("SPI up: SCK=%d MISO=%d MOSI=%d CS=%d @ %d Hz, MODE0\n",
                SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN, SPI_HZ);
  Serial.println("If you see ONLY 0xFF / no NMEA: check D_SEL=GND, CS pin, VCC, V_BCKP.\n");

  delay(200);
  // Poll firmware version to confirm two-way comms
  Serial.println(">> Polling UBX-MON-VER ...");
  sendUBX(0x0A, 0x04, nullptr, 0);
  delay(100);
}

unsigned long lastPoll = 0;
unsigned long lastStat = 0;

void loop() {
  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    pollBuffer();
  }

  // Health summary every 5 s
  if (millis() - lastStat >= 5000) {
    lastStat = millis();
    Serial.println("------------------------------------------------");
    Serial.printf("STATS  bytes=%lu  NMEA=%lu  UBX=%lu  idleFFrun=%lu\n",
                  bytesRead, nmeaCount, ubxCount, ffRun);
    if (nmeaCount == 0 && ubxCount == 0)
      Serial.println("  !! No NMEA/UBX yet. Bus may be silent. See checklist above.");
    if (PPS_PIN >= 0)
      Serial.printf("  PPS pin state = %d (toggles ~1Hz only after a fix)\n",
                    digitalRead(PPS_PIN));
      Serial.println("------------------------------------------------");
  }
}
