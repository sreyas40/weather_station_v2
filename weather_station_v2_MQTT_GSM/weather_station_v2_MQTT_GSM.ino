//gsm module , needs to be declared before GSM client
#define TINY_GSM_MODEM_A7670

#include "secrets.h"
#include <SD.h>
#include <U8g2lib.h>
#include <RTClib.h>
#include <time.h>
#include <Wire.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>

//ws3 uart pins
#define rx2 16
#define tx2 17
//dht config
#define DHT_pin 23
#define DHT_type DHT11
// SD Card pins on HSPI
#define SD_CS 5
#define SD_MOSI 13
#define SD_MISO 12
#define SD_SCK 14
//modem uart pins
#define MODEM_RX 26
#define MODEM_TX 25
#define MODEM_PW_KEY 19  //modem power key

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
TinyGsm modem(SerialAT);

StaticJsonDocument<200> message;


void setup() {
  pinMode(MODEM_PW_KEY, OUTPUT);

  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, rx2, tx2);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);  // for serial communication between gsm module
  Serial.println("[SYS] Weather Station Start");
  u8g2.begin();
  Wire.begin();
  dht.begin();
  hspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  Serial.println("[GSM] Powering on GSM");
  digitalWrite(MODEM_PW_KEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PW_KEY, LOW);

  modem.restart();
  Serial.println("[GSM] Initializing Modem");
  while (!modem.waitForNetwork()) {
    delay(1000);
  }
  Serial.println("[GSM] connected to Cellular network");

  while (!modem.gprsConnect(GSM_APN, GSM_USER, GSM_PASS)) {
    delay(1000);
  }
  Serial.println("[GSM] connected to APN");


  modem.mqtt_begin(true);
  modem.mqtt_set_certificate(AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE);

  int year, month, day, hour, minute, second;
  float timezone;
  while (!modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &timezone)) {
    Serial.println("Waiting for NTP servers to respond");
    delay(1000);
  }
  Serial.printf("Network time: %04d/%02d/%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);

  if (!rtc.begin()) {
    Serial.printf("RTC not found, --stopping");
    while (1)
      ;
  }
  rtc.adjust(DateTime(year, month, day, hour, minute, second));

  while (!SD.begin(5, hspi)) {
    Serial.println("SD card mount failed, trying again in 1 second");
    delay(1000);
  }

  //uncomment if sd card does not have weather_data_log.csv, uncommenting the following might erase all data in the file if it already exists
  // file = SD.open("/weather_data_log.csv", FILE_WRITE);
  // file.println("Date,Time,wind_dir(degree),wind_speed(m/s),greatest_wind_speed_5min(m/s),Temperature(celsius),humidity(%),rainfall_1hr(mm/h),rainfall_24hr(mm/h),atm_pressure(hPa)");
  // file.close();

  rxData[36] = '\0';  //adding null pointer

  connectToAWS();
}

void loop() {
  // if (!modem.mqtt_connected())  // keeping MQTT service alive
  // {
  //   Serial.println("[AWS] Disconnected from MQTT broker");
  //   connectToAWS();
  // }
  DateTime now = rtc.now();
  sprintf(pd_date, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  sprintf(pd_time, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  int secondNow = now.second();
  if (secondNow % 5 == 0 && secondNow != lastSecond) {
    performWeatherTask(now);
    lastSecond = secondNow;
  }
}


void performWeatherTask(DateTime now) {
  Serial.println("Hello");
  getSensorReadings();
  checkValidity();
  if (validData) calcParameters();
  logData();
  displayData();
  if (!modem.mqtt_connected()) {
    Serial.println("[AWS] Disconnected from MQTT broker");
    connectToAWS();
    return;  // to skip outdated readings from sending
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
  if (!modem.isNetworkConnected()) {
    Serial.println("[GSM] Disconnected from Cellular network");
    Serial.println("[GSM] Attempting reconnection to Cellular network");
    modem.restart();
    while (!modem.waitForNetwork()) {
      delay(1000);
    }
    Serial.println("[GSM] connected to Cellular network");
  }

  if (!modem.isGprsConnected()) {
    Serial.println("[GSM] Disconnected from APN");
    Serial.println("[GSM] Attempting reconnection to APN");
    while (!modem.gprsConnect(GSM_APN, GSM_USER, GSM_PASS)) {
      delay(1000);
    }
    Serial.println("[GSM] connected to APN");
  }


  Serial.println("[AWS] Connecting to MQTT broker");
  while (!modem.mqtt_connect(0, AWS_ENDPOINT, AWS_PORT, THING_NAME)) {  // 0 for modems mqtt client
    delay(2000);
  }
  if (modem.mqtt_connected()) {
    Serial.println("[AWS] Connected to MQTT broker");
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
  modem.mqtt_publish(0, AWS_IOT_PUB_TOPIC, messageBuffer);
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
