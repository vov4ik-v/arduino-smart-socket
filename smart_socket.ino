#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>

#define VOLTAGE_PIN 1
#define CURRENT_PIN 2
#define RELAY_PIN   3
#define BUTTON1_PIN 6
#define BUTTON2_PIN 5
#define LED_PIN     8

bool relayState = false;
bool serverMode = false;
String serverAddress = "";
String firmwareURL = "";
WebServer server(80);
Preferences preferences;

float acsZero = 2.5;

float adcToVolts(int raw) {
  return (raw / 4095.0) * 3.3;
}

float restoreModuleSignal(float v_adc) {
  return v_adc * ((4.7 + 10.0) / 10.0);
}

float zmptToACVoltage(float moduleV) {
  return moduleV * 80.0;
}

float acs712ToCurrent(float moduleV) {
  float current = (moduleV - acsZero) / 0.185;
  if (current < 0) current = 0;
  return current;
}

int readAveragedSensor(int pin, int samples = 20, int delayPerSample = 2) {
  long sum = 0;
  for (int i = 0; i < samples; ++i) {
    sum += analogRead(pin);
    delay(delayPerSample);
  }
  return sum / samples;
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  Serial.begin(115200);

  preferences.begin("smart-socket", false);
  serverAddress = preferences.getString("server", "");
  preferences.end();

  if (serverAddress != "") {
    Serial.println("Loaded saved server address: " + serverAddress);
  }

  int rawIdle = readAveragedSensor(CURRENT_PIN, 100, 2);
  float idleV = restoreModuleSignal(adcToVolts(rawIdle));
  acsZero = idleV;

  startAccessPoint();
  setupWebServer();
}

void loop() {
  static unsigned long lastSend = 0;
  static unsigned long wifiFailStart = 0;
  static bool wasConnected = true;

  handleButtons();

  if (serverMode) {
    if (WiFi.status() == WL_CONNECTED) {
      serverCommunication();
      lastSend = millis();
      wasConnected = true;
    } else {
      if (wasConnected) {
        wifiFailStart = millis();
        wasConnected = false;
      }
      if (millis() - wifiFailStart > 10000) {
        Serial.println("WiFi failed — switching back to Access Point mode.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(1000); 
        startAccessPoint();
        serverMode = false;
        wasConnected = true;
      }
    }
  } else {
    server.handleClient();
  }
}


void handleButtons() {
  static bool lastButton1 = HIGH;
  static bool lastButton2 = HIGH;

  bool currentButton1 = digitalRead(BUTTON1_PIN);
  bool currentButton2 = digitalRead(BUTTON2_PIN);

  if (lastButton1 == HIGH && currentButton1 == LOW) {
    relayState = !relayState;
    digitalWrite(RELAY_PIN, relayState);
  }

  if (lastButton2 == HIGH && currentButton2 == LOW) {
    if (serverAddress == "") {
      for (int i = 0; i < 5; ++i) {
        digitalWrite(LED_PIN, HIGH);
        delay(150);
        digitalWrite(LED_PIN, LOW);
        delay(150);
      }
      return;
    }
    serverMode = !serverMode;
    if (serverMode) WiFi.begin();
    else startAccessPoint();
  }

  lastButton1 = currentButton1;
  lastButton2 = currentButton2;
  delay(50);
}

void startAccessPoint() {
  WiFi.softAP("SmartSocket");
  Serial.print("Access Point started. IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupWebServer() {
  server.on("/", []() {
    String html = "<html><head>";
    html += "<script>setInterval(() => fetch('/data').then(r => r.json()).then(d => {";
    html += "document.getElementById('v').innerText = d.voltage.toFixed(2);";
    html += "document.getElementById('c').innerText = d.current.toFixed(2);";
    html += "}), 1000);</script></head><body>";
    html += "<h1>Smart Socket</h1>";
    html += "<p>Voltage: <span id='v'>--</span> V</p>";
    html += "<p>Current: <span id='c'>--</span> A</p>";
    html += "<form action='/relay' method='POST'><button type='submit'>Toggle Relay</button></form>";
    html += "<form action='/upload-server' method='POST'><input name='server'><button type='submit'>Set Server</button></form>";
    html += "<form action='/upload-firmware-url' method='POST'><input name='firmware'><button type='submit'>Set Firmware URL</button></form>";
    html += "<form action='/upload-firmware-file' method='POST' enctype='multipart/form-data'>";
    html += "<input type='file' name='update'><button type='submit'>Upload Firmware File</button></form>";
    html += "<form action='/recalibrate' method='POST'><button type='submit'>Recalibrate Current Zero</button></form>";
    html += "<h2>WiFi Connection</h2>";
    html += "<form action='/connect-wifi' method='POST'>";
    html += "SSID: <input name='ssid'><br>";
    html += "Password: <input name='password' type='password'><br>";
    html += "<button type='submit'>Connect WiFi</button></form>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/data", HTTP_GET, []() {
    StaticJsonDocument<128> doc;
    int rawV = readAveragedSensor(VOLTAGE_PIN);
    float moduleV = restoreModuleSignal(adcToVolts(rawV));
    float realVoltage = zmptToACVoltage(moduleV);
    int rawC = readAveragedSensor(CURRENT_PIN);
    float moduleC = restoreModuleSignal(adcToVolts(rawC));
    float realCurrent = acs712ToCurrent(moduleC);
    doc["voltage"] = realVoltage;
    doc["current"] = realCurrent;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on("/relay", HTTP_POST, []() {
    relayState = !relayState;
    digitalWrite(RELAY_PIN, relayState);
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/upload-server", HTTP_POST, []() {
    if (server.hasArg("server")) {
      serverAddress = server.arg("server");

      preferences.begin("smart-socket", false);
      preferences.putString("server", serverAddress);
      preferences.end();

      Serial.println("Saved server address: " + serverAddress);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/upload-firmware-url", HTTP_POST, []() {
    if (server.hasArg("firmware")) {
      firmwareURL = server.arg("firmware");
      Serial.println("Firmware URL set: " + firmwareURL);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/upload-firmware-file", HTTP_POST, []() {
    server.sendHeader("Location", "/");
    server.send(303);
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/recalibrate", HTTP_POST, []() {
    int rawIdle = readAveragedSensor(CURRENT_PIN, 100, 2);
    acsZero = restoreModuleSignal(adcToVolts(rawIdle));
    Serial.println("ACS recalibrated via web");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/connect-wifi", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      String ssid = server.arg("ssid");
      String password = server.arg("password");

      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), password.c_str());

      Serial.println("Connecting to SSID: " + ssid);
      unsigned long startAttemptTime = millis();

      while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        delay(500);
        Serial.print(".");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
        serverMode = true;
      } else {
        Serial.println("\nFailed to connect.");
        startAccessPoint();
      }
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.begin();
}

void serverCommunication() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, serverAddress);

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> requestJson;
  int rawV = readAveragedSensor(VOLTAGE_PIN);
  float moduleV = restoreModuleSignal(adcToVolts(rawV));
  float realVoltage = zmptToACVoltage(moduleV);
  int rawC = readAveragedSensor(CURRENT_PIN);
  float moduleC = restoreModuleSignal(adcToVolts(rawC));
  float realCurrent = acs712ToCurrent(moduleC);

  requestJson["voltage"] = realVoltage;
  requestJson["current"] = realCurrent;
  requestJson["state"] = relayState;

  String requestBody;
  serializeJson(requestJson, requestBody);
  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    StaticJsonDocument<300> responseJson;
    DeserializationError error = deserializeJson(responseJson, http.getString());
    if (!error) {
      if (responseJson.containsKey("relay")) {
        relayState = responseJson["relay"];
        digitalWrite(RELAY_PIN, relayState);
      }
      if (responseJson.containsKey("firmware")) {
        String fw = responseJson["firmware"].as<String>();
        Serial.println("OTA firmware update requested: " + fw);
      }
    }
  }

  http.end();
}