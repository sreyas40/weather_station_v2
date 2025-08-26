// secrets.h
#ifndef SECRETS_H
#define SECRETS_H

#define SerialAT Serial1

const char* GSM_APN = "http://airtelgprs.com";
const char* GSM_USER = "";
const char* GSM_PASS = "";

// AWS IoT endpoint (from AWS IoT Core -> Settings)
#define AWS_ENDPOINT "xxxxxxxx-ats.iot.us-east-1.amazonaws.com"
#define AWS_PORT 8883
// Thing name (must match the Thing in AWS IoT Core)
#define THING_NAME "GSM"

// Topic names
//#define AWS_IOT_SUB_TOPIC  "my/sub/topic"
#define AWS_IOT_PUB_TOPIC  "weather_sta/asi_campus"

// WiFi credential
const char* ssid = ""; //Replace with your WiFi SSID
const char* pass = ""; //Replace with your WiFi Password

// Device certificate (PEM format, keep it private!)
static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
-----END CERTIFICATE-----
)EOF";

// Device private key
static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
-----END RSA PRIVATE KEY-----
)EOF";

// Amazon root CA certificate
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
-----END CERTIFICATE-----
)EOF";

#endif
