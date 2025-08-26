#include "secrets.h"
#include <SD.h>
#include <U8g2lib.h>
#include <RTClib.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <DHT.h>
#include <lmic.h>
#include <hal/hal.h>
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>

//ws3 uart pins
#define rx2 16
#define tx2 17
//dht config
#define DHT_pin 26
#define DHT_type DHT11
// SD Card pins on HSPI
#define SD_CS 5
#define SD_MOSI 13
#define SD_MISO 12
#define SD_SCK 14


char rxData[37];

// Weather Parameters - Initialization
int windDir = 0;        // degrees × 100
int windSpeed = 0;      // m/s × 100
int highWindSpeed = 0;  // m/s × 100   highest wind speed in 5 minutes
int temp = 0;           // Celsius × 100
int humidity = 0;       // % × 100
int airPressure = 0;    // hPa × 100
int rainfall = 0;       // mm/h × 100
int rainfall24 = 0;     // mm/h × 100

char pd_time[9];   // sensor reading time
char pd_date[11];  //sensor reading date

int validData = 0;
int lastSecond = -1;

bool clientInitialized = 0;
bool gotWiFi = 0;


File file;
RTC_DS3231 rtc;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset =*/U8X8_PIN_NONE);
DHT dht(DHT_pin, DHT_type);
SPIClass hspi(HSPI);
WiFiClientSecure net = WiFiClientSecure();  //handles TLS handshake
MQTTClient client = MQTTClient(256);        // mqtt buffer size 256 bytes

StaticJsonDocument<200> message;

//Lora pin mapping
const lmic_pinmap lmic_pins = {
  .nss = 15,
  .rxtx = LMIC_UNUSED_PIN,
  .rst = 4,
  .dio = { 34, 39, 36 }
};

void setup() {

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(WiFiEvent);

  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, rx2, tx2);
  u8g2.begin();
  Wire.begin();
  WiFi.begin(ssid, pass);
  dht.begin();
  hspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // Serial.print("[WiFi] Connecting...");
  // while (WiFi.status() != WL_CONNECTED) {
  //   delay(1000);
  //   Serial.print('.');
  // }

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo;
  while (!getLocalTime(&timeInfo)) {
    Serial.println("Waiting for NTP servers to respond");
    delay(1000);
  }

  if (!rtc.begin()) {
    Serial.printf("RTC not found, --stopping");
    while (1)
      ;
  }
  rtc.adjust(DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));

  while (!SD.begin(5, hspi)) {
    Serial.println("SD card mount failed, trying again in 1 second");
    delay(1000);
  }

  //uncomment if sd card does not have weather_data_log.csv, uncommenting the following might erase all data in the file if it already exists
  // file = SD.open("/weather_data_log.csv", FILE_WRITE);
  // file.println("Date,Time,wind_dir(degree),wind_speed(m/s),greatest_wind_speed_5min(m/s),Temperature(celsius),humidity(%),rainfall_1hr(mm/h),rainfall_24hr(mm/h),atm_pressure(hPa)");
  // file.close();

  Serial.println("Weather station - start");

  rxData[36] = '\0';  //adding null pointer

  connectToAWS();
}

void loop() {
  client.loop();

  DateTime now = rtc.now();
  sprintf(pd_date, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  sprintf(pd_time, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  int secondNow = now.second();
  if (secondNow % 5 == 0 && secondNow != lastSecond) {
    performWeatherTask(now);
    lastSecond = secondNow;
  }
}


void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[WiFi] STA Started");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] Connected to AP");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] Disconnected from AP");
      gotWiFi = 0;
      WiFi.begin(ssid, pass);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WiFi] Got IP: ");
      Serial.println(WiFi.localIP());
      gotWiFi = 1;
      //if(clientInitialized) reconnectToAWS();
      break;
    default:
      Serial.printf("[WiFi] Unhandled event: %d\n", event);
      break;
  }
}


void performWeatherTask(DateTime now) {
  getSensorReadings();
  checkValidity();
  if (validData) calcParameters();
  logData();
  displayData();
  if (!client.connected()) {
    onMQTTDisconnect();
    return;
  }
  publishData();
  printData();
}

void getSensorReadings() {
  for (int i = 0; i < 36; i++) {
    if (Serial2.available()) {
      rxData[i] = Serial2.read();
      if (rxData[0] != 'c') i = -1;
    } else i--;
  }
}

void checkValidity() {
  String cs = getCheckSum();
  byte xorData = 0;
  for (int i = 0; rxData[i] != '*'; i++) {
    xorData ^= rxData[i];
  }
  char hexCS[3];
  sprintf(hexCS, "%02X", xorData);
  //Serial.println(String(hexCS));
  validData = (String(hexCS) == cs);
}

String getCheckSum() {
  char cs[3];  //checksum
  char* ptr = strchr(rxData, '*');
  if (ptr) {
    cs[0] = *(ptr + 1);
    cs[1] = *(ptr + 2);
    cs[2] = '\0';
  }
  return String(cs);
}

int toInt100(float val) {  // converting floating point value to integer
  return (int)(val * 100);
}

void calcParameters() {
  char* ptr = strchr(rxData, 'c');
  if (ptr) windDir = toInt100(atoi(ptr + 1));

  ptr = strchr(rxData, 's');
  if (ptr) windSpeed = toInt100(atoi(ptr + 1) * 0.44704);

  ptr = strchr(rxData, 'g');
  if (ptr) highWindSpeed = toInt100(atoi(ptr + 1) * 0.44704);

  ptr = strchr(rxData, 't');
  if (ptr) temp = toInt100((atoi(ptr + 1) - 32) * 5.0 / 9.0);

  ptr = strchr(rxData, 'r');
  if (ptr) rainfall = toInt100(atoi(ptr + 1) * 25.4 * 0.01);

  ptr = strchr(rxData, 'p');
  if (ptr) rainfall24 = toInt100(atoi(ptr + 1) * 25.4 * 0.01);

  humidity = toInt100(dht.readHumidity());
  if (isnan(humidity / 100.0)) {
    humidity = 0;
    Serial.println("failed to read DHT sensor");
  }

  ptr = strchr(rxData, 'b');
  if (ptr) airPressure = toInt100(atoi(ptr + 1) / 10.0);
}


void logData() {
  file = SD.open("/weather_data_log.csv", FILE_APPEND);
  if (!file) Serial.println("Failed to create or open file");
  file.printf("%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
              pd_date, pd_time,
              windDir / 100.0,
              windSpeed / 100.0,
              highWindSpeed / 100.0,
              temp / 100.0,
              humidity / 100.0,
              rainfall / 100.0,
              rainfall24 / 100.0,
              airPressure / 100.0);
  file.close();
}

void connectToAWS() {
  //AWS Credentials
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  // connecting to MQTT broker
  client.begin(AWS_IOT_ENDPOINT, 8883, net);
  clientInitialized = 1;

    Serial.print("[AWS] Connecting to MQTT broker.");
  while (!client.connect(THING_NAME)) {
    Serial.print('.');
    delay(2000);
  }
  if (client.connected()) {
    Serial.println("[AWS] Connected to MQTT broker");
  }
}

void onMQTTDisconnect() {
  Serial.println("[AWS] Disconnected from MQTT broker");
  if(gotWiFi)reconnectToAWS();
}
void reconnectToAWS() {
  if (!clientInitialized) return;
  client.connect(THING_NAME);
  if (client.connected()) {
    Serial.println("[AWS] Reconnected to MQTT broker, resuming transmission");
  }
}

void publishData() {
  char messageBuffer[200];

  message["windSpeed"] = (float)windSpeed / 100;
  message["highWindSpeed"] = (float)highWindSpeed / 100;
  message["temperature"] = (float)temp / 100;
  message["humidity"] = humidity / 100;
  message["airPressure"] = (float)airPressure / 100;
  message["rainfall1h"] = (float)rainfall / 100;
  message["rainfall24h"] = (float)rainfall24 / 100;
  message["date"] = pd_date;
  message["time"] = pd_time;

  serializeJson(message, messageBuffer);
  client.publish(AWS_IOT_PUB_TOPIC, messageBuffer);
}

void displayData() {  // display values in the OLED screen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);

  char Buff[40];

  sprintf(Buff, "W_Dir: %.2fdeg", windDir / 100.0);
  u8g2.drawStr(0, 10, Buff);

  sprintf(Buff, "W_Spd: %.2fm/s", windSpeed / 100.0);
  u8g2.drawStr(0, 20, Buff);

  sprintf(Buff, "MX: %.2fm/s", highWindSpeed / 100.0);
  u8g2.drawStr(0, 30, Buff);

  sprintf(Buff, "Temp: %.2fC", temp / 100.0);
  u8g2.drawStr(0, 40, Buff);

  sprintf(Buff, "Atm pr: %.2fhPa", airPressure / 100.0);
  u8g2.drawStr(0, 50, Buff);

  sprintf(Buff, "Rain: %.2fmm/h", rainfall / 100.0);
  u8g2.drawStr(67, 10, Buff);

  sprintf(Buff, "R 24: %.2fmm/h", rainfall24 / 100.0);
  u8g2.drawStr(67, 20, Buff);

  sprintf(Buff, "hum: %.2f%%", humidity / 100.0);
  u8g2.drawStr(67, 30, Buff);

  u8g2.sendBuffer();
}

void printData() {  // print values in the serial monitor
  Serial.printf("\n");
  Serial.printf("\n");
  Serial.printf("Data Validity: %d\n", validData);
  Serial.printf("\n");
  Serial.printf("Wind Direction: %.2f°\n", windDir / 100.0);
  Serial.printf("Wind Speed: %.2fm/s\n", windSpeed / 100.0);
  Serial.printf("Max Wind Speed in 5 min: %.2fm/s\n", highWindSpeed / 100.0);
  Serial.printf("Temperature: %.2f°C\n", temp / 100.0);
  Serial.printf("Rainfall in 1 hour: %.2fmm/h\n", rainfall / 100.0);
  Serial.printf("Rainfall in 24 hours: %.2fmm/h\n", rainfall24 / 100.0);
  Serial.printf("Humidity: %.2f%%\n", humidity / 100.0);
  Serial.printf("Atmospheric Pressure: %.2fhPa\n", airPressure / 100.0);
  Serial.printf("\n");
  Serial.printf("\n");
}
