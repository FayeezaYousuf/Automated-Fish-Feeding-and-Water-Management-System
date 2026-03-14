// === Blynk & Wi-Fi ===
#define BLYNK_TEMPLATE_ID "TMPL6GGo4u6qK"
#define BLYNK_TEMPLATE_NAME "FeedFish"
#define BLYNK_AUTH_TOKEN "UN-Q7GVhP16mOGa3asbFnCscqsaldg2H"
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <WidgetRTC.h>
#include <TimeLib.h>

// Wi-Fi credentials
char ssid[] = "Yousuf";
char pass[] = "1234567890";

// === Pins ===
#define FEED_PIN        15 // ESP32 pin connected to Arduino digital input
#define WARNING_PIN     4
#define WASTE_LEVEL_PIN 35
#define CLEAN_LEVEL_PIN 34
#define WATER_CHANGE_DONE_PIN  21  // ESP32 pin connected to Arduino digital input
#define FEED_FROM_ARD 22 // ESP32 pin connected to Arduino digital OUTPUT

// === NEW: Water Flow Sensor ===
#define FLOW_PIN 23   // Flow sensor signal pin (interrupt)

// L298N Motor Driver pins
#define ENA 25
#define IN1 26
#define IN2 27
#define ENB 33
#define IN3 32
#define IN4 14

// === Feed Count Tracking ===
int feedCountToday = 0;
int lastFeedDay = -1;

// Times
const unsigned long DRAIN_TIME = 60000; // 60s
const unsigned long notifyInterval = 60000;

// === NEW: Flow control constants ===
const float FLOW_CALIBRATION = 7.5;     // YF-S201
const float TARGET_FILL_LITERS = 0.7;  // liters to fill
const unsigned long Fill_TIME = 60000; // 60s

// Float switch logic
const bool WASTE_FULL_ACTIVE_HIGH  = true;
const bool CLEAN_EMPTY_ACTIVE_HIGH = false;

// RTC + tracking
WidgetRTC rtc;
String lastFeedTime = "Not fed yet";
String lastWaterChangeTime = "Not changed yet";
time_t lastFeedEpoch = 0;

// Notification control
unsigned long lastNotifyTime = 0;

// === NEW: Flow sensor variables ===
volatile unsigned long flowPulseCount = 0;
float filledLiters = 0.0;
unsigned long flowLastTime = 0;
volatile unsigned long lastPulseMicros = 0;
volatile bool fillPumpRunning = false;


// === Water Change State Machine ===
enum WaterChangeState { IDLE, START_DRAIN, WAIT_DRAIN, START_FILL, WAIT_FILL, DONE };
WaterChangeState wcState = IDLE;
unsigned long wcStartTime = 0;
bool waterChangeRequested = false;

// === NEW: Flow sensor ISR ===
void IRAM_ATTR flowPulseISR() {
  if (!fillPumpRunning) return;  // ignore pulses if pump is OFF

  unsigned long nowMicros = micros();

  // Reject pulses closer than 2ms (noise)
  if (nowMicros - lastPulseMicros > 2000) {
    flowPulseCount++;
    lastPulseMicros = nowMicros;
  }
}


// === Pump helpers ===
void pumpStopDrain() {
  digitalWrite(ENA, LOW);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

void pumpStopFill() {
  fillPumpRunning = false;   // <<< ADD
  digitalWrite(ENB, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void pumpRunDrain() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(ENA, HIGH);
}

void pumpRunFill() {
  fillPumpRunning = true;   // <<< ADD
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  digitalWrite(ENB, HIGH);
}

// === Blynk Handlers ===
BLYNK_WRITE(V0) {
  if (param.asInt() == 1) {
    feedFish();
  }
}

BLYNK_WRITE(V1) {
  if (param.asInt() == 1) {
    if (digitalRead(WARNING_PIN) == LOW) {
      requestWaterChange();
    } else {
      Blynk.logEvent("water_warning", "✅ Water quality is safe, no need to change water.");
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(WATER_CHANGE_DONE_PIN, OUTPUT);
  digitalWrite(WATER_CHANGE_DONE_PIN, LOW);  // ensure LOW initially

  pinMode(FEED_FROM_ARD, INPUT_PULLUP);  // <-- Arduino pulses this line

  pinMode(FEED_PIN, OUTPUT);
  digitalWrite(FEED_PIN, HIGH);

  pinMode(WARNING_PIN, INPUT_PULLUP);
  pinMode(WASTE_LEVEL_PIN, INPUT_PULLUP);
  pinMode(CLEAN_LEVEL_PIN, INPUT_PULLUP);

  // === NEW: Flow sensor setup ===
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);

  pinMode(ENA, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(ENB, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pumpStopDrain();
  pumpStopFill();

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  rtc.begin();
}

void loop() {
  Blynk.run();

  // Inside loop() add this check:
  if (digitalRead(FEED_FROM_ARD) == LOW) {
    // nothing (default HIGH), wait for pulse
  } else {
    // Arduino sent feed pulse
    feedFishUpdateFromArduino();
    Serial.print("feed Fish Update From Arduino");
  }

  // Float debug
  int wasteRaw = digitalRead(WASTE_LEVEL_PIN);
  int cleanRaw = digitalRead(CLEAN_LEVEL_PIN);
  Serial.print("WasteRaw=");
  Serial.print(wasteRaw);
  Serial.print(" | CleanRaw=");
  Serial.println(cleanRaw);

  // Water quality alert
  if (digitalRead(WARNING_PIN) == LOW) {
    unsigned long currentTime = millis();
    if (currentTime - lastNotifyTime >= notifyInterval) {
      Blynk.logEvent("water_warning", "⚠️ Water quality harmful! Check immediately.");
      lastNotifyTime = currentTime;
    }
  }

  // Feed alert
  static bool feedNotified = false;
  if (lastFeedEpoch != 0) {
    time_t nowEpoch = now();
    if ((nowEpoch - lastFeedEpoch) >= 60UL) {
      if (!feedNotified) {
        Serial.print(" Fish not fed in last 1 minute!\n");
        Blynk.logEvent("feed", "⚠️ Fish not fed in last 1 minute!");
        feedNotified = true;
      }
    } else {
      feedNotified = false;
    }
  }

  // Water change state machine
  unsigned long nowMillis = millis();
  switch (wcState) {

    case START_DRAIN:
      Serial.println("Draining...");
      pumpRunDrain();
      wcStartTime = nowMillis;
      wcState = WAIT_DRAIN;
      break;

    case WAIT_DRAIN:
      if (nowMillis - wcStartTime >= DRAIN_TIME) {
        pumpStopDrain();
        Serial.println("Drain complete.");
        wcState = START_FILL;
      }
      break;

    case START_FILL:
      Serial.println("Filling (flow controlled)...");
      pumpRunFill();

      // === NEW: Reset flow counters ===
      flowPulseCount = 0;
      filledLiters = 0;
      flowLastTime = millis();
      lastPulseMicros = micros();   // <<< ADD THIS
      wcStartTime = nowMillis;
      wcState = WAIT_FILL;
      break;

    case WAIT_FILL:
      // === NEW: Flow-based filling ===
      if (millis() - flowLastTime >= 1000) {
        detachInterrupt(digitalPinToInterrupt(FLOW_PIN));

        float litersThisSecond = flowPulseCount / (FLOW_CALIBRATION * 60.0);
        filledLiters += litersThisSecond;

        Serial.print("Pulses: ");
        Serial.print(flowPulseCount);
        Serial.print(" | Added: ");
        Serial.print(litersThisSecond, 3);
        Serial.print(" L | Total: ");
        Serial.print(filledLiters, 2);
        Serial.println(" L");

        Serial.print("Flow filling: ");
        Serial.print(filledLiters);
        Serial.println(" L");

        flowPulseCount = 0;
        flowLastTime = millis();

        attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);
      }

      //if (filledLiters >= TARGET_FILL_LITERS) {
      if (nowMillis - wcStartTime >= Fill_TIME || filledLiters >= TARGET_FILL_LITERS) {
        pumpStopFill();
        Serial.println("Fill complete (by flow sensor).");

        // Signal Arduino that water change is done
        digitalWrite(WATER_CHANGE_DONE_PIN, HIGH);
        delay(1500);
        digitalWrite(WATER_CHANGE_DONE_PIN, LOW);
        Blynk.logEvent("water_change_done", "✅ Water change done!");
        lastWaterChangeTime = String(day()) + "/" + String(month()) + "/" + String(year()) +
                            " " + String(hour()) + ":" + (minute() < 10 ? "0" : "") + String(minute());
        Blynk.virtualWrite(V3, "Last Water Change: " + lastWaterChangeTime); 
        wcState = DONE;
      }
      break;

    case DONE:
      waterChangeRequested = false;
      wcState = IDLE;
      break;

    default:
      break;
  }

  // === Reset feed count at midnight ===
  if (day() != lastFeedDay) {
    feedCountToday = 0;
    lastFeedDay = day();
    Blynk.virtualWrite(V4, "Feed Count Today: " + String(feedCountToday));
  }

  delay(1000); // for debug prints only
}

// === Actions ===
void feedFish() {
  Serial.println("Feeding fish...");
  updateFeedCount();
  digitalWrite(FEED_PIN, LOW);
  delay(1500);
  digitalWrite(FEED_PIN, HIGH);

  lastFeedTime = String(day()) + "/" + String(month()) + "/" + String(year()) +
                 " " + String(hour()) + ":" + (minute() < 10 ? "0" : "") + String(minute());
  lastFeedEpoch = now();
  delay(10000); //new
  Blynk.virtualWrite(V2, "Last Feed: " + lastFeedTime);
  Blynk.logEvent("feed", "✅ Fish fed!");
}

void feedFishUpdateFromArduino() {
Serial.println("Feed signal received from Arduino.");
lastFeedTime = String(day()) + "/" + String(month()) + "/" + String(year()) +
               " " + String(hour()) + ":" + (minute() < 10 ? "0" : "") + String(minute());
lastFeedEpoch = now();
updateFeedCount();
Blynk.virtualWrite(V2, "Last Feed: " + lastFeedTime);
delay(500); //new
Blynk.logEvent("feed", "✅ Fish fed (RTC/Manual via Arduino)!");
 // small debounce delay so multiple pulses don't register
delay(500);
}


void requestWaterChange() {
  int wasteRaw = digitalRead(WASTE_LEVEL_PIN);
  int cleanRaw = digitalRead(CLEAN_LEVEL_PIN);

  bool wasteFull  = WASTE_FULL_ACTIVE_HIGH  ? (wasteRaw == HIGH) : (wasteRaw == LOW);
  bool cleanEmpty = CLEAN_EMPTY_ACTIVE_HIGH ? (cleanRaw == HIGH) : (cleanRaw == LOW);

  if (wasteFull) {
    Serial.println("Blocked: Waste pot FULL.");
    Blynk.logEvent("waste_pot_full", "⚠️ Wastewater pot FULL! Empty it.");
    return;
  }
  if (cleanEmpty) {
    Serial.println("Blocked: Clean pot EMPTY.");
    Blynk.logEvent("clean_pot_empty", "⚠️ Clean water pot EMPTY! Fill it.");
    return;
  }

  waterChangeRequested = true;
  wcState = START_DRAIN;
}

void updateFeedCount() {
  int today = day();
  if (lastFeedDay != today) {
    feedCountToday = 0;
    lastFeedDay = today;
  }
  feedCountToday++;
  Blynk.virtualWrite(V4, "Feed Count Today: " + String(feedCountToday));
  Serial.print("Feed Count Today: ");
  Serial.println(feedCountToday);
}
