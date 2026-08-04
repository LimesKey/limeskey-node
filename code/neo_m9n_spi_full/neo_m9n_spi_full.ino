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
 * Decodes NAV-PVT, MON-VER, MON-RF, MON-SPAN, MON-GNSS, NAV-SAT and CFG-VALGET.
 *
 * ---------------------------------------------------------------------------
 * CURRENT DIAGNOSTIC STATE (2026-07)
 *
 * Front end is confirmed healthy and confirmed fed:
 *
 *                     AGC            MON-SPAN PGA   Noise   magI/magQ
 *   good antenna      1755 (21%)     15 dB          100     172/168
 *   weak antenna      3510 (43%)     30 dB           95     160/155
 *   no antenna        5265 (64%)     45 dB           94     152/144
 *
* antenna on 2106 (26%), antenna off ~5980 (73%), PGA 18 dB, magI/magQ ~37/37.
*
 * Three independent loops agree that the good antenna's LNA is delivering: AGC,
 * PGA, and the swap between the two antennas moving both by the same ratio. The
 * MON-SPAN passband is correctly positioned over L1. And NAV-SAT still reports
 * zero satellites outdoors.
 *
 * Every healthy reading above is a BROADBAND POWER measurement. AGC, PGA,
 * noisePerMS, magI/magQ and MON-SPAN all work correctly regardless of what
 * frequency the internal TCXO is actually running at. Correlation is the only
 * thing that depends on the reference being right, and correlation is the only
 * thing failing. Nothing in the module measures its own reference.
 *
 * At L1, 1 ppm of oscillator error is 1575 Hz of apparent Doppler. Cold-start
 * frequency search windows are order +/-10 kHz, so past roughly 6 ppm satellites
 * start dropping out and past 20 ppm acquisition is impossible. A healthy TCXO
 * is inside +/-2 ppm. This module has been through repeated hot-air rework.
 *
 * So this build adds three things:
 *
 *   1. TP TEST      Route the raw local oscillator to TIMEPULSE so the TCXO
 *                   error can be measured on external gear. See the TP TEST
 *                   block. This is the primary experiment.
 *   2. CFG READBACK Poll the per-signal CFG-SIGNAL-* keys individually.
 *                   MON-GNSS reports major constellation enables only; it does
 *                   NOT report GPS_L1CA_ENA. On an L1-only M9N, GPS_ENA=1 with
 *                   GPS_L1CA_ENA=0 gives exactly the symptom seen here.
 *   3. AIDING       Inject rough UTC time (from NTP over WiFi, then WiFi off
 *                   again before acquisition) with UBX-MGA-INI-TIME_UTC, and
 *                   optionally position with MGA-INI-POS_LLH. This collapses
 *                   the search space. If it acquires WITH aiding and not
 *                   without, that confirms a search-window problem, which is
 *                   the clock hypothesis from the other direction.
 *
 * Also confirmed by measurement and worth not relearning:
 *   - AGC direction: removing input power makes agcCnt RISE.
 *   - AGC at 21% with a good active antenna is NORMAL. Not evidence of jamming.
 *   - magI/magQ sit after the AGC and stay near 150 either way, so they cannot
 *     detect a missing antenna. Only their ratio is meaningful.
 *   - antStatus reports "OK, power ON" with nothing connected. Useless here,
 *     because ANT_DETECT is not wired and LNA_EN (U2 pin 14) only goes to TP1.
 *   - The single-bin spike at the exact MON-SPAN centre persists with the
 *     antenna removed, so it is internal. The integration manual says the same.
 *   - Measured bias-T current: 46 mV across R13 (10R) = 4.6 mA. The manual
 *     gives 5-20 mA typical for an active antenna, so this is just under range.
 *
 * MON-SPAN amplitudes are uint8 with no documented dB-per-LSB, so they are
 * printed as COUNTS, not dB. A "115 count" peak-to-trough is a normal SAW
 * shape of roughly 25-30 dB, not a catastrophe. Do not compare traces across
 * captures with different PGA values.
 * ---------------------------------------------------------------------------
 *
 * Wiring (matches the SPI bring-up net list):
 *   SCK    -> GPIO19      MISO -> GPIO20      MOSI -> GPIO18
 *   CS_GPS -> GPIO2  (through R3)
 *   TIMEPULSE -> JP1 / JP2 (probe here for the TP TEST)
 *
 * RF QUIET: this sketch is a GNSS-only bring-up tool, so it shuts down every
 * other transmitter on the board. ESP32-C6 WiFi and BLE are stopped and the BLE
 * controller's RAM is handed back to the heap, which makes BLE unstartable until
 * the next reset. The E22P is put in its CLOSE state (EN low) and the SX1262 is
 * put in cold-start sleep, optionally then held in reset. The E22P VCC rail
 * itself stays up: U5's EN is strapped to its own VIN.
 *
 * ORDERING MATTERS with aiding enabled. WiFi is a 2.4 GHz transmitter sitting a
 * few mm from the GNSS antenna feed. The sequence is: LoRa pins safe -> WiFi up
 * -> NTP -> WiFi fully down and deinitialised -> everything else. No GNSS
 * measurement in this sketch is ever taken with the WiFi PHY powered.
 *
 *   LoRa EN   -> GPIO17 (R17 10k pull-down)     LoRa NRST -> GPIO16 (R15 10k to +5V)
 *   LoRa NSS  -> GPIO21 (through R4)            LoRa BUSY -> GPIO7
 *   LoRa DIO1 -> GPIO1                          T/R CTRL  -> DIO2 only, no MCU access
 */

#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include "esp_wifi.h"
#if defined(CONFIG_BT_ENABLED)
#include "esp_bt.h"
#endif

// GPIO16/GPIO17 are U0TXD/U0RXD on the C6 and are wired to LoRa NRST and EN on
// this board. If Serial lands on UART0 instead of USB CDC, the console would be
// bit-banging the radio's reset line.
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT == 0
#warning "Serial is on UART0 (GPIO16/17) = LoRa NRST/EN. Enable USB CDC on boot."
#endif

// ================= CREDENTIALS =================
//
// This repository is PUBLIC (github.com/LimesKey/limeskey-node, GPL-3.0). Do not
// commit a real SSID and password. Once this builds, move them into a local
// secrets.h and add that file to .gitignore:
//
//     // secrets.h  (git-ignored)
//     #define WIFI_SSID "your-ssid"
//     #define WIFI_PASS "your-password"
//
// If secrets.h exists it wins; the fallback below is only so this compiles now.
// Also worth knowing: WiFi credentials written by esp_wifi are cached in the
// ESP32's NVS partition, so `esptool.py erase_flash` before handing the board to
// anyone or posting flash dumps.
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "Dragon"
#define WIFI_PASS "strawberry"
#endif

// ---------------- CONFIG ----------------
#define CS_PIN 2  // CS_GPS (GPIO2, through R3)
#define SCK_PIN 19
#define MISO_PIN 20
#define MOSI_PIN 18
#define SPI_HZ 250000   // 250 kHz: under the 125 kB/s data ceiling for contiguous frames
#define PPS_PIN -1      // set if PPS is broken out; -1 to skip
#define DRAIN_MS 250    // how often to drain the module's SPI buffer
#define DRAIN_MAX 4096  // safety cap on bytes clocked per drain
#define IDLE_RUN 4      // consecutive 0xFF at a packet boundary => buffer empty
#define DIAG_MS 10000   // how often to poll MON-RF + MON-SPAN + NAV-SAT
#define SPAN_COLS 64    // ASCII spectrum width
#define SPAN_ROWS 16    // ASCII spectrum height
#define NAV_PVT_RATE 5  // NAV-PVT every Nth epoch; 1 Hz buries the diagnostics

// CFG WRITE PERSISTENCE.
// The previous build wrote every config change to layers RAM|BBR|Flash (0x07) on
// EVERY BOOT. Flash on the M9 has finite write endurance and a flash-layer write
// is slow, so a bring-up loop that reflashes on each reset is burning the part
// for no reason. RAM-only is the right default for experiments: it survives
// until power-down, which is the whole session. Set to 1 only when a setting has
// earned permanence.
#define CFG_PERSIST 0
#if CFG_PERSIST
#define CFG_LAYERS 0x07  // RAM | BBR | Flash
#else
#define CFG_LAYERS 0x01  // RAM only
#endif

// AGC thresholds, measured on THIS board with THIS antenna (see header notes).
// AGC_HOT was 1000, but the good antenna's observed floor is 1053, which would
// have tripped the warning constantly. 800 leaves margin.
#define AGC_NO_ANTENNA 4500  // at or above this, nothing is feeding the front end
#define AGC_HOT 800          // below this, more power than the LNA alone explains

#define ENABLE_ITFM 1    // switch the interference monitor on at boot
#define ITFM_ANT_TYPE 2  // 0 = unknown, 1 = passive, 2 = active
#define GPS_ONLY 1       // disable GLO/BDS/GAL: all four split the correlators
// ----------------------------------------

// ================= TP TEST (TCXO frequency measurement) =================
//
// THE POINT: CFG-TP-SYNC_GNSS_TP1 = 0 makes the time pulse run off the LOCAL
// OSCILLATOR instead of GNSS-derived frequency. That routes the TCXO, divided
// down, straight to a pin. Fractional error at the pin equals fractional error
// of the reference, so 4 MHz reading 4.000120 MHz is 30 ppm and the diagnosis
// is over.
//
// MEASURE IT ON A FREQUENCY COUNTER, NOT THE SCOPE. A scope timebase is
// typically +/-25 to +/-50 ppm, the same order as the fault. The scope can only
// catch a gross failure in the hundreds of ppm. Humber's bench counters are
// reciprocal-interpolating and will resolve sub-ppm in a 1 s gate; the CXA's
// frequency reference also works.
//
// Frequency ceiling: the integration manual says neither the high nor the low
// period may be under 50 ns, which caps 50% duty at 10 MHz. 4 MHz has margin.
// FREQ_TP1 is unlikely to be an exact integer divisor of the internal clock, so
// the output will be dithered cycle to cycle. A counter averages that out; a
// single-shot period measurement will not.
#define TP_TEST 1
#define TP_TEST_FREQ_HZ 4000000UL
#define TP_TEST_DUTY 50.0  // percent, sent as R8 double (NOT an integer)
// ========================================================================

// ================= AIDING (MGA-INI) =================
// Injects rough UTC time, and optionally position, to collapse the acquisition
// search space. Time comes from NTP; WiFi is brought up and then fully torn down
// before any GNSS measurement.
#define ENABLE_AIDING 1
#define WIFI_TIMEOUT_MS 15000
#define NTP_TIMEOUT_MS 10000
#define AID_LEAP_SECS 18  // UTC-GPS offset, 18 since 2017-01-01

// Position aiding. Deliberately OFF by default: this is a public repo and your
// home coordinates would be committed in plaintext. Fill these in locally only,
// or put them in secrets.h alongside the WiFi credentials. +/-5 km is plenty of
// accuracy to be useful, so round them off; you do not need your driveway.
#define AID_POS 0
#ifndef AID_LAT_DEG
#define AID_LAT_DEG 0.0
#define AID_LON_DEG 0.0
#define AID_ALT_M 100
#endif
#define AID_POS_ACC_M 5000
// ====================================================

// ------------- FACTORY RESET -------------
// DO_FACTORY_RESET    compile the reset path in at all
// RESET_TOKEN         latched in NVS; change the string to arm another run
// RESET_KEEP_SPI      re-set CFG-SPI-ENABLED=1 after the wipe. The M9 default for
//                     this key is 0 (false). Whether D_SEL=GND overrides that at
//                     the port level is not documented, and this board has no
//                     UART/I2C fallback, so leaving it at 1 is the safe choice.
//                     This one write DOES go to flash, deliberately: if SPI does
//                     not survive the next power cycle the board is unreachable.
// RESET_ENABLE_NAV_PVT  also re-enable NAV-PVT on SPI (default is 0; NMEA GGA
//                     defaults to 1, which this UBX-only parser will ignore).
#define DO_FACTORY_RESET 0
#define RESET_TOKEN "fr-2026-07-a"
#define RESET_KEEP_SPI 1
#define RESET_ENABLE_NAV_PVT 1
// ----------------------------------------

SPISettings gnssSpi(SPI_HZ, MSBFIRST, SPI_MODE0);  // u-blox: CPOL=0, CPHA=0

static inline void csLow() {
  digitalWrite(CS_PIN, LOW);
}
static inline void csHigh() {
  digitalWrite(CS_PIN, HIGH);
}

// ---- little-endian field readers ----
static inline uint32_t u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t i32(const uint8_t* p) {
  return (int32_t)u32(p);
}
static inline uint16_t u16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool gotData = false;  // set once any valid UBX frame is decoded

// ---- parser health counters -------------------------------------------------
// The 0xA8 history on this board means a lossy SPI read is a live possibility,
// and a lossy read looks exactly like "no satellites" if a variable-length
// message such as NAV-SAT gets mangled. These make that visible instead of
// silent: a rising ubxBad with a healthy ubxGood means bytes are being dropped
// mid-frame, and nothing downstream of the parser can be trusted.
uint32_t ubxGood = 0, ubxBad = 0, ubxOverflow = 0;

// ACK/NAK latch, consumed by sendWaitAck()
volatile bool ackSeen = false, ackOk = false;
volatile uint8_t ackCls = 0, ackId = 0;

// forward declarations (these are called from setup(), which appears before them
// in this file; the .ino preprocessor usually inserts prototypes but relying on
// that is how you get a build that breaks when someone renames the file)
void drainGNSS();
void sendUBX(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len);
bool sendWaitAck(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len, uint32_t timeoutMs);
bool ubxSetGpsOnly();
bool ubxEnableItfm(uint8_t antType);
bool ubxConfigureTimepulse();
void ubxDumpCriticalKeys();
void factoryResetOnce();
bool ntpSync();
void injectAiding();

// ---- decoders ----
const char* fixName(uint8_t t) {
  switch (t) {
    case 0: return "no fix";
    case 1: return "dead reckoning";
    case 2: return "2D";
    case 3: return "3D";
    case 4: return "GNSS + dead reckoning";
    case 5: return "time only";
    default: return "unknown";
  }
}

const char* gnssName(uint8_t id) {
  switch (id) {
    case 0: return "GPS";
    case 1: return "SBAS";
    case 2: return "GAL";
    case 3: return "BDS";
    case 4: return "IMES";
    case 5: return "QZSS";
    case 6: return "GLO";
    default: return "?";
  }
}

// UBX-NAV-PVT (class 0x01 id 0x07, 92-byte payload)
void printNavPvt(const uint8_t* p) {
  uint16_t year = u16(p + 4);
  uint8_t mon = p[6], day = p[7];
  uint8_t hh = p[8], mm = p[9], ss = p[10];
  uint8_t valid = p[11];
  uint8_t fixType = p[20], flags = p[21], numSV = p[23];
  bool fixOK = flags & 0x01;
  int32_t lon = i32(p + 24), lat = i32(p + 28);
  int32_t hEllip = i32(p + 32), hMSL = i32(p + 36);
  uint32_t hAcc = u32(p + 40), vAcc = u32(p + 44);
  int32_t gSpeed = i32(p + 60), headMot = i32(p + 64);
  uint32_t sAcc = u32(p + 68);
  uint16_t pDOP = u16(p + 76);

  bool vDate = valid & 0x01, vTime = valid & 0x02, resolved = valid & 0x04;

  Serial.println();
  Serial.printf("[GNSS] %04u-%02u-%02u %02u:%02u:%02u UTC   (date %s, time %s%s)\r\n",
                year, mon, day, hh, mm, ss,
                vDate ? "valid" : "invalid",
                vTime ? "valid" : "invalid",
                resolved ? ", fully resolved" : "");

  Serial.printf("       Fix: %s%s   satellites: %u   pDOP: %.2f\r\n",
                fixName(fixType), fixOK ? "" : " (not usable)", numSV, pDOP / 100.0);

  if (fixOK) {
    Serial.printf("       Position: %.7f, %.7f\r\n", lat * 1e-7, lon * 1e-7);
    Serial.printf("       Altitude: %.2f m MSL   (%.2f m ellipsoid)\r\n",
                  hMSL / 1000.0, hEllip / 1000.0);
    Serial.printf("       Motion:   %.2f m/s   heading %.1f deg\r\n",
                  gSpeed / 1000.0, headMot * 1e-5);
    Serial.printf("       Accuracy: horiz %.2f m   vert %.2f m   speed %.2f m/s\r\n",
                  hAcc / 1000.0, vAcc / 1000.0, sAcc / 1000.0);
  } else {
    Serial.println("       Position: not available (waiting on satellites / antenna)");
  }

  // With aiding injected, a plausible date here and validTime set is proof the
  // MGA-INI-TIME_UTC landed, without needing MGA-ACK enabled.
#if ENABLE_AIDING
  if (year >= 2026 && vTime)
    Serial.println("       >> time is valid and plausible: aiding was accepted.");
#endif
}

// UBX-MON-VER (class 0x0A id 0x04): 30-byte SW, 10-byte HW, then 30-byte extensions
void printMonVer(const uint8_t* p, uint16_t len) {
  char buf[31];
  Serial.println("\r\n[GNSS] Firmware (UBX-MON-VER):");
  if (len >= 30) {
    memcpy(buf, p, 30);
    buf[30] = 0;
    Serial.printf("       SW: %s\r\n", buf);
  }
  if (len >= 40) {
    memcpy(buf, p + 30, 10);
    buf[10] = 0;
    Serial.printf("       HW: %s\r\n", buf);
  }
  for (uint16_t o = 40; o + 30 <= len; o += 30) {
    memcpy(buf, p + o, 30);
    buf[30] = 0;
    if (buf[0]) Serial.printf("       %s\r\n", buf);
  }
}

// UBX-MON-RF (class 0x0A id 0x38): 4-byte header + 24 bytes per RF block.
//
// antStatus is NOT usable on this board. Open-circuit detection needs ANT_DETECT
// wired, and LNA_EN (U2 pin 14) only goes to TP1, so it controls nothing. Measured
// behaviour: the field reports "OK, power ON" with the antenna physically removed.
// Ignore it. AGC is the only real antenna-presence signal available here.
//
// magI/magQ are sampled AFTER the AGC, which holds the ADC level constant, so they
// track the AGC target rather than input power. Measured: ~152/144 with no antenna
// attached at all. A healthy magnitude proves nothing about the antenna path. Only
// the ratio between them carries information.
//
// ofsI/ofsQ: four samples on this board read 22/13, 29/18, 22/17, 22/17. That is
// scatter around 23/16, not a trend. u-blox publishes no healthy range, so this is
// unknown rather than bad. Do not chase it without a reference module to compare.
void printMonRf(const uint8_t* p, uint16_t len) {
  if (len < 4) return;
  uint8_t nBlocks = p[1];
  static const char* antSt[] = { "INIT", "DONTKNOW", "OK", "SHORT", "OPEN" };
  static const char* antPw[] = { "OFF", "ON", "DONTKNOW" };
  static const char* jamSt[] = { "unknown/disabled", "ok", "warning", "critical" };

  Serial.println("\r\n[GNSS] RF status (UBX-MON-RF):");
  for (uint8_t n = 0; n < nBlocks; n++) {
    uint16_t b = 4 + (uint16_t)n * 24;
    if (b + 24 > len) break;
    uint8_t blockId = p[b];
    uint8_t jam = p[b + 1] & 0x03;
    uint8_t aSt = p[b + 2], aPw = p[b + 3];
    uint16_t noise = u16(p + b + 12);
    uint16_t agc = u16(p + b + 14);
    uint8_t jamInd = p[b + 16];
    int8_t ofsI = (int8_t)p[b + 17];
    uint8_t magI = p[b + 18];
    int8_t ofsQ = (int8_t)p[b + 19];
    uint8_t magQ = p[b + 20];

    Serial.printf("       block %u (%s band)\r\n", blockId, blockId == 0 ? "L1" : "L2/L5");
    Serial.printf("         AGC:   %u / 8191  (%.0f%%)\r\n", agc, agc * 100.0 / 8191.0);
    Serial.printf("         Noise: %u   CW jam ind: %u/255   state: %s\r\n",
                  noise, jamInd, jamSt[jam]);
    Serial.printf("         I/Q:   mag %u/%u   offset %d/%d\r\n", magI, magQ, ofsI, ofsQ);
    Serial.printf("         Ant:   %s, power %s\r\n",
                  aSt <= 4 ? antSt[aSt] : "?", aPw <= 2 ? antPw[aPw] : "?");

    // Running extremes for block 0. u-blox publishes no healthy range for AGC or
    // noisePerMS, so the only way to read them is against your own baseline.
    if (blockId == 0) {
      static uint16_t agcLo = 0xFFFF, agcHi = 0, nseLo = 0xFFFF, nseHi = 0;
      if (agc < agcLo) agcLo = agc;
      if (agc > agcHi) agcHi = agc;
      if (noise < nseLo) nseLo = noise;
      if (noise > nseHi) nseHi = noise;
      Serial.printf("         Range since boot:  AGC %u..%u   Noise %u..%u\r\n",
                    agcLo, agcHi, nseLo, nseHi);
    }

    if (agc > AGC_NO_ANTENNA)
      Serial.println("         >> AGC at the no-antenna baseline: the element is not "
                     "delivering. Connector, cable, or bias-T voltage. antStatus is "
                     "not wired for open detect here, so its OK means nothing.");
    else if (agc < AGC_HOT)
      Serial.println("         >> AGC below the connected baseline: more power at the "
                     "input than the antenna LNA alone accounts for. This also "
                     "suppresses Noise and jam ind downstream, so a low CW reading "
                     "here proves nothing.");

    // Only the I/Q ratio is meaningful; the absolute magnitudes follow the AGC.
    if (magI && magQ) {
      int d = (int)magI - (int)magQ;
      if (d < 0) d = -d;
      int imb = d * 200 / ((int)magI + (int)magQ);
      if (imb > 15)
        Serial.printf("         >> I/Q magnitudes %d%% apart: front-end imbalance\r\n", imb);
    }

    // The reference spectrum ITFM compares against is only captured after a first
    // good fix, so on a receiver that has never fixed, jamInd has no baseline.
    if (jam == 0)
      Serial.println("         >> ITFM off or not yet calibrated, so the CW jam ind "
                     "above is uncalibrated. It also never sees broadband noise.");
  }
}

// UBX-NAV-SAT (class 0x01 id 0x35): 8-byte header + 12 bytes per SV
//
// NOTE on an empty list. NAV-SAT reports SVs "either known to be visible or
// currently tracked". With empty BBR there is no almanac, so nothing is known
// visible, and if nothing is tracked then numSvs = 0 is what a CORRECTLY
// FUNCTIONING receiver reports while blind-searching and finding nothing. An
// empty list does NOT by itself distinguish a config fault from a hardware
// fault. What it does mean, sustained over many minutes under open sky with a
// confirmed-good front end, is that no correlation is happening.
void printNavSat(const uint8_t* p, uint16_t len) {
  if (len < 8) return;
  uint8_t n = p[5];
  uint8_t tracked = 0, used = 0, best = 0;
  uint32_t cnoSum = 0;

  // Raw length is printed so "0 satellites" can be distinguished from a
  // truncated or mis-parsed frame. A genuine empty report is exactly 8 bytes.
  Serial.printf("\r\n[GNSS] Satellites (UBX-NAV-SAT): %u reported "
                "(payload %u B, expected %u)\r\n",
                n, len, 8 + n * 12);
  if (len != (uint16_t)(8 + n * 12))
    Serial.println("       >> LENGTH MISMATCH: this frame is truncated. The SPI read "
                   "is lossy and the satellite count below is meaningless.");

  for (uint8_t i = 0; i < n; i++) {
    uint16_t b = 8 + (uint16_t)i * 12;
    if (b + 12 > len) break;
    uint8_t gid = p[b], sv = p[b + 1], cno = p[b + 2];
    int8_t elev = (int8_t)p[b + 3];
    uint32_t fl = u32(p + b + 8);
    uint8_t qual = fl & 0x07;
    bool svUsed = fl & 0x08;
    uint8_t orb = (fl >> 8) & 0x07;

    if (cno > 0) {
      tracked++;
      cnoSum += cno;
      if (cno > best) best = cno;
    }
    if (svUsed) used++;

    if (cno > 0 || qual > 1)
      Serial.printf("       %-4s %-3u  C/N0 %2u dB-Hz  elev %3d  q=%u%s%s\r\n",
                    gnssName(gid), sv, cno, elev, qual,
                    svUsed ? "  USED" : "",
                    orb == 1 ? "  eph" : (orb == 2 ? "  alm" : ""));
  }

  Serial.printf("       ---- %u with signal, %u used, best %u dB-Hz, mean %u dB-Hz\r\n",
                tracked, used, best, tracked ? (unsigned)(cnoSum / tracked) : 0);

  if (n == 0)
    Serial.println("       >> empty list. Consistent with blind search finding nothing. "
                   "Front end is confirmed good, so sustained emptiness under open sky "
                   "means no correlation: check the TP TEST result.");
  else if (tracked == 0)
    Serial.println("       >> SVs listed but zero signal on all of them.");
  else if (best < 25)
    Serial.println("       >> signals present but weak. Ephemeris download needs about "
                   "32 dB-Hz sustained for 18-30 s per SV without losing lock.");
  else if (best >= 32)
    Serial.println("       >> above the 32 dB-Hz nav-data threshold. Leave it alone and "
                   "untouched for 20 minutes; every reset restarts the cold start.");
}

// UBX-MON-SPAN (class 0x0A id 0x31): 4-byte header + 272 bytes per RF block.
// Per the interface description, bin centre frequencies are
//   f(i) = center + span * (i - 128) / 256
// Amplitudes are relative uint8 COUNTS with no documented dB-per-LSB, and they
// exclude both the PGA gain and the fixed internal LNA gain. Comparative only,
// and only within a single capture: PGA changes rescale everything.
void printMonSpan(const uint8_t* p, uint16_t len) {
  if (len < 4) return;
  uint8_t nBlocks = p[1];

  for (uint8_t n = 0; n < nBlocks; n++) {
    uint32_t b = 4 + (uint32_t)n * 272;
    if (b + 272 > len) break;
    const uint8_t* spec = p + b;
    uint32_t span = u32(p + b + 256);
    uint32_t res = u32(p + b + 260);
    uint32_t center = u32(p + b + 264);
    uint8_t pga = p[b + 268];

    uint16_t nPts = (res && span) ? (uint16_t)(span / res) : 256;
    if (nPts > 256) nPts = 256;
    // Disabling constellations narrows the span, which can drop nPts below the
    // column count. Clamping here means only the first SPAN_COLS bins get
    // plotted, so the trace is real but truncated. Flagged rather than hidden.
    bool truncated = false;
    if (nPts < SPAN_COLS) {
      nPts = SPAN_COLS;
      truncated = true;
    }
    uint8_t group = nPts / SPAN_COLS;
    if (group == 0) group = 1;

    // collapse the bins into columns, keeping the peak of each group so a
    // single narrow spur cannot be averaged away
    uint8_t col[SPAN_COLS];
    uint8_t lo = 255, hi = 0;
    for (uint8_t c = 0; c < SPAN_COLS; c++) {
      uint8_t m = 0;
      for (uint8_t k = 0; k < group; k++) {
        uint8_t v = spec[(uint16_t)c * group + k];
        if (v > m) m = v;
      }
      col[c] = m;
      if (m > hi) hi = m;
      if (m < lo) lo = m;
    }
    if (hi == lo) hi = lo + 1;  // flat trace, avoid divide by zero

    Serial.printf("\r\n[GNSS] Spectrum (UBX-MON-SPAN) block %u\r\n", n);
    Serial.printf("       center %.3f MHz   span %.3f MHz   res %u kHz   PGA %u dB\r\n",
                  center / 1e6, span / 1e6, (unsigned)(res / 1000), pga);
    Serial.printf("       trace %u to %u counts, peak-to-trough %u counts "
                  "(relative, NOT dB)\r\n",
                  lo, hi, hi - lo);
    if (truncated)
      Serial.println("       >> span narrower than the plot width: only the first "
                     "64 bins are shown.");

    for (int8_t r = SPAN_ROWS - 1; r >= 0; r--) {
      uint8_t thr = lo + (uint16_t)(hi - lo) * (r + 1) / SPAN_ROWS;
      Serial.printf("  %3u |", thr);
      for (uint8_t c = 0; c < SPAN_COLS; c++) Serial.print(col[c] >= thr ? '#' : ' ');
      Serial.println();
    }
    Serial.print("      +");
    for (uint8_t c = 0; c < SPAN_COLS; c++) Serial.print(c == SPAN_COLS / 2 ? '^' : '-');
    Serial.println();
    Serial.printf("       %.2f MHz  <-  %.2f MHz  ->  %.2f MHz\r\n",
                  (center - span / 2.0) / 1e6, center / 1e6,
                  (center + span / 2.0) / 1e6);

    if (span) {
      int idx = (int)(((1575.42e6 - (double)center) * 256.0 / (double)span) + 128.5);
      if (idx >= 0 && idx < 256)
        Serial.printf("       GPS L1 (1575.42 MHz) is bin %d, column %d\r\n",
                      idx, idx / group);
      else
        Serial.println("       GPS L1 falls outside this span");
    }
  }
}

// UBX-MON-GNSS (class 0x0A id 0x28), 8-byte payload. Reports which major
// constellations the receiver supports and which are actually switched on.
// Bit 0 = GPS, 1 = GLONASS, 2 = BeiDou, 3 = Galileo.
//
// IMPORTANT LIMIT: this reports major constellation enables ONLY. It does not
// report per-signal keys such as CFG-SIGNAL-GPS_L1CA_ENA. GPS_ENA=1 with
// GPS_L1CA_ENA=0 shows up here as "GPS ENABLED y" and acquires nothing. Use
// ubxDumpCriticalKeys() to see the real state.
void printMonGnss(const uint8_t* p, uint16_t len) {
  if (len < 5) return;
  static const char* sys[] = { "GPS", "GLONASS", "BeiDou", "Galileo" };
  uint8_t sup = p[1], def = p[2], ena = p[3], sim = p[4];

  Serial.println("\r\n[GNSS] Constellations (UBX-MON-GNSS):");
  for (uint8_t i = 0; i < 4; i++)
    Serial.printf("       %-8s  supported %c   default %c   ENABLED %c\r\n",
                  sys[i],
                  ((sup >> i) & 1) ? 'y' : 'n',
                  ((def >> i) & 1) ? 'y' : 'n',
                  ((ena >> i) & 1) ? 'y' : 'n');
  Serial.printf("       max simultaneous: %u\r\n", sim);
  Serial.println("       >> major constellations only. Per-signal enables are NOT "
                 "shown here; see the CFG readback.");

  if ((ena & 0x0F) == 0)
    Serial.println("       >> nothing enabled: the engine is not searching at all. "
                   "Config problem, not RF.");
  else if ((ena & 0x01) == 0)
    Serial.println("       >> GPS is off. Factory default is on, so something set this.");
}

// ================= CFG-VALGET decode =================
//
// Key ID encoding, from the interface description: bits 28-30 give the storage
// size, so values can be walked generically without a size table.
//   0x1 = 1 bit (L, stored as one byte)   0x2 = 1 byte    0x3 = 2 bytes
//   0x4 = 4 bytes                          0x5 = 8 bytes
static uint8_t cfgKeySize(uint32_t key) {
  switch ((key >> 28) & 0x07) {
    case 1:
    case 2: return 1;
    case 3: return 2;
    case 4: return 4;
    case 5: return 8;
    default: return 0;
  }
}

struct CfgKeyName {
  uint32_t key;
  const char* name;
};

// Key IDs cross-checked against gpsd ubxtool's table. The CFG-SIGNAL and CFG-TP
// entries are high confidence. The NAVSPG and PM entries are worth verifying
// against UBX-19035940 for your PROTVER before treating a NAK as a fault: a NAK
// on VALGET means one requested key ID is unknown to the firmware, which is a
// documentation problem, not a hardware problem. They are polled one at a time
// below precisely so a bad ID identifies itself instead of poisoning a batch.
static const CfgKeyName kCfgNames[] = {
  // --- the ones that would produce exactly this symptom ---
  { 0x1031001F, "SIGNAL-GPS_ENA" },
  { 0x10310001, "SIGNAL-GPS_L1CA_ENA" },  // <-- the prime suspect
  { 0x10310020, "SIGNAL-SBAS_ENA" },
  { 0x10310005, "SIGNAL-SBAS_L1CA_ENA" },
  { 0x10310021, "SIGNAL-GAL_ENA" },
  { 0x10310007, "SIGNAL-GAL_E1_ENA" },
  { 0x10310022, "SIGNAL-BDS_ENA" },
  { 0x1031000D, "SIGNAL-BDS_B1_ENA" },
  { 0x10310024, "SIGNAL-QZSS_ENA" },
  { 0x10310012, "SIGNAL-QZSS_L1CA_ENA" },
  { 0x10310025, "SIGNAL-GLO_ENA" },
  { 0x10310018, "SIGNAL-GLO_L1_ENA" },
  // --- would silently suppress or duty-cycle acquisition ---
  { 0x20D00001, "PM-OPERATEMODE" },        // verify ID
  { 0x201100A3, "NAVSPG-INFIL_MINCNO" },   // verify ID
  { 0x201100A4, "NAVSPG-INFIL_MINELEV" },  // verify ID
  { 0x20110021, "NAVSPG-DYNMODEL" },       // verify ID
  // --- link and output, confirms the config path itself works ---
  { 0x10640006, "SPI-ENABLED" },
  { 0x2091000A, "MSGOUT-UBX_NAV_PVT_SPI" },
  { 0x20910016, "MSGOUT-UBX_NAV_SAT_SPI" },  // verify ID
  { 0x1041000D, "ITFM-ENABLE" },
  // --- TP test readback ---
  { 0x10050007, "TP-TP1_ENA" },
  { 0x10050008, "TP-SYNC_GNSS_TP1" },
  { 0x10050009, "TP-USE_LOCKED_TP1" },
  { 0x20050023, "TP-PULSE_DEF" },
  { 0x20050030, "TP-PULSE_LENGTH_DEF" },
  { 0x40050024, "TP-FREQ_TP1" },
};

static const char* cfgKeyName(uint32_t key) {
  for (size_t i = 0; i < sizeof(kCfgNames) / sizeof(kCfgNames[0]); i++)
    if (kCfgNames[i].key == key) return kCfgNames[i].name;
  return "(unknown)";
}

// UBX-CFG-VALGET response (0x06 0x8B): version=1, layer, position, then
// key(U4) + value pairs.
void printCfgValget(const uint8_t* p, uint16_t len) {
  if (len < 4) return;
  static const char* layerName[] = { "RAM", "BBR", "Flash", "?", "?", "?", "?", "Default" };
  uint8_t layer = p[1];

  uint16_t o = 4;
  while (o + 4 <= len) {
    uint32_t key = u32(p + o);
    uint8_t sz = cfgKeySize(key);
    if (!sz || o + 4 + sz > len) break;
    const uint8_t* v = p + o + 4;

    Serial.printf("       [%-7s] 0x%08X %-24s = ",
                  layer <= 7 ? layerName[layer] : "?", key, cfgKeyName(key));
    if (sz == 1) Serial.printf("%u", v[0]);
    else if (sz == 2) Serial.printf("%u", u16(v));
    else if (sz == 4) Serial.printf("%lu", (unsigned long)u32(v));
    else {  // 8 bytes; R8 for the DUTY keys
      double d;
      memcpy(&d, v, 8);
      Serial.printf("%.3f", d);
    }
    Serial.println();
    o += 4 + sz;
  }
}

void onUbxMessage(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len) {
  gotData = true;
  if (cls == 0x01 && id == 0x07 && len >= 92) printNavPvt(p);
  else if (cls == 0x01 && id == 0x35) printNavSat(p, len);
  else if (cls == 0x0A && id == 0x38) printMonRf(p, len);
  else if (cls == 0x0A && id == 0x31) printMonSpan(p, len);
  else if (cls == 0x0A && id == 0x28) printMonGnss(p, len);
  else if (cls == 0x0A && id == 0x04) printMonVer(p, len);
  else if (cls == 0x06 && id == 0x8B) printCfgValget(p, len);
  else if (cls == 0x13 && id == 0x60 && len >= 8) {  // MGA-ACK
    Serial.printf("[GNSS] MGA-%s type=0x%02X\r\n",
                  p[0] == 1 ? "ACK" : "NAK", p[4]);
  } else if (cls == 0x05 && len >= 2) {  // ACK / NAK
    ackSeen = true;
    ackOk = (id == 0x01);
    ackCls = p[0];
    ackId = p[1];
    // VALGET/VALSET NAKs are printed by the callers with the key name attached,
    // so this stays quiet for class 0x06 to avoid doubling every line.
    if (ackCls != 0x06)
      Serial.printf("[GNSS] %s for cls=0x%02X id=0x%02X\r\n",
                    ackOk ? "ACK" : "NAK", p[0], p[1]);
  }
}

// ---- UBX receive parser (running Fletcher checksum; only valid frames dispatched) ----
enum UbxState { WAIT_S1,
                WAIT_S2,
                GET_CLS,
                GET_ID,
                LEN_LO,
                LEN_HI,
                PAYLOAD,
                CK_A,
                CK_B };
static UbxState st = WAIT_S1;
static uint8_t msgCls, msgId, ckA, ckB, rxCkA;
static uint16_t msgLen, payIdx;
static uint8_t payload[1024];  // NAV-SAT is 8 + numSvs*12; 40 SVs = 488 B

static inline bool ubxIdle() {
  return st == WAIT_S1;
}

void ubxFeed(uint8_t b) {
  switch (st) {
    case WAIT_S1:
      if (b == 0xB5) st = WAIT_S2;
      break;
    case WAIT_S2: st = (b == 0x62) ? GET_CLS : WAIT_S1; break;
    case GET_CLS:
      msgCls = b;
      ckA = b;
      ckB = b;
      st = GET_ID;
      break;
    case GET_ID:
      msgId = b;
      ckA += b;
      ckB += ckA;
      st = LEN_LO;
      break;
    case LEN_LO:
      msgLen = b;
      ckA += b;
      ckB += ckA;
      st = LEN_HI;
      break;
    case LEN_HI:
      msgLen |= (uint16_t)b << 8;
      ckA += b;
      ckB += ckA;
      payIdx = 0;
      st = msgLen ? PAYLOAD : CK_A;
      break;
    case PAYLOAD:
      if (payIdx < sizeof(payload)) payload[payIdx] = b;
      ckA += b;
      ckB += ckA;
      if (++payIdx >= msgLen) st = CK_A;
      break;
    case CK_A:
      rxCkA = b;
      st = CK_B;
      break;
    case CK_B:
      st = WAIT_S1;
      if (rxCkA == ckA && b == ckB) {
        if (msgLen <= sizeof(payload)) {
          ubxGood++;
          onUbxMessage(msgCls, msgId, payload, msgLen);
        } else {
          ubxOverflow++;  // valid frame, too big for the buffer
        }
      } else {
        ubxBad++;  // checksum failed: bytes were dropped
      }
      break;
  }
}

// ---- transmit a UBX frame over SPI; reply bytes clocked back are parsed too ----
void sendUBX(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len) {
  uint8_t ckA = 0, ckB = 0;
  auto ck = [&](uint8_t b) {
    ckA += b;
    ckB += ckA;
  };
  uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  ck(cls);
  ck(id);
  ck(len & 0xFF);
  ck(len >> 8);
  for (uint16_t i = 0; i < len; i++) ck(p[i]);

  SPI.beginTransaction(gnssSpi);
  csLow();
  for (int i = 0; i < 6; i++) ubxFeed(SPI.transfer(hdr[i]));
  for (uint16_t i = 0; i < len; i++) ubxFeed(SPI.transfer(p[i]));
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
    if (b == 0xFF && ubxIdle()) {
      if (++idle >= IDLE_RUN) break;
    } else idle = 0;
  }
  csHigh();
  SPI.endTransaction();
}

// Send a CFG message and block until its ACK/NAK comes back.
bool sendWaitAck(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len, uint32_t timeoutMs) {
  ackSeen = false;
  sendUBX(cls, id, p, len);  // reply bytes are parsed inside sendUBX
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (ackSeen && ackCls == cls && ackId == id) return ackOk;
    drainGNSS();
    delay(20);
  }
  return false;  // timed out
}

// ================= CFG-VALSET helpers =================
//
// Sends ONE key per message. Slower than batching, but a NAK then names the
// exact key that the firmware rejected instead of failing the whole group and
// leaving you guessing. That distinction matters a lot here: the previous
// timepulse function batched seven keys, all with wrong IDs, and a single NAK
// would have told you nothing about which.
static bool valsetRaw(uint32_t key, const uint8_t* val, uint8_t valLen, const char* label) {
  uint8_t p[4 + 4 + 8];
  p[0] = 0x00;        // version
  p[1] = CFG_LAYERS;  // layers
  p[2] = 0x00;
  p[3] = 0x00;  // reserved
  p[4] = key & 0xFF;
  p[5] = (key >> 8) & 0xFF;
  p[6] = (key >> 16) & 0xFF;
  p[7] = (key >> 24) & 0xFF;
  memcpy(p + 8, val, valLen);

  bool ok = sendWaitAck(0x06, 0x8A, p, 8 + valLen, 5000);
  // A real NAK is a class-0x05 frame whose payload names 0x06/0x8A. Anything
  // else that leaves ok false is silence, which is a different problem.
  bool realNak = ackSeen && ackCls == 0x06 && ackId == 0x8A && !ackOk;
  Serial.printf("       %-26s 0x%08X  %s\r\n", label, key,
                ok        ? "ACK"
                : realNak ? "NAK: firmware rejected the write  <<<"
                          : "TIMEOUT: no response  <<<");
  return ok;
}

static bool valsetU1(uint32_t key, uint8_t v, const char* label) {
  return valsetRaw(key, &v, 1, label);
}
static bool valsetU4(uint32_t key, uint32_t v, const char* label) {
  uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                   (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
  return valsetRaw(key, b, 4, label);
}
// R8 keys take an IEEE-754 double, little-endian. Sending an integer here is a
// documented way to silently break the timepulse: the receiver accepts the write
// and then produces nothing, because the bit pattern of integer 50 is a
// denormal near zero as a double.
static bool valsetR8(uint32_t key, double v, const char* label) {
  uint8_t b[8];
  memcpy(b, &v, 8);
  return valsetRaw(key, b, 8, label);
}

// Poll a list of keys one at a time, from the RAM layer.
static void valgetOne(uint32_t key) {
  uint8_t p[8] = { 0x00, 0x00, 0x00, 0x00,  // version 0, layer 0 = RAM
                   (uint8_t)(key & 0xFF), (uint8_t)((key >> 8) & 0xFF),
                   (uint8_t)((key >> 16) & 0xFF), (uint8_t)((key >> 24) & 0xFF) };
  // The response arrives as CFG-VALGET and is printed by printCfgValget(). A NAK
  // means this firmware does not know the key, i.e. the ID above is wrong.
  if (!sendWaitAck(0x06, 0x8B, p, sizeof(p), 3000))
    Serial.printf("       0x%08X %-24s  NAK: key unknown to this firmware "
                  "(bad ID, not a fault)\r\n",
                  key, cfgKeyName(key));
  drainGNSS();
}

void ubxDumpCriticalKeys() {
  Serial.println("\r\n[cfg] ---- CFG readback (RAM layer, one key per poll) ----");
  Serial.println("      Looking for any *_L1CA_ENA or *_ENA reading 0, a nonzero "
                 "PM-OPERATEMODE, or an INFIL_MINCNO above ~6.");
  for (size_t i = 0; i < sizeof(kCfgNames) / sizeof(kCfgNames[0]); i++) {
    valgetOne(kCfgNames[i].key);
    delay(10);
  }
  Serial.println("[cfg] ---- end CFG readback ----\r\n");
}

// ================= TP TEST =================
//
// Corrected key IDs. The previous version of this function had all seven wrong,
// and the wrong ones were not harmless:
//
//   was 0x10050001 "TP1_ENA"          -> no such L key in the TP group
//   was 0x10050004 "PULSE_DEF"        -> real PULSE_DEF is 0x20050023 (E1)
//   was 0x10050007 "USE_LOCKED" = 0   -> 0x10050007 IS TP1_ENA. Set to 0 that
//                                        DISABLES the timepulse outright.
//   was 0x10050009 "LOCK_TO_GNSS" = 0 -> 0x10050009 is USE_LOCKED_TP1
//   was 0x1005000A "PULSE_LENGTH_DEF" -> 0x1005000A is ALIGN_TO_TOW_TP1
//   was 0x2005000D "DUTY" = 50        -> DUTY is 0x5005002A and is R8, not U1
//   was 0x40050006 "FREQ" = 4000000   -> 0x40050006 is USER_DELAY_TP1 (I4), so
//                                        that wrote a 4 ms pulse delay
//
// Net effect if the receiver had accepted them: timepulse off, 4 ms of delay,
// and alignment-to-TOW turned on. In practice VALSET NAKs the whole message when
// any key is unknown, so it likely just failed silently. Either way the pin
// would have been dead and the TCXO hypothesis would have looked disproved when
// it had never actually been tested.
//
// SYNC_GNSS_TP1 = 0 is the whole point of this function: local oscillator, not
// GNSS-derived frequency.
bool ubxConfigureTimepulse() {
  Serial.printf("\r\n[cfg] TP TEST: routing local oscillator to TIMEPULSE at %lu Hz\r\n",
                (unsigned long)TP_TEST_FREQ_HZ);
  bool ok = true;
  ok &= valsetU1(0x10050007, 1, "TP-TP1_ENA");                 // L
  ok &= valsetU1(0x10050008, 0, "TP-SYNC_GNSS_TP1");           // L, 0 = local osc
  ok &= valsetU1(0x10050009, 0, "TP-USE_LOCKED_TP1");          // L, unlocked set always
  ok &= valsetU1(0x1005000A, 0, "TP-ALIGN_TO_TOW_TP1");        // L, meaningless unlocked
  ok &= valsetU1(0x20050023, 1, "TP-PULSE_DEF=FREQ");          // E1
  ok &= valsetU1(0x20050030, 0, "TP-PULSE_LENGTH_DEF=RATIO");  // E1
  ok &= valsetU4(0x40050024, TP_TEST_FREQ_HZ, "TP-FREQ_TP1");  // U4
  ok &= valsetR8(0x5005002A, TP_TEST_DUTY, "TP-DUTY_TP1");     // R8 double
  // FREQ_LOCK/DUTY_LOCK are irrelevant with USE_LOCKED_TP1=0, but a bad value in
  // the LOCK pair has been reported to kill the output, so set them to match.
  ok &= valsetU4(0x40050025, TP_TEST_FREQ_HZ, "TP-FREQ_LOCK_TP1");  // U4
  ok &= valsetR8(0x5005002B, TP_TEST_DUTY, "TP-DUTY_LOCK_TP1");     // R8 double

  Serial.println("      Probe TIMEPULSE at JP1/JP2. Measure on a bench frequency");
  Serial.println("      counter, NOT the scope: a scope timebase is +/-25 to 50 ppm,");
  Serial.printf("      the same order as the fault. Expect %lu Hz within ~2 ppm\r\n",
                (unsigned long)TP_TEST_FREQ_HZ);
  Serial.printf("      (+/- %.1f Hz). Above ~10 ppm (%.0f Hz) explains zero satellites.\r\n",
                TP_TEST_FREQ_HZ * 2e-6, TP_TEST_FREQ_HZ * 10e-6);
  return ok;
}

// UBX-CFG-VALSET: enable GPS and disable GLONASS, BeiDou, and Galileo.
// Four constellations split the correlators four ways, which is the worst case
// for an unaided cold start. Also narrows MON-SPAN, improving its resolution
// around 1575.42 MHz.
bool ubxSetGpsOnly() {
  Serial.println("\r\n[cfg] GPS only (disabling GLO/BDS/GAL):");
  bool ok = true;
  ok &= valsetU1(0x1031001F, 1, "SIGNAL-GPS_ENA");
  ok &= valsetU1(0x10310001, 1, "SIGNAL-GPS_L1CA_ENA");  // explicitly, see notes
  ok &= valsetU1(0x10310025, 0, "SIGNAL-GLO_ENA");
  ok &= valsetU1(0x10310022, 0, "SIGNAL-BDS_ENA");
  ok &= valsetU1(0x10310021, 0, "SIGNAL-GAL_ENA");
  return ok;
}

// UBX-CFG-VALSET: switch the interference monitor on so jammingState stops
// reporting "unknown/disabled" and jamInd acquires a reference. ANTSETTING is not
// cosmetic: the detection thresholds differ between a passive and an active
// antenna, so a wrong value gives a miscalibrated monitor rather than none.
//
// Caveat that has already cost time here: the ITFM reference spectrum is only
// captured after a first good fix. On a receiver that has never fixed, this
// changes nothing and jammingState stays "unknown". Enabling it is for later.
bool ubxEnableItfm(uint8_t antType) {
  Serial.println("\r\n[cfg] interference monitor (ITFM):");
  bool ok = true;
  ok &= valsetU1(0x1041000D, 1, "ITFM-ENABLE");
  ok &= valsetU1(0x20410010, antType, "ITFM-ANTSETTING");
  ok &= valsetU1(0x2091000A, NAV_PVT_RATE, "MSGOUT-UBX_NAV_PVT_SPI");
  ok &= valsetU1(0x20910016, 1, "MSGOUT-UBX_NAV_SAT_SPI");
  return ok;
}

// UBX-CFG-CFG (0x06 0x09). On PROTVER 32 the three masks lost their granularity:
// any bit set in clearMask deletes ALL saved config in the selected NVM layers.
// saveMask/loadMask stay 0 so the live RAM layer keeps the link up; CFG-RST below
// rebuilds RAM from the now-empty lower layers. deviceMask 0x03 = BBR | Flash.
// This clears configuration only. It does not touch the firmware image.
bool ubxClearAllConfig() {
  const uint8_t p[13] = {
    0xFF, 0xFF, 0xFF, 0xFF,  // clearMask  = clear everything
    0x00, 0x00, 0x00, 0x00,  // saveMask   = save nothing
    0x00, 0x00, 0x00, 0x00,  // loadMask   = don't reload yet
    0x03                     // deviceMask = devBBR | devFlash
  };
  return sendWaitAck(0x06, 0x09, p, sizeof(p), 8000);  // flash erase is slow
}

// UBX-CFG-VALSET, layers RAM|BBR|Flash (0x07) DELIBERATELY, overriding
// CFG_LAYERS: if SPI does not survive the next power cycle this board has no
// UART or I2C fallback and becomes unreachable.
bool ubxRearmSpi() {
  uint8_t p[] = {
    0x00, 0x07, 0x00, 0x00,        // version 0, layers RAM|BBR|Flash
    0x06, 0x00, 0x64, 0x10, 0x01,  // CFG-SPI-ENABLED            = 1
    0x0A, 0x00, 0x91, 0x20, 0x01   // CFG-MSGOUT-UBX_NAV_PVT_SPI = 1
  };
  uint16_t len = RESET_ENABLE_NAV_PVT ? sizeof(p) : sizeof(p) - 5;
  return sendWaitAck(0x06, 0x8A, p, len, 5000);
}

// UBX-CFG-RST (0x06 0x04): cold start, controlled software reset.
// Never acknowledged, per the interface description.
//
// Note that on the M9N, RESET_N and this both clear BBR, so every reset restarts
// the cold start from scratch. Any injected aiding is also wiped, which is why
// injectAiding() runs after all resets, never before.
void ubxColdReset() {
  const uint8_t p[4] = { 0xFF, 0xFF, 0x01, 0x00 };  // navBbrMask=0xFFFF, resetMode=0x01
  sendUBX(0x06, 0x04, p, sizeof(p));
}

void factoryResetOnce() {
  Preferences nvs;
  nvs.begin("gnss", false);
  if (nvs.getString("fr_token", "") == RESET_TOKEN) {
    Serial.printf("[reset] token \"%s\" already used, skipping\r\n", RESET_TOKEN);
    nvs.end();
    return;
  }

  Serial.println("\r\n[reset] ---- FACTORY RESET (one shot) ----");

  Serial.print("[reset] clearing BBR + Flash config layers ... ");
  Serial.println(ubxClearAllConfig() ? "ACK" : "no ACK / NAK");

#if RESET_KEEP_SPI
  Serial.print("[reset] re-arming SPI keys to flash (defaults leave SPI disabled) ... ");
  Serial.println(ubxRearmSpi() ? "ACK" : "no ACK / NAK");
#else
  Serial.println("[reset] RESET_KEEP_SPI=0: SPI may come back disabled");
#endif

  nvs.putString("fr_token", RESET_TOKEN);  // latch before the reset, not after
  nvs.end();

  Serial.println("[reset] cold start + software reset ...");
  ubxColdReset();
  delay(2500);  // let the receiver reboot

  gotData = false;
  sendUBX(0x0A, 0x04, nullptr, 0);  // MON-VER poll proves the link survived
  Serial.println("[reset] ---- done, watching for UBX ----\r\n");
}

// ================= AIDING: NTP then MGA-INI =================
#if ENABLE_AIDING

// Bring WiFi up only long enough to set the system clock, then take it fully
// down. Returns true if time(nullptr) is now credible. Everything after this
// runs with the WiFi PHY unpowered.
bool ntpSync() {
  Serial.printf("\r\n[aid] WiFi \"%s\" up for NTP only ... ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("failed to associate");
    WiFi.disconnect(true, true);
    return false;
  }
  Serial.printf("connected (%s, %d dBm)\r\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.cloudflare.com");

  struct tm tmv;
  bool ok = false;
  t0 = millis();
  while (millis() - t0 < NTP_TIMEOUT_MS) {
    if (getLocalTime(&tmv, 500) && tmv.tm_year + 1900 >= 2026) {
      ok = true;
      break;
    }
  }

  // Down hard. WiFi.disconnect(wifioff=true, eraseap=true) also keeps the
  // credentials out of the NVS wifi partition.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  Serial.print("[aid] WiFi down. ");

  if (!ok) {
    Serial.println("NTP did not resolve, skipping time aiding.");
    return false;
  }
  Serial.printf("UTC now %04d-%02d-%02d %02d:%02d:%02d\r\n",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return true;
}

// UBX-MGA-INI-TIME_UTC (class 0x13 id 0x40, type 0x10), 24-byte payload.
// Not acknowledged unless CFG-NAVSPG-ACKAIDING is set, so verification here is
// indirect: the next NAV-PVT should show a plausible year with validTime set.
void mgaIniTimeUtc() {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);

  uint16_t year = t.tm_year + 1900;
  // NTP over WiFi lands well inside 100 ms, but claiming more accuracy than the
  // path really has just makes the receiver trust a bad number. 500 ms is honest
  // and still collapses the search enormously.
  const uint32_t tAccNs = 500000000UL;

  uint8_t p[24] = { 0 };
  p[0] = 0x10;  // type: UTC time
  p[1] = 0x00;  // version
  p[2] = 0x00;  // ref: valid on message reception
  p[3] = (uint8_t)(int8_t)AID_LEAP_SECS;
  p[4] = year & 0xFF;
  p[5] = (year >> 8) & 0xFF;
  p[6] = t.tm_mon + 1;
  p[7] = t.tm_mday;
  p[8] = t.tm_hour;
  p[9] = t.tm_min;
  p[10] = t.tm_sec;
  // p[11] reserved, p[12..15] ns = 0
  p[16] = 0x00;
  p[17] = 0x00;  // tAccS = 0
  // p[18..19] reserved
  p[20] = tAccNs & 0xFF;
  p[21] = (tAccNs >> 8) & 0xFF;
  p[22] = (tAccNs >> 16) & 0xFF;
  p[23] = (tAccNs >> 24) & 0xFF;

  sendUBX(0x13, 0x40, p, sizeof(p));
  Serial.printf("[aid] MGA-INI-TIME_UTC sent: %04u-%02u-%02u %02u:%02u:%02u, "
                "leap %d, acc 500 ms\r\n",
                year, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                AID_LEAP_SECS);
}

#if AID_POS
// UBX-MGA-INI-POS_LLH (class 0x13 id 0x40, type 0x01), 20-byte payload.
void mgaIniPosLlh() {
  int32_t lat = (int32_t)(AID_LAT_DEG * 1e7);
  int32_t lon = (int32_t)(AID_LON_DEG * 1e7);
  int32_t alt = (int32_t)(AID_ALT_M * 100);        // cm
  uint32_t acc = (uint32_t)(AID_POS_ACC_M * 100);  // cm

  uint8_t p[20] = { 0 };
  p[0] = 0x01;  // type: position LLH
  p[1] = 0x00;  // version
  // p[2..3] reserved
  memcpy(p + 4, &lat, 4);
  memcpy(p + 8, &lon, 4);
  memcpy(p + 12, &alt, 4);
  memcpy(p + 16, &acc, 4);

  sendUBX(0x13, 0x40, p, sizeof(p));
  Serial.printf("[aid] MGA-INI-POS_LLH sent: %.4f, %.4f, %d m, acc %d m\r\n",
                (double)AID_LAT_DEG, (double)AID_LON_DEG,
                (int)AID_ALT_M, (int)AID_POS_ACC_M);
}
#endif

void injectAiding() {
  if (!ntpSync()) return;
  mgaIniTimeUtc();
#if AID_POS
  if (AID_LAT_DEG != 0.0 || AID_LON_DEG != 0.0) mgaIniPosLlh();
  else Serial.println("[aid] AID_POS set but coordinates are 0,0. Skipping position.");
#else
  Serial.println("[aid] position aiding off (AID_POS=0). Time only.");
#endif
  drainGNSS();
}
#endif  // ENABLE_AIDING

// ================= RF QUIET: ESP32-C6 WiFi and BLE =================
//
// With ENABLE_AIDING this runs AFTER ntpSync() has finished with the radio, so
// "was WiFi ever initialised" will legitimately answer yes. That is expected.
// The invariant that matters is that no GNSS measurement is taken while the WiFi
// PHY is powered, and the ordering in setup() enforces that.
//
// esp_bt_controller_mem_release() returns the controller's RAM to the heap and
// BLE cannot be started again without a reset.
void espRadiosOff() {
  WiFi.mode(WIFI_OFF);
  esp_err_t e = esp_wifi_stop();
  if (e == ESP_ERR_WIFI_NOT_INIT) {
    Serial.println("[rf] WiFi: driver never initialised, PHY was never powered");
  } else {
    esp_wifi_deinit();
    Serial.printf("[rf] WiFi: stopped and deinitialised (%s)\r\n", esp_err_to_name(e));
  }

#if defined(CONFIG_BT_ENABLED)
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
  }
  // C6 is BLE only, there is no classic controller region to release.
  esp_err_t b = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  Serial.printf("[rf] BLE:  controller idle, RAM released (%s)\r\n", esp_err_to_name(b));
#else
  Serial.println("[rf] BLE:  not compiled into this build");
#endif
}

// ================= RF QUIET: E22P-915M30S / SX1262 =================
//
// E22P manual section 4.2, 915M30S variant: LNA_EN is tied internally to the RF
// switch EN pin and PA_EN is tied to T/R CTRL. Truth table EN=0, T/R=X -> CLOSE.
// So EN (GPIO17) low kills the switch and the LNA in one move, and is the first
// thing done, before the SPI bus even exists.
//
// Notes specific to this board:
//   - NRST is driven, not left floating. R15 ties that net to +5V, so as an input
//     the C6 pad sits above its 3.6 V absolute max and leaks through the ESD
//     clamp. Driven low it just sinks ~0.5 mA through R15. R15 moving to +3V3 is
//     already on the next-revision list.
//   - GPIO17 has a boot-time weak internal pull-up (U0RXD default) fighting R17's
//     10k, so EN idles at a few hundred mV rather than 0 V until firmware drives
//     it. Below VIL, but not a clean zero, which is why this runs early.
//   - With the SX1262 in reset, DIO2 goes high-Z and the only thing defining the
//     T/R CTRL / PA_EN node is the TPS610333's internal 1 MOhm MODE pull-down
//     through R21. Works, but an explicit 10k to GND on that net is cheap
//     insurance for the next revision.
//   - The +5V rail feeding E22P VCC cannot be switched off: U5's EN pin is
//     strapped to its own VIN. "Off" here means the RF blocks and the SX1262.
#if 1
static SPISettings loraSpi(1000000, MSBFIRST, SPI_MODE0);  // SX1262: CPOL=0, CPHA=0
#define LORA_NSS_PIN 21
#define LORA_NRST_PIN 16
#define LORA_EN_PIN 17
#define LORA_BUSY_PIN 7
#define LORA_DIO1_PIN 1
#define LORA_HOLD_RESET 1

static bool loraWaitBusyLow(uint32_t ms) {
  uint32_t t0 = millis();
  while (digitalRead(LORA_BUSY_PIN)) {
    if (millis() - t0 > ms) return false;
    delay(1);
  }
  return true;
}

static void loraCmd(uint8_t op, const uint8_t* p, uint8_t n) {
  SPI.beginTransaction(loraSpi);
  digitalWrite(LORA_NSS_PIN, LOW);
  SPI.transfer(op);
  for (uint8_t i = 0; i < n; i++) SPI.transfer(p[i]);
  digitalWrite(LORA_NSS_PIN, HIGH);
  SPI.endTransaction();
}

// GetStatus (0xC0): status arrives on the byte after the opcode.
static uint8_t loraGetStatus() {
  SPI.beginTransaction(loraSpi);
  digitalWrite(LORA_NSS_PIN, LOW);
  SPI.transfer(0xC0);
  uint8_t s = SPI.transfer(0x00);
  digitalWrite(LORA_NSS_PIN, HIGH);
  SPI.endTransaction();
  return s;
}

// Safe pin state. Call before SPI.begin(); needs no bus.
void loraPinsSafe() {
  pinMode(LORA_EN_PIN, OUTPUT);
  digitalWrite(LORA_EN_PIN, LOW);  // CLOSE + LNA off
  pinMode(LORA_NSS_PIN, OUTPUT);
  digitalWrite(LORA_NSS_PIN, HIGH);  // release shared MISO
  pinMode(LORA_NRST_PIN, OUTPUT);
  digitalWrite(LORA_NRST_PIN, HIGH);
  pinMode(LORA_BUSY_PIN, INPUT);
  pinMode(LORA_DIO1_PIN, INPUT);
}

// Verify the SX1262 is actually there, then sleep it. Call after SPI.begin().
void loraSleep() {
  digitalWrite(LORA_NRST_PIN, LOW);  // defined start rather than assuming POR state
  delay(2);
  digitalWrite(LORA_NRST_PIN, HIGH);
  if (!loraWaitBusyLow(50)) {
    Serial.println("[rf] LoRa: BUSY never fell after reset. Holding NRST low, EN low.");
    digitalWrite(LORA_NRST_PIN, LOW);
    return;
  }

  uint8_t s = loraGetStatus();
  static const char* modeName[] = { "?", "RFU", "STBY_RC", "STBY_XOSC", "FS", "RX", "TX", "?" };
  Serial.printf("[rf] LoRa: status 0x%02X (chipMode %s)", s, modeName[(s >> 4) & 0x07]);
  if (s == 0x00 || s == 0xFF)
    Serial.print("  <<< all-zeros/all-ones, the radio is not answering on MISO");

  const uint8_t stdby[1] = { 0x00 };
  loraCmd(0x80, stdby, 1);  // SetStandby, STDBY_RC
  loraWaitBusyLow(20);

  const uint8_t clr[2] = { 0xFF, 0xFF };
  loraCmd(0x02, clr, 2);  // ClearIrqStatus
  loraWaitBusyLow(20);

  const uint8_t slp[1] = { 0x00 };
  loraCmd(0x84, slp, 1);  // SetSleep, cold start, no RTC wake
  delay(2);               // BUSY goes HIGH in sleep and stays there

#if LORA_HOLD_RESET
  digitalWrite(LORA_NRST_PIN, LOW);
  Serial.println(" -> sleep (cold start), NRST then held low. EN low, DIO3/TCXO unpowered.");
#else
  Serial.println(" -> sleep (cold start), no RTC wake. EN low, DIO3/TCXO unpowered.");
#endif
}
#endif

// -----------------------------------------------------------------------------

unsigned long lastDrain = 0, startMs = 0, lastDiag = 0;
bool warned = false;

// Wait for the module to answer a UBX poll. Returns true if it did.
static bool waitForUbx(uint16_t tries) {
  for (uint16_t i = 0; i < tries && !gotData; i++) {
    drainGNSS();
    delay(50);
  }
  return gotData;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\r\n=== NEO-M9N SPI reader (limeskey-node) ===");
  Serial.printf("Config writes go to %s\r\n",
                CFG_PERSIST ? "RAM|BBR|Flash (persistent)" : "RAM only (session)");

  // 1. LoRa pins to a safe state first: EN low kills the RF switch and LNA
  //    before anything else on the board wakes up.
  loraPinsSafe();
  if (PPS_PIN >= 0) pinMode(PPS_PIN, INPUT);

    // 2. WiFi window, if aiding is enabled. This is the ONLY time the 2.4 GHz PHY
    //    is powered, and it closes before any GNSS measurement is taken.
#if ENABLE_AIDING
  bool haveTime = ntpSync();
#endif

  // 3. Radios definitively down.
  espRadiosOff();

  // 4. SPI bus and the SX1262 to sleep.
  pinMode(CS_PIN, OUTPUT);
  csHigh();
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  Serial.printf("SPI up: SCK=%d MISO=%d MOSI=%d CS=%d @ %d Hz, MODE0\r\n",
                SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN, SPI_HZ);
  loraSleep();

  delay(200);
  sendUBX(0x0A, 0x04, nullptr, 0);  // request firmware version once

  // 5. Optional wipe. Must come before aiding: a cold start clears BBR and would
  //    discard anything injected.
#if DO_FACTORY_RESET
  if (waitForUbx(12)) factoryResetOnce();
  else Serial.println("[reset] no UBX reply in 600 ms, skipping factory reset "
                      "(module may still be in ROM BOOT)");
#endif

  if (!waitForUbx(12)) {
    Serial.println("\r\n[!] module is not answering UBX. Everything below is skipped.");
  } else {
    // 6. Constellation config.
#if GPS_ONLY
    ubxSetGpsOnly();
#endif

    // 7. TP TEST. This is the primary experiment in this build.
#if TP_TEST
    ubxConfigureTimepulse();
#endif

#if ENABLE_ITFM
    ubxEnableItfm(ITFM_ANT_TYPE);
#endif

    // 8. Read back what actually took. This is the cheap answer to "is it a
    //    config fault or is it silicon", and it runs after the writes so the
    //    values printed are the live ones.
    ubxDumpCriticalKeys();

    // 9. Aiding last, after every reset and config write.
#if ENABLE_AIDING
    if (haveTime) injectAiding();
    else Serial.println("[aid] no credible system time, skipping MGA injection.");
#endif
  }

  startMs = millis();
  lastDiag = millis() - DIAG_MS + 2000;  // first diagnostic poll ~2 s after boot
}

void loop() {
  if (millis() - lastDrain >= DRAIN_MS) {
    lastDrain = millis();
    drainGNSS();
  }

  if (millis() - lastDiag >= DIAG_MS) {
    lastDiag = millis();
    sendUBX(0x0A, 0x38, nullptr, 0);  // poll MON-RF
    sendUBX(0x0A, 0x31, nullptr, 0);  // poll MON-SPAN
    sendUBX(0x0A, 0x28, nullptr, 0);  // poll MON-GNSS
    sendUBX(0x01, 0x35, nullptr, 0);  // poll NAV-SAT

    // Parser health. A nonzero ubxBad that climbs alongside ubxGood means the
    // SPI read is dropping bytes mid-frame, and no count reported above can be
    // trusted until that is fixed.
    Serial.printf("\r\n[parse] frames ok %lu   checksum fail %lu   oversize %lu   "
                  "uptime %lu s\r\n",
                  (unsigned long)ubxGood, (unsigned long)ubxBad,
                  (unsigned long)ubxOverflow, (millis() - startMs) / 1000);
    if (ubxBad > 0 && ubxBad * 20 > ubxGood)
      Serial.println("[parse] >> over 5% of frames failing checksum. Fix the SPI read "
                     "before trusting any diagnostic in this log.");
  }

  if (!gotData && !warned && millis() - startMs > 6000) {
    warned = true;
    Serial.println("\r\n[!] 6 s and no UBX frame decoded: bus is returning only 0xFF.");
    Serial.println("    Confirm D_SEL = GND (SPI selected) and power-cycle the module,");
    Serial.println("    and that the SX1262 NSS is held high so it isn't driving MISO.");
  }
}
