// Servo light-switch on ESP32 (GPIO13) with WiFi + mDNS + HTTP control
//
// Wiring:
//   Servo signal -> GPIO13
//   Servo V+     -> external 5V supply (do not power from ESP32 3V3)
//   Servo GND    -> common GND with ESP32 and supply
//
// Dependencies:
//   arduino-cli lib install ESP32Servo
//
// Credentials:
//   Copy secrets.h.example to secrets.h and set WIFI_SSID / WIFI_PASSWORD.
//   Also set CONTROL_USER / CONTROL_PASSWORD for HTTP Basic Auth.
//   secrets.h is gitignored; do not commit real credentials.
//
// Compile (run from motor_control/bin):
//   arduino-cli compile --fqbn esp32:esp32:esp32 servo
//
// Upload (adjust port as needed):
//   arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 servo
//
// Serial monitor:
//   arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
//
// HTTP control once connected (mDNS hostname: light-switch):
//   curl -u admin:PASSWORD "http://light-switch.local/press"         toggle ON/OFF once
//   curl -u admin:PASSWORD "http://light-switch.local/reset"         toggle twice (fast double tap)
//   curl -u admin:PASSWORD "http://light-switch.local/dim"           dim once (quick double tap)
//   curl -u admin:PASSWORD "http://light-switch.local/dim?times=4"   dim with 4 taps
//   curl -u admin:PASSWORD "http://light-switch.local/status"
//   All /press, /reset, /dim and /status requests require HTTP Basic Auth.
//   Note: Basic Auth is base64, not encrypted; use only on a trusted LAN.
//
// Behavior:
//   Switch positions: ON/OFF toggle at TOGGLE_ANGLE (122), dimming at
//   DIM_ANGLE (69), rest at REST_ANGLE (90).
//   /press  : rest -> toggle, hold, rest. One ON/OFF toggle.
//   /reset  : rest -> toggle, hold, dip to DOUBLE_TAP_MID_ANGLE (120),
//             back to toggle, hold, rest. A fast double toggle that never
//             returns all the way to rest between taps.
//   /dim    : one dim is a quick double tap: rest -> dim, hold, release
//             to DIM_TAP_MID_ANGLE (hold DIM_RELEASE_HOLD_MS), back to
//             dim, hold, rest. /dim?times=N does N taps (default 2,
//             max MAX_DIM_TIMES).
//   Shorten PRESS_HOLD_MS to the minimum your switch needs.
//
// Serial control (115200 baud):
//   p      toggle ON/OFF once (same as /press)
//   r      fast double toggle (same as /reset)
//   d      dim once, a quick double tap (same as /dim)
//   dN     dim with N taps (e.g. d4, max 5)
//   s      print servo status
//   w      print WiFi status and IP (if connected)
//   0-180  debug: move directly to that angle (cancels any running sequence)
//
// Power saving:
//   - CPU runs at 80 MHz instead of 240 MHz.
//   - WiFi modem sleep is enabled (slightly higher HTTP latency).
//   - Servo pulses are stopped (release, not detach) after SERVO_IDLE_RELEASE_MS
//     at rest so it stops drawing holding current. Release keeps the last pulse
//     width, which avoids the detach/reattach jump to the library's default
//     angle. Set the constant to 0 to keep holding torque at rest.
//   - Motion is slew-limited over MOVE_MS, which lowers peak current.
//     Raise MOVE_MS for gentler starts; lower it for a faster press.

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include "secrets.h"

const int SERVO_PIN = 13;
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2400;

const int REST_ANGLE = 90;
const int TOGGLE_ANGLE = 122;
const int DIM_ANGLE = 69;
const int DOUBLE_TAP_MID_ANGLE = 115;
const int DIM_TAP_MID_ANGLE = 75;
const unsigned long MOVE_MS = 400;
const unsigned long PRESS_HOLD_MS = 100;
const unsigned long FAST_MOVE_MS = 150;
const unsigned long FAST_HOLD_MS = 250;
const unsigned long DIM_RELEASE_HOLD_MS = 200;
const int MAX_STEPS = 32;
const int MAX_DIM_TIMES = 5;
const char* HOSTNAME = "light-switch";

const uint32_t CPU_MHZ = 80;
const wifi_power_t WIFI_TX_POWER = WIFI_POWER_20_5dBm;
const unsigned long SERVO_IDLE_RELEASE_MS = 1000;

Servo servo;
WebServer server(80);

struct Step {
  int angle;
  unsigned long moveMs;
  unsigned long holdMs;
};

enum PressState { IDLE, MOVING, HOLDING, MOVING_TO_REST };
PressState pressState = IDLE;
unsigned long stateSince = 0;
Step steps[MAX_STEPS];
int stepCount = 0;
int stepIndex = 0;
int currentAngle = REST_ANGLE;
int fromAngle = REST_ANGLE;
int toAngle = REST_ANGLE;
bool mdnsStarted = false;
bool lastWifiConnected = false;
bool servoReleased = false;
unsigned long lastActivity = 0;

void ensureServoAttached() {
  if (!servo.attached()) servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  servoReleased = false;
}

int interpolateAngle(int a, int b, unsigned long elapsed, unsigned long duration) {
  if (duration == 0 || elapsed >= duration) return b;
  return a + (int)(((long)(b - a) * (long)elapsed) / (long)duration);
}

void setAngle(int angle) {
  ensureServoAttached();
  currentAngle = constrain(angle, 0, 180);
  servo.write(currentAngle);
  lastActivity = millis();
}

void clearSteps() {
  stepCount = 0;
}

void addStep(int angle, unsigned long moveMs, unsigned long holdMs) {
  if (stepCount >= MAX_STEPS) return;
  steps[stepCount].angle = constrain(angle, 0, 180);
  steps[stepCount].moveMs = moveMs;
  steps[stepCount].holdMs = holdMs;
  stepCount++;
}

void buildToggleSequence() {
  clearSteps();
  addStep(TOGGLE_ANGLE, MOVE_MS, PRESS_HOLD_MS);
}

void buildDoubleTapSequence() {
  clearSteps();
  addStep(TOGGLE_ANGLE, MOVE_MS, PRESS_HOLD_MS);
  addStep(DOUBLE_TAP_MID_ANGLE, FAST_MOVE_MS, FAST_HOLD_MS);
  addStep(TOGGLE_ANGLE, FAST_MOVE_MS, PRESS_HOLD_MS);
}

void buildDimSequence(int times) {
  clearSteps();
  for (int i = 0; i < times; i++) {
    addStep(DIM_ANGLE, MOVE_MS, PRESS_HOLD_MS);
    if (i + 1 < times) addStep(DIM_TAP_MID_ANGLE, FAST_MOVE_MS, DIM_RELEASE_HOLD_MS);
  }
}

void startSequence() {
  if (stepCount == 0) return;
  ensureServoAttached();
  stepIndex = 0;
  fromAngle = currentAngle;
  toAngle = steps[stepIndex].angle;
  pressState = MOVING;
  stateSince = millis();
  lastActivity = stateSince;
}

void updatePressCycle() {
  unsigned long now = millis();

  if (pressState == MOVING || pressState == MOVING_TO_REST) {
    unsigned long moveMs = (pressState == MOVING) ? steps[stepIndex].moveMs : MOVE_MS;
    unsigned long elapsed = now - stateSince;
    currentAngle = interpolateAngle(fromAngle, toAngle, elapsed, moveMs);
    servo.write(currentAngle);

    if (elapsed >= moveMs) {
      currentAngle = toAngle;
      pressState = (pressState == MOVING) ? HOLDING : IDLE;
      stateSince = now;
      lastActivity = now;
    }
  } else if (pressState == HOLDING && now - stateSince >= steps[stepIndex].holdMs) {
    stepIndex++;
    if (stepIndex < stepCount) {
      fromAngle = currentAngle;
      toAngle = steps[stepIndex].angle;
      pressState = MOVING;
    } else if (currentAngle != REST_ANGLE) {
      fromAngle = currentAngle;
      toAngle = REST_ANGLE;
      pressState = MOVING_TO_REST;
    } else {
      pressState = IDLE;
    }
    stateSince = now;
    lastActivity = now;
  }
}

bool requireAuth() {
  // TODO: authenticate via HTTPS
  // if (server.authenticate(CONTROL_USER, CONTROL_PASSWORD)) return true;
  // server.requestAuthentication(BASIC_AUTH, "light-switch");
  return true;
}

const char* stateName() {
  if (pressState == MOVING) return "moving";
  if (pressState == HOLDING) return "holding";
  if (pressState == MOVING_TO_REST) return "releasing";
  return "idle";
}

void sendStatus() {
  if (!requireAuth()) return;
  char body[96];
  snprintf(body, sizeof(body),
           "{\"state\":\"%s\",\"rest\":%d,\"target\":%d,\"angle\":%d}",
           stateName(), REST_ANGLE, toAngle, currentAngle);
  server.send(200, "application/json", body);
}

bool rejectIfBusy() {
  if (pressState == IDLE) return false;
  server.send(409, "application/json", "{\"error\":\"busy\"}");
  return true;
}

void handlePress() {
  if (!requireAuth()) return;
  if (rejectIfBusy()) return;
  buildToggleSequence();
  startSequence();
  server.send(202, "application/json", "{\"status\":\"toggling\"}");
}

void handleReset() {
  if (!requireAuth()) return;
  if (rejectIfBusy()) return;
  buildDoubleTapSequence();
  startSequence();
  server.send(202, "application/json", "{\"status\":\"double_tap\"}");
}

void handleDim() {
  if (!requireAuth()) return;
  if (rejectIfBusy()) return;
  int times = 1;
  if (server.hasArg("times")) {
    int requested = server.arg("times").toInt();
    if (requested >= 1 && requested <= MAX_DIM_TIMES) times = requested;
  }
  buildDimSequence(times);
  startSequence();
  char body[64];
  snprintf(body, sizeof(body), "{\"status\":\"dimming\",\"times\":%d}", times);
  server.send(202, "application/json", body);
}

void handleRoot() {
  server.send(200, "text/plain",
              "light-switch servo\n"
              "GET /press     toggle ON/OFF once\n"
              "GET /reset     fast double toggle\n"
              "GET /dim       dim once (or /dim?times=N)\n"
              "GET /status\n");
}

void handleNotFound() {
  server.send(404, "text/plain", "not found\n");
}

void startMDNS() {
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.printf("mDNS: http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("mDNS start failed");
  }
}

void printWiFiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: connected SSID=%s IP=", WIFI_SSID);
    Serial.println(WiFi.localIP());
    Serial.printf("WiFi: RSSI=%d dBm\n", WiFi.RSSI());
  } else {
    Serial.printf("WiFi: disconnected SSID=%s\n", WIFI_SSID);
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_TX_POWER);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  printWiFiStatus();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: retrying in loop");
  }
}

void setup() {
  setCpuFrequencyMhz(CPU_MHZ);

  Serial.begin(115200);
  Serial.setTimeout(50);

  ensureServoAttached();
  servo.write(REST_ANGLE);
  lastActivity = millis();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) startMDNS();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/press", HTTP_GET, handlePress);
  server.on("/reset", HTTP_GET, handleReset);
  server.on("/dim", HTTP_GET, handleDim);
  server.on("/status", HTTP_GET, sendStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  lastWifiConnected = WiFi.status() == WL_CONNECTED;
  Serial.println("READY");
}

void handleSerial() {
  if (Serial.available() <= 0) return;

  static char cmd_buf[16];
  int len = Serial.readBytesUntil('\n', cmd_buf, sizeof(cmd_buf) - 1);
  cmd_buf[len] = '\0';
  if (len > 0 && cmd_buf[len - 1] == '\r') cmd_buf[--len] = '\0';

  if (len == 0) return;

  if (cmd_buf[0] == 'p' || cmd_buf[0] == 'P') {
    if (pressState == IDLE) {
      buildToggleSequence();
      startSequence();
    }
    Serial.printf("state=%s\n", stateName());
    return;
  }

  if (cmd_buf[0] == 'r' || cmd_buf[0] == 'R') {
    if (pressState == IDLE) {
      buildDoubleTapSequence();
      startSequence();
    }
    Serial.printf("state=%s\n", stateName());
    return;
  }

  if (cmd_buf[0] == 'd' || cmd_buf[0] == 'D') {
    int times = 2;
    if (len > 1) {
      int requested = atoi(cmd_buf + 1);
      if (requested >= 1 && requested <= MAX_DIM_TIMES) times = requested;
    }
    if (pressState == IDLE) {
      buildDimSequence(times);
      startSequence();
    }
    Serial.printf("state=%s dim_times=%d\n", stateName(), times);
    return;
  }

  if (cmd_buf[0] == 's' || cmd_buf[0] == 'S') {
    Serial.printf("state=%s rest=%d target=%d angle=%d\n", stateName(), REST_ANGLE, toAngle, currentAngle);
    return;
  }

  if (cmd_buf[0] == 'w' || cmd_buf[0] == 'W') {
    printWiFiStatus();
    return;
  }

  int angle = atoi(cmd_buf);
  if (angle >= 0 && angle <= 180) {
    pressState = IDLE;
    setAngle(angle);
  }
}

void loop() {
  static unsigned long lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect >= 5000) {
    lastReconnect = millis();
    WiFi.reconnect();
  }

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != lastWifiConnected) {
    lastWifiConnected = connected;
    printWiFiStatus();
    if (connected) {
      if (!mdnsStarted) startMDNS();
    } else {
      MDNS.end();
      mdnsStarted = false;
    }
  }

  updatePressCycle();

  if (SERVO_IDLE_RELEASE_MS > 0 && pressState == IDLE && servo.attached() &&
      !servoReleased && millis() - lastActivity >= SERVO_IDLE_RELEASE_MS) {
    servo.release();
    servoReleased = true;
  }

  server.handleClient();
  handleSerial();
  delay(1);
}
