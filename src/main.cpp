/*
 * Dual SEN0644 continuous logger: baud auto-detect + reset to 9600,
 * acquisition level 1, CSV output over USB serial.
 *
 * Two sensors, each on its own RS485 transceiver / own ESP32 UART, so both
 * can keep the default Modbus address 0x01 without colliding.
 *
 *   Sensor #1 -> UART1, ESP32 RX GPIO 17, ESP32 TX GPIO 18
 *   Sensor #2 -> UART2, ESP32 RX GPIO 43, ESP32 TX GPIO 44
 *
 * On boot, for each sensor independently:
 *   1. Tries each candidate baud until the sensor answers
 *   2. Sets acquisition level 1
 *   3. Resets baud register 0x0065 to 3 (9600) if the sensor was elsewhere,
 *      then asks you to power-cycle that sensor and waits for it at 9600
 * A sensor that never answers is marked offline; the other one still logs.
 *
 * Then streams CSV lines: seconds,lux1,lux2
 * The timestamp is seconds since boot with millisecond resolution
 * (e.g. "12.345"). A failed read leaves that column empty,
 * e.g. "12.345,22.211," .
 *
 * Status/diagnostic lines are prefixed with '#' so the host CSV logger
 * can tell them apart from data.
 *
 * Wiring (per sensor, one RS485 module each):
 *   RS485 module TXD -> ESP32 RX pin (17 for #1, 43 for #2)
 *   RS485 module RXD -> ESP32 TX pin (18 for #1, 44 for #2)
 *   RS485 module VCC/GND -> 3.3V / GND
 *   A <-> sensor yellow, B <-> sensor green
 *   Sensor power DC 5-32 V, supply GND common with ESP32 GND
 *
 * Serial port: built with ARDUINO_USB_CDC_ON_BOOT=1 (see platformio.ini),
 * so `Serial` is the native USB CDC on the "ESP32-S3 Type-C USB & OTG"
 * connector (GPIO 19/20), not UART0 / the CH343P bridge.
 */

#include <Arduino.h>

#define S1_RX 17
#define S1_TX 18
#define S2_RX 43
#define S2_TX 44
#define USB_BAUD 115200          // ignored by native USB CDC, kept for UART0 fallback
#define USB_CONNECT_TIMEOUT_MS 3000
#define POLL_DELAY_MS 30
#define EXPECTED_BAUD 9600       // the baud we force every sensor back to
#define BAUD_WRITE_RETRIES 3     // attempts at the baud-register write per init
#define RECOVER_AFTER_FAILS 20   // consecutive read errors before re-running init
#define OFFLINE_RETRY_MS 10000   // how often to retry a sensor that never answered
#define POWERCYCLE_WAIT_MS 60000 // give up waiting for a power-cycled sensor

struct Sensor {
  HardwareSerial *port;
  const char *name;
  int rxPin;
  int txPin;
  uint32_t baud;                 // detected baud, 0 while unknown
  bool online;
  uint16_t consecFail;           // consecutive failed reads while online
  uint32_t lastAttempt;          // millis() of the last init attempt
};

HardwareSerial RS485_1(1);
HardwareSerial RS485_2(2);

Sensor sensors[] = {
  { &RS485_1, "sensor1", S1_RX, S1_TX, 0, false, 0, 0 },
  { &RS485_2, "sensor2", S2_RX, S2_TX, 0, false, 0, 0 },
};
const uint8_t NUM_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

const uint32_t CANDIDATE_BAUDS[] = {9600, 57600, 4800, 19200, 38400, 115200, 2400};
const uint8_t  NUM_BAUDS = sizeof(CANDIDATE_BAUDS) / sizeof(CANDIDATE_BAUDS[0]);

const uint8_t READ_LUX[]      = {0x01, 0x03, 0x00, 0x02, 0x00, 0x02, 0x65, 0xCB};
const uint8_t SET_LEVEL_1[]   = {0x01, 0x06, 0x00, 0x46, 0x00, 0x01, 0xA9, 0xDF};
const uint8_t SET_BAUD_9600[] = {0x01, 0x06, 0x00, 0x65, 0x00, 0x03, 0xD9, 0xD4};

uint16_t crc16Modbus(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
  }
  return crc;
}

size_t sendAndReceive(Sensor &s, const uint8_t *frame, size_t frameLen,
                      uint8_t *resp, size_t respLen) {
  while (s.port->available()) s.port->read();
  s.port->write(frame, frameLen);
  s.port->flush();

  size_t n = 0;
  uint32_t start = millis();
  while (n < respLen && millis() - start < 300) {
    if (s.port->available()) resp[n++] = s.port->read();
  }
  return n;
}

bool readLuxRaw(Sensor &s, uint32_t &raw) {
  uint8_t resp[9];
  size_t n = sendAndReceive(s, READ_LUX, 8, resp, 9);
  if (n != 9 || resp[0] != 0x01 || resp[1] != 0x03 || resp[2] != 0x04) return false;
  uint16_t crc = crc16Modbus(resp, 7);
  if (resp[7] != (crc & 0xFF) || resp[8] != (crc >> 8)) return false;
  raw = ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16) |
        ((uint32_t)resp[5] << 8) | resp[6];
  return true;
}

bool sensorResponds(Sensor &s) {
  uint32_t dummy;
  return readLuxRaw(s, dummy);
}

uint32_t detectBaud(Sensor &s) {
  for (uint8_t i = 0; i < NUM_BAUDS; i++) {
    s.port->updateBaudRate(CANDIDATE_BAUDS[i]);
    delay(50);
    if (sensorResponds(s)) return CANDIDATE_BAUDS[i];
    if (sensorResponds(s)) return CANDIDATE_BAUDS[i];  // one retry per baud
  }
  return 0;
}

bool writeAndConfirm(Sensor &s, const uint8_t *frame) {
  uint8_t resp[8];
  size_t n = sendAndReceive(s, frame, 8, resp, 8);
  return n == 8 && memcmp(resp, frame, 8) == 0;
}

void status(Sensor &s, const char *msg) {
  Serial.print("# [");
  Serial.print(s.name);
  Serial.print("] ");
  Serial.println(msg);
}

// Brings one sensor up: find it, set level 1, force the baud register back to
// EXPECTED_BAUD. Sets s.online. Safe to call again at runtime to recover a
// sensor that dropped off the bus.
void initSensor(Sensor &s) {
  s.lastAttempt = millis();
  s.consecFail = 0;

  s.port->end();                 // no-op the first time, resets the UART on retries
  delay(50);
  s.port->begin(EXPECTED_BAUD, SERIAL_8N1, s.rxPin, s.txPin);
  delay(200);

  Serial.print("# [");
  Serial.print(s.name);
  Serial.print("] searching... (ESP32 RX GPIO ");
  Serial.print(s.rxPin);
  Serial.print(", TX GPIO ");
  Serial.print(s.txPin);
  Serial.println(")");
  uint32_t found = detectBaud(s);
  if (found == 0) {
    status(s, "ERROR: not found at any baud (check A/B, common GND, pins)");
    status(s, "marked offline, its CSV column stays empty");
    s.online = false;
    return;
  }
  s.baud = found;
  s.online = true;

  Serial.print("# [");
  Serial.print(s.name);
  Serial.print("] found at ");
  Serial.print(found);
  Serial.println(" baud");

  if (writeAndConfirm(s, SET_LEVEL_1)) {
    status(s, "acquisition level 1 confirmed");
  } else {
    status(s, "WARNING: level write not confirmed, continuing");
  }

  // Rewrite the baud register on EVERY init, even when the sensor already
  // answers at EXPECTED_BAUD. A sensor swapped in from another rig, or one
  // whose register got clobbered by a half-finished write, is then always
  // driven to a known state instead of silently running on whatever it held.
  bool baudWritten = false;
  for (uint8_t attempt = 1; attempt <= BAUD_WRITE_RETRIES && !baudWritten; attempt++) {
    baudWritten = writeAndConfirm(s, SET_BAUD_9600);
    if (!baudWritten) delay(100);
  }

  if (!baudWritten) {
    status(s, "WARNING: baud write not confirmed, staying at detected baud");
    s.port->updateBaudRate(found);
    return;
  }

  if (found == EXPECTED_BAUD) {
    // Already there: the write was a no-op, so no power-cycle is needed.
    status(s, "baud register rewritten to 9600 (already there, no reset needed)");
    return;
  }

  status(s, "baud register set to 9600");
  status(s, ">>> POWER-CYCLE THIS SENSOR NOW (unplug its supply ~3 s) <<<");
  s.port->updateBaudRate(EXPECTED_BAUD);
  status(s, "waiting for it at 9600...");
  uint32_t waitStart = millis();
  while (!sensorResponds(s)) {
    if (millis() - waitStart > POWERCYCLE_WAIT_MS) {
      status(s, "ERROR: never came back at 9600, marked offline");
      s.online = false;
      return;
    }
    delay(500);
  }
  s.baud = EXPECTED_BAUD;
  status(s, "back online at 9600");
}

// Timestamp as seconds since boot with 3 decimals. Printed from the integer
// millisecond count rather than millis()/1000.0f so the value stays exact:
// a float loses millisecond resolution after ~4.7 hours of uptime.
void printSeconds(uint32_t ms) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu.%03lu",
           (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  Serial.print(buf);
}

void setup() {
  Serial.begin(USB_BAUD);

  // Native USB enumerates ~1 s after boot; without this wait the first
  // status lines are written before the host has opened the port and lost.
  uint32_t usbWaitStart = millis();
  while (!Serial && millis() - usbWaitStart < USB_CONNECT_TIMEOUT_MS) delay(10);
  delay(200);   // let the host-side port settle before the first write

  delay(1000);  // give both sensors time to boot

  // Sequential so the power-cycle prompts never overlap.
  for (uint8_t i = 0; i < NUM_SENSORS; i++) initSensor(sensors[i]);

  // Retry rather than halt: lets you fix wiring and watch it come up live.
  while (true) {
    uint8_t onlineCount = 0;
    for (uint8_t i = 0; i < NUM_SENSORS; i++) if (sensors[i].online) onlineCount++;
    if (onlineCount > 0) break;
    Serial.println("# ERROR: no sensor found on either bus, retrying in 5 s");
    Serial.println("# check sensor supply (5-32 V), A/B pair, and common GND");
    delay(5000);
    for (uint8_t i = 0; i < NUM_SENSORS; i++)
      if (!sensors[i].online) initSensor(sensors[i]);
  }

  Serial.println("seconds,lux1,lux2");  // CSV header for the host logger
}

void loop() {
  uint32_t raw;
  float lux[NUM_SENSORS];
  bool ok[NUM_SENSORS];

  // Buses are independent, but polling is sequential: at 9600 baud one
  // transaction is ~18 ms, so the two samples in a row are ~20 ms apart.
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    ok[i] = sensors[i].online && readLuxRaw(sensors[i], raw);
    if (ok[i]) lux[i] = raw / 1000.0f;
  }

  printSeconds(millis());
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    Serial.print(",");
    if (ok[i]) Serial.print(lux[i], 3);
  }
  Serial.println();

  // Recovery: a sensor that stops answering gets a full re-init (re-detect
  // baud, re-apply level 1, rewrite the baud register), and one that never
  // answered is retried on a timer, so fixing the wiring brings it back
  // without a reflash or a reset.
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    Sensor &s = sensors[i];
    if (s.online) {
      if (ok[i]) {
        s.consecFail = 0;
        continue;
      }
      s.consecFail++;
      if (s.consecFail >= RECOVER_AFTER_FAILS) {
        status(s, "lost contact, re-running detection and baud reset");
        initSensor(s);
      } else {
        Serial.print("# [");
        Serial.print(s.name);
        Serial.println("] read error");
      }
    } else if (millis() - s.lastAttempt >= OFFLINE_RETRY_MS) {
      initSensor(s);
    }
  }

  delay(POLL_DELAY_MS);
}
