#include <SD.h>
#include <U8g2lib.h>
#include <RTClib.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <DHT.h>
#include <lmic.h>
#include <hal/hal.h>

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

// LoraWAN configuration
static const u1_t PROGMEM APPEUI[8] = { 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43 };
void os_getArtEui(u1_t* buf) {
  memcpy_P(buf, APPEUI, 8);
}

static const u1_t PROGMEM DEVEUI[8] = { 0x85, 0x49, 0x00, 0xD8, 0x7E, 0xD5, 0xB3, 0x70 };
void os_getDevEui(u1_t* buf) {
  memcpy_P(buf, DEVEUI, 8);
}

static const u1_t PROGMEM APPKEY[16] = { 0x81, 0xE6, 0xAC, 0x7B, 0xB1, 0x9D, 0xE8, 0x13, 0xCD, 0x0A, 0xE6, 0xD4, 0xBC, 0x87, 0x17, 0xC9 };
void os_getDevKey(u1_t* buf) {
  memcpy_P(buf, APPKEY, 16);
}

char rxData[37];

// WiFi credential
const char* ssid = "64bit";
const char* pass = "meowh3h3";

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

static osjob_t sendjob;
const unsigned TX_INTERVAL = 5;

File file;
RTC_DS3231 rtc;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset =*/U8X8_PIN_NONE);
DHT dht(DHT_pin, DHT_type);
SPIClass hspi(HSPI);

//Lora pin mapping
const lmic_pinmap lmic_pins = {
  .nss = 15,
  .rxtx = LMIC_UNUSED_PIN,
  .rst = 4,
  .dio = { 34, 39, 36 }
};

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, rx2, tx2);
  u8g2.begin();
  Wire.begin();
  WiFi.begin(ssid, pass);
  dht.begin();
  hspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);


  Serial.print("Connecting to WiFi.");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print('.');
  }
  Serial.println("Connected to WiFi");


  // need WiFi to set time for the rtc, once time is set WiFi is turned off
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

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.println("WiFi disconnected and turned off.");
  while (!SD.begin(5, hspi)) {
    Serial.println("SD card mount failed, trying again in 1 second");
    delay(1000);
  }

  //uncomment if sd card does not have weather_data_log.csv, uncommenting the following might erase all data in the file if it already exists
  // file = SD.open("/weather_data_log.csv", FILE_WRITE);
  // file.println("Date,Time,wind_dir(degree),wind_speed(m/s),greatest_wind_speed_5min(m/s),Temperature(celsius),humidity(%),rainfall_1hr(mm/h),rainfall_24hr(mm/h),atm_pressure(hPa)");
  // file.close();

  os_init();     // Initialize LMIC runtime and internal scheduler
  LMIC_reset();  // Reset LoRaWAN MAC state (session, timers, queues)

  Serial.println("Weather station - start");

  rxData[36] = '\0';  //adding null pointer
}

void loop() {
  DateTime now = rtc.now();
  sprintf(pd_date, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  sprintf(pd_time, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  int secondNow = now.second();
  if (secondNow % 5 == 0 && secondNow != lastSecond) {
    performWeatherTask(now);
    lastSecond = secondNow;
  }
  os_runloop_once();
}
void performWeatherTask(DateTime now) {
  getSensorReadings();
  checkValidity();
  if (validData) calcParameters();
  logData();
  transmitData(&sendjob);
  displayData();
  printData();
}

void onEvent(ev_t ev) {
  Serial.print(os_getTime());
  Serial.print(": ");
  switch (ev) {
    case EV_JOINING:
      Serial.println(F("EV_JOINING"));
      break;
    case EV_JOINED:
      Serial.println(F("EV_JOINED"));
      LMIC_setLinkCheckMode(0);
      break;
    case EV_TXCOMPLETE:
      Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
      if (LMIC.txrxFlags & TXRX_ACK)
        Serial.println(F("Received ack"));
      if (LMIC.dataLen) {
        Serial.print(F("Received "));
        Serial.print(LMIC.dataLen);
        Serial.println(F(" bytes of payload"));
      }
      // Schedule the next transmission after TX_INTERVAL seconds
      os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(TX_INTERVAL), transmitData);
      break;
    case EV_JOIN_FAILED:
      Serial.println(F("EV_JOIN_FAILED"));
      break;
    case EV_TXSTART:
      Serial.println(F("EV_TXSTART"));
      break;
    default:
      Serial.print(F("Unknown event: "));
      Serial.println((unsigned)ev);
      break;
  }
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

int toInt100(float val) { // converting floating point value to integer
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

void transmitData(osjob_t* j) {
  uint8_t payload[52];

  memcpy(payload, &windDir, 4);
  memcpy(payload + 4, &windSpeed, 4);
  memcpy(payload + 8, &highWindSpeed, 4);
  memcpy(payload + 12, &temp, 4);
  memcpy(payload + 16, &humidity, 4);
  memcpy(payload + 20, &airPressure, 4);
  memcpy(payload + 24, &rainfall, 4);
  memcpy(payload + 28, &rainfall24, 4);
  memcpy(payload + 32, &pd_date, 11);
  memcpy(payload + 43, &pd_time, 9);

  if (LMIC.opmode & OP_TXRXPEND) {
    Serial.println(F("OP_TXRXPEND, not sending"));
  } else {
    // queue the packet for sending
    LMIC_setTxData2(1, payload, sizeof(payload), 0);
    Serial.println(F("Packet queued"));
  }
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
