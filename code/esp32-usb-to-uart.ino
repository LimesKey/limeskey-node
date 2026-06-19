#define NEO_RX 20      // ESP32 RX  <- NEO TXD  (your "MISO" net)
#define NEO_TX 18      // ESP32 TX  -> NEO RXD  (your "MOSI" net)
#define NEO_BAUD 9600

void setup() {
  Serial.begin(115200);  // USB CDC; PC-side baud is ignored over USB
  Serial1.begin(NEO_BAUD, SERIAL_8N1, NEO_RX, NEO_TX);
  pinMode(/*LoRa NSS*/ 21, OUTPUT);
  digitalWrite(/*LoRa NSS*/ 21, HIGH); // keep SX1262 off the shared MISO
}
void loop() {
  while (Serial.available())  Serial1.write(Serial.read());
  while (Serial1.available()) Serial.write(Serial1.read());
}