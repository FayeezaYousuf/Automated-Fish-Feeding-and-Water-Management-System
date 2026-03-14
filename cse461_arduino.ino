#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Servo.h>
#include <RTClib.h>


// LCD Setup
LiquidCrystal_I2C lcd(0x27, 16, 2);


// Temperature sensor setup
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);


//led pin
const int led_pin =4;



// pH sensor setup
const int phPin = A0;
float phSlope = -5.70;
float phIntercept = 21.34;
float smoothedPH = 7.0;
float alpha = 0.2;


// TDS sensor pin
const int TDSPin = A1;


// Servo Setup
Servo feederServo;
const int servoPin = 9;


// RTC Setup
RTC_DS3231 rtc;


// Manual feed input pin from ESP32
const int manualFeedPin = 7;  // Active LOW
// Feed notify pin to ESP32
const int feedNotifyPin = 6;  // Pulse HIGH when feed occurs


// Water change input pin
const int waterChangePin = A2;


// Warning signal pin
const int warningPin = 8;


// Feeding control variables
bool isFeeding = false;
unsigned long feedStartTime = 0;
const unsigned long feedDuration = 10000; // 10 sec
unsigned long lastFeedTime = 0;
const unsigned long feedInterval = 300000; // 5 minutes = 300,000 ms (change this value as needed)


// Sensor reading timer
unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 2000; // every 2 sec


// Water change display flag
bool waterChangeReported = false;


void setup() {
 Serial.begin(115200);


 lcd.begin(16, 2);
 lcd.backlight();
 lcd.setCursor(0,0);
 lcd.print("Water Monitor");
 delay(1500);
 lcd.clear();


 sensors.begin();
 feederServo.attach(servoPin);
 feederServo.write(0); // Start position


 pinMode(manualFeedPin, INPUT_PULLUP);
 pinMode(feedNotifyPin, OUTPUT);
 digitalWrite(feedNotifyPin, LOW);


 pinMode(warningPin, OUTPUT);
 digitalWrite(warningPin, HIGH);


 pinMode(waterChangePin, INPUT_PULLUP); // Active LOW
 pinMode(led_pin, OUTPUT);
 digitalWrite(led_pin, LOW);  // Start OFF

 if (!rtc.begin()) {
   lcd.print("RTC not found!");
   while(1);
 }


 if (rtc.lostPower()) {
   lcd.clear();
   lcd.print("RTC lost power");
   rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
   delay(3000);
   lcd.clear();
 }
}


void loop() {
 DateTime now = rtc.now();


 // --- Water Change Detection ---
 if (digitalRead(waterChangePin) == LOW && !waterChangeReported) {
   lcd.clear();
   lcd.setCursor(0,0);
   lcd.print("Water Change");
   lcd.setCursor(0,1);
   lcd.print("Completed!");
   Serial.println("Water Change Completed!");
   waterChangeReported = true;
   delay(1500);
   lcd.clear();
 }
 if (digitalRead(waterChangePin) == HIGH) {
   waterChangeReported = false;
 }


//  // --- Scheduled feeding (every hour) ---
//  if (!isFeeding && now.hour() != lastFedHour) {
//    startFeeding();
//    lastFedHour = now.hour();
//  }
 // --- Scheduled feeding (every 5 minutes) ---
  if (!isFeeding && (millis() - lastFeedTime >= feedInterval)) {
    lastFeedTime = millis();
    startFeeding();
}

 // --- Manual feeding ---
 if (!isFeeding && digitalRead(manualFeedPin) == LOW) {
   lastFeedTime = millis();
   startFeeding();
 }


 // --- Feeding process ---
 if (isFeeding) {
   lcd.setCursor(0,0);
   lcd.print("Feeding fish...  ");
   lcd.setCursor(0,1);
   lcd.print("Please wait...   ");


   if (millis() - feedStartTime >= feedDuration) {
     feederServo.write(0); // back to start
     isFeeding = false;
     lcd.clear();
     lcd.setCursor(0,0);
     lcd.print("Feeding done!");
     delay(1500);
     lcd.clear();
   }
 } else {
   // Monitor water parameters
   if (millis() - lastSensorRead >= sensorInterval) {
     monitorWater();
     lastSensorRead = millis();
   }
 }
}


void startFeeding() {
 isFeeding = true;
 feedStartTime = millis();
 feederServo.write(180);


 // Notify ESP32
 digitalWrite(feedNotifyPin, HIGH);
 delay(1000); 
 digitalWrite(feedNotifyPin, LOW);


 Serial.println("Fish fed!");
}


void monitorWater() {
 // --- pH ---
 float ph_act = readPH();


 // --- TDS ---
 int tds_raw = analogRead(TDSPin);
 float tds_voltage = tds_raw * (5.0 / 1023.0);
 float tds_value = (133.42 * tds_voltage * tds_voltage * tds_voltage
                  - 255.86 * tds_voltage * tds_voltage
                  + 857.39 * tds_voltage) * 0.5;


 // --- Temperature ---
 sensors.requestTemperatures();
 float tempC = sensors.getTempCByIndex(0);


 // --- LCD Display ---
 lcd.setCursor(0,0);
//  lcd.print("pH:");
//  lcd.print(ph_act,2);
 lcd.print(" Temp: ");
 if (tempC == DEVICE_DISCONNECTED_C) {
   lcd.print("Err ");
 } else {
   lcd.print(tempC,1);
   lcd.print((char)223);
   lcd.print("C");
 }


 lcd.setCursor(0,1);
 lcd.print("TDS: ");
 lcd.print(tds_value,0);
 lcd.print("ppm ");


 // --- Serial Monitor ---
 Serial.print("pH:"); Serial.print(ph_act,2);
 Serial.print(", TDS:"); Serial.print(tds_value,0);
 Serial.print(", Temp:"); Serial.println(tempC);


 // --- Safety Check ---
 bool waterBad = (tds_value < 100 || tds_value > 500 || tempC<18|| tempC>32 );
 digitalWrite(warningPin, waterBad ? LOW : HIGH);
 digitalWrite(led_pin, waterBad ? HIGH : LOW);
}


float readPH() {
 int readings[10];
 for(int i=0;i<10;i++){
   readings[i]=analogRead(phPin);
   delay(5);
 }


 // sort readings
 for(int i=0;i<9;i++){
   for(int j=i+1;j<10;j++){
     if(readings[i]>readings[j]){
       int t=readings[i];
       readings[i]=readings[j];
       readings[j]=t;
     }
   }
 }


 int medianADC = readings[5];
 float voltage = medianADC * 3.0 / 1023.0;
 float phValue = phSlope * voltage + phIntercept;
 smoothedPH = alpha * phValue + (1-alpha) * smoothedPH;
 return smoothedPH;
}



