// Xiaomi Mi Computer Monitor Light Bar (MJGJD01YL) controller for nice!nano v2 (nRF52840).
// Uses the nRF52840 radio in nRF24-compatible mode (nrf_to_nrf library), no external module.
// Protocol: https://github.com/lamperez/xiaomi-lightbar-nrf24
//
// Serial commands (115200, newline terminated):
//   on | up [n] | down [n] | cool [n] | warm [n] | reset | bri <0-15> | ct <0-15>
//   id <hex>   set remote id (e.g. id 1A2B3C), saved to flash
//   sniff      listen for the original remote and learn its id (any key stops)
//   pair       resend reset 3 times (use within 20 s after replugging the bar)
//   info
//   reps <n>   repetitions per channel (default 20)

#include <nrf_to_nrf.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

static const uint8_t PREAMBLE[8] = {0x53, 0x39, 0x14, 0xDD, 0x1C, 0x49, 0x34, 0x12};
static const uint8_t CHANNELS[4] = {6, 15, 43, 68};
static const uint64_t TX_ADDR = 0x5555555555ULL;
static const uint64_t RX_ADDR = 0xAAAAAAAAAAULL;
static const char *CFG_FILE = "/lightbar.cfg";
static const uint32_t LED_PIN = 15;  // nice!nano blue LED on P0.15

nrf_to_nrf radio;
uint32_t remoteId = 0x4C1D5A;  // arbitrary default; replace with sniffed id or pair
uint8_t counter = 0;
uint8_t rxChannel = 68;
uint8_t reps = 20;       // repetitions per channel (burst ~ reps * 4 * 2.5 ms)

static void led(bool on) {
  if (on) NRF_P0->OUTSET = 1UL << LED_PIN;
  else NRF_P0->OUTCLR = 1UL << LED_PIN;
}

static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFE;  // poly 0x1021, init 0xFFFE, no reflection, no xorout
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

static void saveId() {
  InternalFS.remove(CFG_FILE);
  File f(InternalFS);
  if (f.open(CFG_FILE, FILE_O_WRITE)) {
    f.write((uint8_t *)&remoteId, sizeof(remoteId));
    f.close();
  }
}

static void loadId() {
  File f(InternalFS);
  if (f.open(CFG_FILE, FILE_O_READ)) {
    uint32_t v;
    if (f.read((uint8_t *)&v, sizeof(v)) == sizeof(v) && v <= 0xFFFFFF) remoteId = v;
    f.close();
  }
}

static void radioTxMode() {
  radio.stopListening();
  radio.setPayloadSize(17);
  radio.openWritingPipe(TX_ADDR);
}

static void radioRxMode() {
  radio.setPayloadSize(18);
  radio.openReadingPipe(1, RX_ADDR);
  radio.setChannel(rxChannel);
  radio.startListening();
}

static void send(uint8_t cmd, uint8_t opt) {
  uint8_t p[17];
  memcpy(p, PREAMBLE, 8);
  p[8] = remoteId >> 16;
  p[9] = remoteId >> 8;
  p[10] = remoteId;
  p[11] = 0xFF;
  p[12] = counter++;
  p[13] = cmd;
  p[14] = opt;
  uint16_t c = crc16(p, 15);
  p[15] = c >> 8;
  p[16] = c & 0xFF;

  led(true);
  radioTxMode();
  for (int rep = 0; rep < reps; rep++) {
    for (uint8_t ch : CHANNELS) {
      radio.setChannel(ch);
      radio.write(p, sizeof(p), true);
      delay(2);
    }
  }
  led(false);
  Serial.printf("sent cmd=%02X opt=%02X cnt=%u id=%06lX\n", cmd, opt, p[12], remoteId);
}

static uint8_t clamp15(long v) { return v < 0 ? 0 : (v > 15 ? 15 : v); }

static void sniff() {
  Serial.printf("sniffing on ch %u... use the remote near the board (send any char to stop)\n", rxChannel);
  radioRxMode();
  uint32_t lastHop = millis();
  uint8_t chIdx = 3;
  while (!Serial.available()) {
    if (radio.available()) {
      uint8_t raw[18] = {0};
      radio.read(raw, sizeof(raw));
      // Re-align: received stream is shifted relative to the packet (see lightbar2mqtt)
      uint8_t d[17];
      for (int i = 0; i < 17; i++) {
        if (i == 0) d[i] = 0x50 | raw[i] >> 5;
        else d[i] = ((raw[i - 1] >> 1) & 0x0F) << 4 | ((raw[i - 1] & 0x01) << 3) | raw[i] >> 5;
      }
      if (memcmp(d, PREAMBLE, 8) != 0) continue;
      uint16_t c = crc16(d, 15);
      bool ok = c == ((d[15] << 8) | d[16]);
      uint32_t id = (uint32_t)d[8] << 16 | d[9] << 8 | d[10];
      Serial.printf("pkt id=%06lX cnt=%u cmd=%02X%02X crc=%s\n", id, d[12], d[13], d[14], ok ? "OK" : "bad");
      if (ok) {
        remoteId = id;
        counter = d[12] + 1;
        saveId();
        Serial.printf("learned remote id %06lX (saved)\n", id);
        break;
      }
    }
    if (millis() - lastHop > 3000) {  // hop channel every 3 s to find the active one
      chIdx = (chIdx + 1) % 4;
      rxChannel = CHANNELS[chIdx];
      radio.stopListening();
      radioRxMode();
      Serial.printf("ch %u\n", rxChannel);
      lastHop = millis();
    }
  }
  while (Serial.available()) Serial.read();
  radio.stopListening();
}

static void handle(String line) {
  line.trim();
  line.toLowerCase();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long n = arg.length() ? arg.toInt() : 1;

  if (cmd == "on" || cmd == "off" || cmd == "toggle") send(0x01, 0x00);
  else if (cmd == "cool") send(0x02, clamp15(n));
  // Option byte is a signed step: 0x0300 - n == cmd 0x02 / opt -n (as in lamperez/xiaomi-lightbar-nrf24)
  else if (cmd == "warm") send(0x02, (uint8_t)(0x100 - clamp15(n)));
  else if (cmd == "up") send(0x04, clamp15(n));
  else if (cmd == "down") send(0x04, (uint8_t)(0x100 - clamp15(n)));
  else if (cmd == "reset") send(0x06, 0x00);
  else if (cmd == "bri") { send(0x04, 0xF0); delay(50); send(0x04, clamp15(n)); }
  else if (cmd == "ct") { send(0x02, 0xF0); delay(50); send(0x02, clamp15(n)); }
  else if (cmd == "pair") { for (int i = 0; i < 3; i++) { send(0x06, 0x00); delay(200); } }
  else if (cmd == "sniff") sniff();
  else if (cmd == "id" && arg.length()) {
    remoteId = strtoul(arg.c_str(), nullptr, 16) & 0xFFFFFF;
    saveId();
    Serial.printf("id set to %06lX (saved)\n", remoteId);
  }
  else if (cmd == "reps" && arg.length()) { reps = constrain(arg.toInt(), 1, 100); Serial.printf("reps=%u\n", reps); }
  else if (cmd == "info") Serial.printf("id=%06lX counter=%u reps=%u\n", remoteId, counter, reps);
  else if (cmd.length()) Serial.println("cmds: on up down cool warm reset bri ct id sniff pair info");
}

void setup() {
  NRF_P0->DIRSET = 1UL << LED_PIN;
  Serial.begin(115200);
  InternalFS.begin();
  loadId();

  radio.begin();
  radio.setDataRate(NRF_2MBPS);
  radio.setAutoAck(false);
  radio.disableDynamicPayloads();
  radio.setCRCLength(NRF_CRC_DISABLED);
  radio.setPALevel(NRF_PA_MAX);
  radio.setAddressWidth(5);
  radioTxMode();

  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  Serial.printf("xiaomi lightbar ctl ready, id=%06lX\n", remoteId);
}

void loop() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { handle(buf); buf = ""; }
    else buf += c;
  }
}
