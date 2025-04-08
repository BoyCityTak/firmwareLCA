#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <Wire.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <EEPROM.h>
#include "MCP23008.h"
#include "RTClib.h"
#include "time.h"
#include <ModbusRTUMaster.h>
#include <NimBLEDevice.h>
#include <SD.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>

unsigned int _modeStatus = 0;
#define connectedWifiMode  0
#define pairingBleMode     1

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

#define SD_CS 5

#define MAX_STATES    10
#define MAX_SCHEDULES 10
#define MAX_VALUES    10

/* RS-485 */
#define SLAVE_ID1   1
#define SLAVE_ID2   11
#define RS485_TX    17
#define RS485_RX    16
bool coils[2];
uint16_t holdingRegisters[8];
uint16_t inputRegisters[20];
ModbusRTUMaster modbus(Serial2);

#define BUZZER_PIN 27

MCP23008 MCP    (0x24);
RTC_DS1307 rtc; /* ประกาศใช้ rtc */

/* สถานะการรเชื่อมต่อ wifi */
#define cannotConnect   0
#define wifiConnected   1
#define serverConnected 2
#define statusLedAddDevice  0
#define statusLedWifi       1
int connectWifiStatus = cannotConnect;

WiFiClient espClient; /* ประกาศใช้ WiFiClient */
PubSubClient client(espClient);
WiFiUDP ntpUDP; /* ประกาศใช้ WiFiUDP */
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);
//NTPClient timeClient(ntpUDP);

TaskHandle_t WifiStatus = NULL;
TaskHandle_t BleAddDevice;

unsigned long previousTimeUpdateData = 0;
const unsigned long eventIntervalpublishData = 2 * 60 * 1000;

unsigned long previousTimeDevice = 0;
unsigned long previousTimeWifi = 0;
int _StateBLE = 0, _StateWifi = 0;

unsigned long previousTimeUpdateSensor = 0;
const unsigned long eventIntervalUpdateSensor = 1 * 17 * 1000;

unsigned long previousTimeActivity = 0;
const unsigned long eventIntervalActivity = 60 * 60 * 1000;

StaticJsonDocument<512> doc;
String MqttClientId, MqttSecret, MqttToken, WifiPassword, WifiSSID, MqttServer, MqttPort;

/* HandySense Pro PIN */
const int SW[4]         = {36, 39, 34, 35};
const int relay_pin[4]  = {32, 33, 25, 26};
#define sw_1  1
#define sw_2  2
#define sw_3  3
#define sw_4  4
//int _statusSW[4] = {0, 0, 0, 0};

String startDate;
String endDate;
int totalDays;

bool isFinished         = false;
int formulaCount        = 0;
int lastSentStateIndex  = -1;
String currentState     = "";

struct Schedule {
  String title;
  int startTime;
  int endTime;
  String repeatEvery;
  uint8_t repeatOnBitmask;
  int repeatMode;
};

/* -------- Value -------*/
struct Value {
  String id;
  float targetStart;
  float targetEnd;
};

/* -------- FormulaState -------*/
struct FormulaState {
  String state;
  int startDay;
  int endDay;
  Schedule schedules[10];
  int scheduleCount;
  Value values[10];
  int valueCount;
};
FormulaState formulaStates[10];

TaskHandle_t bleWifiTaskHandle = NULL;
bool _bleActive = false;
unsigned long lastButtonPress = 0;
TaskHandle_t bleTaskHandle = NULL;
static bool bleInitialized = false;
bool bleConnectedFlag = false;

static NimBLEServer* pServer;
static NimBLECharacteristic* pCharacteristic;
bool deviceConnected = false;

struct BleTaskParam {
  BLECharacteristic* characteristic;
  String Bledata;
};

/* -------- Sensor -------*/
float EC, pH, waterTemp;

/* -------- OTA variables -------*/
#define LED_BUILTIN_OTA 26
String firmwareUrl = "";
bool otaRequired = false;

/* --------- Callback function get data from APP ---------- */
void MqttCallback(String topic, byte* payload, unsigned int _length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String message;
  for (int i = 0; i < _length; i++) {
    message = message + (char)payload[i];
  }
  Serial.println();
  Serial.println("message: " + message);
  if (topic.substring(12, 56) == String(MqttClientId + "/formula")) {
    if (topic.substring(12, 65) != String(MqttClientId + "/formula/received")) {
      Serial.println("PlantingFormulaSent...");
      if (!SD.begin(SD_CS)) {
        Serial.println("SD Card initialization failed!");
        //return;
      }
      File file = SD.open("/data.json", FILE_WRITE);
      if (file) {
        file.println(message);
        file.close();
        SD.end();
        Serial.println("JSON saved to SD Card!");
      } else {
        Serial.println("Failed to open file for writing");
      }
      formulaReceived();
    }
  }
  if (String(topic) == MqttClientId + "/firmware/update") {
    StaticJsonDocument<512> URL;
    DeserializationError error = deserializeJson(URL, message);
    if (!error) {
      firmwareUrl = URL["url"].as<String>();
      Serial.println("Firmware URL received: " + firmwareUrl);
      otaRequired = true;
    } else {
      Serial.println("JSON parsing failed for OTA message");
    }
  }
}

void formulaReceived() {
  char msgDataPublish[100];
  String _dataPublish = "@msg/device/";
  _dataPublish += String(MqttClientId);
  _dataPublish += "/formula/received";
  _dataPublish.toCharArray(msgDataPublish, (_dataPublish.length() + 1));
  client.publish(msgDataPublish, "received");

}

/* ---------- For test --------- */
void updateSensorValue() {
  String DatatoWeb;
  char msgtoWeb[400];
  DatatoWeb = "{\" wifiStrength\":" + String(random(90, 100)) +
              ",\"sensors\": [{\"type\": \"AIR_TEMP\", \"value\":" + String(random(20, 30)) +
              "}, {\"type\": \"AIR_HUMIDITY\", \"value\":" + String(random(70, 85)) +
              "}, {\"type\": \"SOIL_HUMIDITY\", \"value\":" + String(random(40, 70)) +
              "}, {\"type\": \"LIGHT_INTENSITY\", \"value\":" + String(random(1, 10)) +
              "}, {\"type\": \"ec\", \"value\":" + String(random(1600, 1800)) +
              "}, {\"type\": \"acid\", \"value\":" + String(random(6, 7)) +
              "}, {\"type\": \"WATER_TEMP\", \"value\":" + String(random(20, 30)) +
              "}, {\"type\": \"WATER_METER\", \"value\":" + String(random(90, 100)) +
              "}, {\"type\": \"ELECTRIC_METER\", \"value\":" + String(random(10, 20)) +
              "}]}";
  Serial.print("DatatoWeb : "); Serial.println(DatatoWeb);
  DatatoWeb.toCharArray(msgtoWeb, (DatatoWeb.length() + 1));
  char msgDataPublish[100];
  String _dataPublish = "@msg/device/";
  _dataPublish += String(MqttClientId);
  _dataPublish += "/sensors";
  _dataPublish.toCharArray(msgDataPublish, (_dataPublish.length() + 1));
  if (client.publish(msgDataPublish, msgtoWeb)) {
    Serial.println("Send Data Complete");
  }
}

/* ---------- For test --------- */
void updateActivitie() {
  String DatatoWeb;
  char msgtoWeb[200];
  String _dataPublishClient = "\"";
  _dataPublishClient += String(MqttClientId);
  _dataPublishClient += "\"";
  Serial.println(_dataPublishClient);

  DatatoWeb = "{\"deviceId\":" + String(_dataPublishClient) +
              ",\"activity\":" "\"WATERING\""
              ",\"data\": {\"duration\":" + String(random(10, 50)) +
              ",\"volume\":" + String(random(1, 5)) +
              "}}";
  Serial.print("DatatoWeb : "); Serial.println(DatatoWeb);
  DatatoWeb.toCharArray(msgtoWeb, (DatatoWeb.length() + 1));

  char msgDataPublish[100];
  String _dataPublish = "@msg/device/";
  _dataPublish += String(MqttClientId);
  _dataPublish += "/activity";
  _dataPublish.toCharArray(msgDataPublish, (_dataPublish.length() + 1));
  if (client.publish(msgDataPublish, msgtoWeb)) {
    Serial.println("Send Data Complete");
  }
}

/* ---------- For test --------- */
void TestUpdateActivitieFertilizer() {
  String DatatoWeb;
  char msgtoWeb[200];
  String _dataPublishClient = "\"";
  _dataPublishClient += String(MqttClientId);
  _dataPublishClient += "\"";

  DatatoWeb = "{\"deviceId\":" + String(_dataPublishClient) +
              ",\"activity\":" "\"FERTILIZING\""
              ",\"data\": {\"duration\":" + String(random(10, 50)) +
              ",\"volume\":" + String(random(50, 60)) +
              ",\"fertilizerA\":" + String(random(10, 100)) +
              ",\"fertilizerB\":" + String(random(10, 100)) +
              ",\"fertilizerC\":" + String(random(10, 100)) +
              "}}";
  Serial.print("DatatoWeb : "); Serial.println(DatatoWeb);
  DatatoWeb.toCharArray(msgtoWeb, (DatatoWeb.length() + 1));

  char msgDataPublish[100];
  String _dataPublish = "@msg/device/";
  _dataPublish += String(MqttClientId);
  _dataPublish += "/activity";
  _dataPublish.toCharArray(msgDataPublish, (_dataPublish.length() + 1));
  if (client.publish(msgDataPublish, msgtoWeb)) {
    Serial.println("Send Data Complete");
  }
}

/* ----------------------- Delete All Config --------------------------- */
void Delete_All_config() {
  for (int b = 0; b < 512; b++) {
    EEPROM.write(b, 255);
    EEPROM.commit();
  }
}

/* --------- SW_ADC ------------- */
int SW_ADC() {
  if (digitalRead(SW[0]) == LOW)      return sw_1;
  else if (digitalRead(SW[1]) == LOW) return sw_2;
  else if (digitalRead(SW[2]) == LOW) return sw_3;
  else if (digitalRead(SW[3]) == LOW) return sw_4;
  return 0;
}

/* --------- toggleLedBlinkForPairing ------------- */
void toggleLedBlinkForPairing() {
  if (millis() - previousTimeDevice >= 500) {
    if (_StateBLE == 0) {
      MCP.digitalWrite(statusLedAddDevice, HIGH); _StateBLE = 1;
    } else if (_StateBLE == 1) {
      MCP.digitalWrite(statusLedAddDevice, LOW); _StateBLE = 0;
    }
    previousTimeDevice = millis();
  }
}

/* --------- showWifiConnectingLed ------------- */
void showWifiConnectingLed() {
  unsigned long currentTimeledWifi = millis();
  if (millis() - previousTimeWifi >= 500) {
    if (WiFi.status() != WL_CONNECTED) {
      if (_StateWifi == 0) {
        MCP.digitalWrite(statusLedWifi, HIGH); _StateWifi = 1;
      } else if (_StateWifi == 1) {
        MCP.digitalWrite(statusLedWifi, LOW); _StateWifi = 0;
      }
      previousTimeWifi = millis();
    } else MCP.digitalWrite(statusLedWifi, HIGH);
  }
}

/* --------- RS-485 receiveDataHydroponicController ------------- */
void receiveDataHydroponicController() {
  Serial.println("Reading from Hydronic Controller...");
  uint16_t DataHydroponic[7];
  int err = modbus.readInputRegisters(SLAVE_ID2, 0, DataHydroponic, 5);
  Serial.print("error: ");
  Serial.println(err);
  for (int i = 0; i < 5; i++) {
    Serial.println("DataHydroponic " + String(i) + ": " + String(DataHydroponic[i]));
  }
  Serial.println("--------------------------");
  EC = DataHydroponic[0];
  pH = DataHydroponic[1];
  pH = pH / 100;
  waterTemp = DataHydroponic[2];
  waterTemp = waterTemp / 100;
}

/* --------- RS-485 receiveDataSensors ------------- */
void receiveDataSensors() {
  int err = modbus.readInputRegisters(83, 9, inputRegisters, 6); /* read input sensor adress 0 - 20 */
  Serial.println(err);
  for (int i = 0; i < 6; i++) {
    Serial.println("inputRegisters " + String(i) + ": " + String(inputRegisters[i]));
  }
  //  modbus.readInputRegisters(101, 9, inputRegisters, 5); /* read input sensor adress 0 - 20 */
  //  for (int i = 0; i < 5; i++) {
  //    Serial.println("inputRegisters " + String(i) + ": " + String(inputRegisters[i]));
  //  }
}

void controlCoil() {
  coils[0] = true;
  coils[1] = false;
  modbus.writeMultipleCoils(85, 0, coils, 2);
  delay(3000);
  coils[0] = false;
  coils[1] = true;
  modbus.writeMultipleCoils(85, 0, coils, 2);
  delay(3000);
}

/* --------------------------------------------------------------------- */
/* ----------------------- Formula State ------------------------------- */
DateTime parseISO8601(String isoDate) {
  int year    = isoDate.substring(0, 4).toInt();
  int month   = isoDate.substring(5, 7).toInt();
  int day     = isoDate.substring(8, 10).toInt();
  int hour    = isoDate.substring(11, 13).toInt();
  int minute  = isoDate.substring(14, 16).toInt();
  int second  = isoDate.substring(17, 19).toInt();
  return DateTime(year, month, day, hour, minute, second);
}

void updateRTCFromStartDate() {
  if (!rtc.begin()) {
    Serial.println("RTC Not Found!");
    return;
  }
  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running, updating from startDate...");
    DateTime startDateTime = parseISO8601(startDate);
    rtc.adjust(startDateTime);
    Serial.println("RTC Updated Successfully!");
  } else {
    Serial.println("RTC is already running.");
  }
}

void updateRTCFromInternet() {
  timeClient.begin();
  Serial.println("Getting time from NTP...");
  while (!timeClient.update()) {
    timeClient.forceUpdate();
  }
  int year    = timeClient.getEpochTime() / 31556926 + 1970;  // แปลง UNIX Time เป็นปี
  int month   = (timeClient.getEpochTime() % 31556926) / 2629743 + 1;
  int day     = ((timeClient.getEpochTime() % 31556926) % 2629743) / 86400 + 1;
  int hour    = timeClient.getHours();
  int minute  = timeClient.getMinutes();
  int second  = timeClient.getSeconds();

  DateTime newRTC(year, month, day, hour, minute, second);
  rtc.adjust(newRTC);  // ตั้งค่า RTC
  Serial.println("RTC Updated from NTP!");
  Serial.print("Current Time: ");
  Serial.println(rtc.now().timestamp());
}

int calculateCurrentDay() {
  if (!rtc.begin()) {
    Serial.println("RTC Not Found!");
    return -1;
  }

  DateTime now = rtc.now();  // อ่านเวลาปัจจุบันจาก RTC
  DateTime periodStart  = parseISO8601(startDate);
  DateTime periodEnd    = parseISO8601(endDate);

  // คำนวณจำนวนวันทั้งหมดใน period
  totalDays = (periodEnd.unixtime() - periodStart.unixtime()) / 86400 + 1;

  // คำนวณวันที่ปัจจุบันว่าผ่านไปกี่วันแล้วจาก startDate
  int dayDifference = (now.unixtime() - periodStart.unixtime()) / 86400;
  int currentDay    = dayDifference + 1; // วันที่เริ่มจาก 1

  // แสดงข้อมูลที่อ่านได้
  Serial.println("[Current Day Calculation]");
  //  Serial.print("Current RTC Time: "); Serial.println(now.timestamp());
  //  Serial.print("Period Start: "); Serial.println(periodStart.timestamp());
  //  Serial.print("Period End: "); Serial.println(periodEnd.timestamp());
  //  Serial.print("Total Days: "); Serial.println(totalDays);
  //  Serial.print("Days Passed: "); Serial.println(dayDifference);
  Serial.print("Current Day: "); Serial.println(currentDay);

  // เช็คว่าเกิน period หรือยัง
  if (currentDay > totalDays) {
    isFinished = true;
    Serial.println("Process Finished! No more states.");
    return -1;
  }
  return currentDay;
}

// ฟังก์ชันแปลง repeatOn เป็น bitmask
uint8_t calculateRepeatOnBitmask(JsonObject repeatOn) {
  uint8_t bitmask = 0;
  if (repeatOn["monday"])    bitmask |= (1 << 0);
  if (repeatOn["tuesday"])   bitmask |= (1 << 1);
  if (repeatOn["wednesday"]) bitmask |= (1 << 2);
  if (repeatOn["thursday"])  bitmask |= (1 << 3);
  if (repeatOn["friday"])    bitmask |= (1 << 4);
  if (repeatOn["saturday"])  bitmask |= (1 << 5);
  if (repeatOn["sunday"])    bitmask |= (1 << 6);
  return bitmask;
}

void readRawJsonFromSD() {
  File file = SD.open("/data.json");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  Serial.println("Raw JSON Data from SD Card:");
  while (file.available()) {
    Serial.write(file.read());  // ปริ้น JSON ทั้งหมดออกมาดู
  }
  file.close();
}

// อ่าน period (startDate, endDate)
void readPeriodData(JsonObject period) {
  startDate = period["startDate"].as<String>();
  endDate   = period["endDate"].as<String>();
  Serial.println("Period Data Read Successfully:");
  Serial.print("  Start Date: ");
  Serial.println(startDate);
  Serial.print("  End Date: ");
  Serial.println(endDate);
}

// อ่าน schedules
void readSchedulesData(JsonObject stage, FormulaState &state) {
  if (stage.containsKey("schedules")) {
    JsonArray schedules = stage["schedules"];
    state.scheduleCount = 0;
    for (JsonObject schedule : schedules) {
      if (state.scheduleCount >= MAX_SCHEDULES) break;
      Schedule &s = state.schedules[state.scheduleCount];
      s.title = schedule["title"].as<String>();

      // แปลง executionTime เป็น startTime & endTime (นาที)
      String executionTime = schedule["executionTime"].as<String>();
      int dashPos = executionTime.indexOf('-');
      if (dashPos > 0) {
        s.startTime = executionTime.substring(0, 2).toInt() * 60 + executionTime.substring(3, 5).toInt();
        s.endTime   = executionTime.substring(6, 8).toInt() * 60 + executionTime.substring(9, 11).toInt();
      } else {
        s.startTime = executionTime.substring(0, 2).toInt() * 60 + executionTime.substring(3, 5).toInt();
        s.endTime   = s.startTime;
      }

      // แปลง repeatEvery เป็นตัวเลข (1 = weekly, 2 = biweekly, 0 = none)
      String repeatStr = schedule["repeatEvery"].as<String>();
      s.repeatMode     = (repeatStr == "weekly") ? 1 : (repeatStr == "biweekly") ? 2 : 0;

      // แปลง repeatOn เป็น bitmask
      if (schedule.containsKey("repeatOn")) {
        JsonObject repeatOn = schedule["repeatOn"];
        uint8_t bitmask = 0;
        if (repeatOn["monday"]) bitmask |= (1 << 0);
        if (repeatOn["tuesday"]) bitmask |= (1 << 1);
        if (repeatOn["wednesday"]) bitmask |= (1 << 2);
        if (repeatOn["thursday"]) bitmask |= (1 << 3);
        if (repeatOn["friday"]) bitmask |= (1 << 4);
        if (repeatOn["saturday"]) bitmask |= (1 << 5);
        if (repeatOn["sunday"]) bitmask |= (1 << 6);
        s.repeatOnBitmask = bitmask;
      }
      state.scheduleCount++;
    }
  }
}

// อ่าน values
void readValuesData(JsonObject stage, FormulaState &state) {
  if (stage.containsKey("values")) {
    JsonArray values = stage["values"];
    state.valueCount = 0;
    for (JsonObject value : values) {
      if (state.valueCount >= MAX_VALUES) break;
      Value &v = state.values[state.valueCount];
      v.id = value["id"].as<String>();
      v.targetStart = value["targetStart"].as<float>();
      v.targetEnd = value.containsKey("targetEnd") ? value["targetEnd"].as<float>() : -1;
      state.valueCount++;
    }
  }
}

// อ่าน formula (เรียกใช้ readSchedulesData() & readValuesData())
void readFormulaData(JsonArray formulaArray) {
  formulaCount = 0;
  Serial.println("Found 'formula' in JSON:");
  for (JsonObject stage : formulaArray) {
    if (formulaCount >= MAX_STATES) break;
    FormulaState &state = formulaStates[formulaCount];
    state.state = stage["state"].as<String>();
    state.startDay = stage["startDay"].as<int>();
    state.endDay = stage["endDay"].as<int>();
    // อ่าน schedules
    readSchedulesData(stage, state);
    // อ่าน values
    readValuesData(stage, state);
    formulaCount++;
  }
}

// ฟังก์ชันหลัก อ่านข้อมูลจาก SD Card และเรียกฟังก์ชันย่อย
void readPeriodAndFormula() {
  if (!SD.begin(5)) {
    Serial.println("SD Card initialization failed!"); return;
  }
  Serial.println("SD Card initialized.");
  File file = SD.open("/data.json");
  if (!file) {
    Serial.println("Failed to open file for reading"); return;
  }
  StaticJsonDocument<4096> recipe;
  DeserializationError error = deserializeJson(recipe, file);
  file.close();
  if (error) {
    Serial.print("Failed to parse JSON: ");
    Serial.println(error.c_str());
    return;
  }
  // อ่าน `period`
  if (recipe.containsKey("period")) {
    readPeriodData(recipe["period"]);
  }
  // อ่าน `formula`
  if (recipe.containsKey("formula")) {
    readFormulaData(recipe["formula"]);
  }
  SD.end();
  Serial.print("ขนาด JSON ที่ใช้จริง: ");
  Serial.println(measureJson(recipe));
}

void printFormulaData() {
  Serial.println("[Formula Data from JSON]");
  Serial.println("--------------------------------------------------");
  for (int i = 0; i < formulaCount; i++) {
    Serial.print("State: ");
    Serial.println(formulaStates[i].state);
    Serial.print("Start Day: ");
    Serial.print(formulaStates[i].startDay);
    Serial.print(" | End Day: ");
    Serial.println(formulaStates[i].endDay);
    // แสดง Schedules
    if (formulaStates[i].scheduleCount > 0) {
      Serial.println("[Schedules]");
      for (int j = 0; j < formulaStates[i].scheduleCount; j++) {
        Schedule &s = formulaStates[i].schedules[j];
        Serial.print("  - ");
        Serial.print(s.title);
        Serial.print(" | Time: ");
        Serial.print(s.startTime);
        Serial.print(" - ");
        Serial.println(s.endTime);
        // แสดง Repeat Mode
        Serial.print("Repeat: ");
        if (s.repeatMode == 1) {
          Serial.println("Weekly");
        } else if (s.repeatMode == 2) {
          Serial.println("Biweekly");
        } else {
          Serial.println("None");
        }
        // แสดง Repeat On (Bitmask)
        Serial.print("Repeat On: ");
        Serial.println(s.repeatOnBitmask, BIN);  // แสดงเป็นเลขฐาน 2
      }
    }
    // แสดง Values
    if (formulaStates[i].valueCount > 0) {
      Serial.println("[Values]");
      for (int j = 0; j < formulaStates[i].valueCount; j++) {
        Value &v = formulaStates[i].values[j];
        Serial.print("  - ID: ");
        Serial.print(v.id);
        Serial.print(" | Start: ");
        Serial.print(v.targetStart, 2);
        Serial.print(" | End: ");
        if (v.targetEnd != -1) {
          Serial.println(v.targetEnd, 2);
        } else {
          Serial.println("N/A");
        }
      }
    }
    Serial.println("--------------------------------------------------");
  }
}

void sendCurrentStateDataToSlave(int currentDay) {
  if (currentDay < 0) {
    Serial.println("Process Finished! No more states."); return;
  }
  int currentStateIndex = -1;
  for (int i = 0; i < formulaCount; i++) {
    if (currentDay >= formulaStates[i].startDay && currentDay <= formulaStates[i].endDay) {
      currentStateIndex = i;
      break;
    }
  }
  if (currentStateIndex == -1) {
    Serial.println("No active state found for today."); return;
  }
  // ถ้า State เปลี่ยน ให้ส่งข้อมูลใหม่
  if (lastSentStateIndex != currentStateIndex) {
    Serial.println("State Changed! Sending new data...");
    //sendScheduleDataToSlave(currentStateIndex);
    sendValueDataToSlave(currentStateIndex);
    lastSentStateIndex = currentStateIndex;
  } else {
    Serial.println("No State Change - Skipping Transmission");
    //    Serial.println("Sending only schedule & value data (State Unchanged)...");
    //    sendScheduleDataToSlave(currentStateIndex);
    sendValueDataToSlave(currentStateIndex);
  }
}

uint16_t getValueID(String id) {
  if (id == "ec") return 1;
  if (id == "acid") return 2;
  if (id == "airTemp") return 3;
  return 0;
}

void sendValueDataToSlave(int currentStateIndex) {
  if (formulaStates[currentStateIndex].valueCount == 0) return;
  Serial.println("Sending Value Data to Slave ID2...");
  uint16_t valueData[MAX_VALUES * 3];  // ข้อมูลที่ต้องส่ง
  int valueCount = formulaStates[currentStateIndex].valueCount;
  for (int j = 0; j < 2; j++) { // valueCount
    Value &v = formulaStates[currentStateIndex].values[j];
    //valueData[j * 3] = getValueID(v.id);
    valueData[j] = v.targetStart;  //คูณ 100 เพื่อให้เป็น int
    //valueData[j * 3 + 1] = (v.targetEnd != -1) ? v.targetEnd * 100 : 0;
    //Serial.println("         ID: " + String(valueData[j * 3]));
    Serial.println("targetStart: " + String(valueData[j * 3]));
    //Serial.println("  targetEnd: " + String(valueData[j * 3 + 2]));
  }
  /*debug*/
  valueData[2] = 10;
  valueData[3] = 10;
  valueData[4] = 10;
  valueData[5] = 10;
  valueData[6] = 1;
  valueData[7] = 1;
  int er = modbus.writeMultipleHoldingRegisters(SLAVE_ID2, 0, valueData, 8); // valueCount * 3
  Serial.print("error: ");
  Serial.println(er);
  delay(1000);
}

void sendScheduleDataToSlave(int currentStateIndex) {
  if (formulaStates[currentStateIndex].scheduleCount == 0) return;
  Serial.println("Sending Schedule Data to Slave ID1...");
  uint16_t scheduleData[MAX_SCHEDULES * 3];  // ข้อมูลที่ต้องส่ง
  int scheduleCount = formulaStates[currentStateIndex].scheduleCount;
  for (int j = 0; j < scheduleCount; j++) {
    Schedule &s = formulaStates[currentStateIndex].schedules[j];
    scheduleData[j * 3] = s.startTime;
    scheduleData[j * 3 + 1] = s.endTime;
    scheduleData[j * 3 + 2] = s.repeatOnBitmask;
  }
  if (modbus.writeMultipleHoldingRegisters(SLAVE_ID1, 100, scheduleData, scheduleCount * 3)) {
    Serial.println("Schedule Data Sent Successfully!");
  } else {
    Serial.println("Failed to Send Schedule Data");
  }
  delay(50);
}

void setup() {
  delay(500);
  Wire.begin();
  MCP.begin();
  MCP.pinMode8(0x00);
  if (!rtc.begin());
  EEPROM.begin(512);
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  modbus.begin(9600);
  pinMode(SW[0], INPUT);
  pinMode(SW[1], INPUT);
  pinMode(SW[2], INPUT);
  pinMode(SW[3], INPUT);
  pinMode(relay_pin[0], OUTPUT);
  pinMode(relay_pin[1], OUTPUT);
  pinMode(relay_pin[2], OUTPUT);
  pinMode(relay_pin[3], OUTPUT);
  pinMode(LED_BUILTIN_OTA, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(relay_pin[0], LOW);
  digitalWrite(relay_pin[1], LOW);
  digitalWrite(relay_pin[2], LOW);
  digitalWrite(relay_pin[3], LOW);

  EepromStream eeprom(0, 512);
  deserializeJson(doc, eeprom);
  if (!doc.isNull()) {
    MqttClientId  = doc["mqttClientId"].as<String>();
    MqttSecret    = doc["mqttSecret"].as<String>();
    MqttToken     = doc["mqttToken"].as<String>();
    WifiPassword  = doc["wifiPassword"].as<String>();
    WifiSSID      = doc["wifiSSID"].as<String>();
    Serial.println("mqttClientId : " + MqttClientId);
    Serial.println("wifiPassword : " + WifiPassword);
    Serial.println("wifiSSID     : " + WifiSSID);
  }
  for (int pin = 0; pin < 8; pin++) MCP.digitalWrite(pin, HIGH);  delay(500); /* alternating HIGH/LOW */
  for (int pin = 0; pin < 8; pin++) MCP.digitalWrite(pin, LOW);   delay(500);
  xTaskCreatePinnedToCore(TaskWifiStatus, "WifiStatus", 4096, NULL, 1, &WifiStatus, 0);
  readRawJsonFromSD();
  readPeriodAndFormula();
  printFormulaData();
  updateRTCFromStartDate();
}

void loop() {
  int currentDay = 1; /* test */
  //int currentDay = calculateCurrentDay();
  if (SW_ADC() == 1 && millis() - lastButtonPress > 500) { /* SW1 */
    lastButtonPress = millis();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelete(WifiStatus);
    delay(500);
    if (!_bleActive) {
      Serial.println("Pairing device with BLE...");
      _modeStatus = pairingBleMode;
      _bleActive = true;
      pairDeviceWithBLE();
    }
  }
  if (_modeStatus == pairingBleMode) {
    static unsigned long lastMelodyTime = 0;
    toggleLedBlinkForPairing();
    checkHeapMemory();
    if (millis() - lastMelodyTime > 3000) {
      playPairingMelody();
      lastMelodyTime = millis();
    }
    if (SW_ADC() == 1 && millis() - lastButtonPress > 500) {
      lastButtonPress = millis();
      Serial.println("Cancelled by user");
      //      BLEDevice::deinit();
      //      _bleActive = false;
      //      _modeStatus = connectedWifiMode;
      //      MCP.digitalWrite(statusLedAddDevice, LOW);
      playPairingCancelTone();
      esp32Restart();
    }
    if (isPairedSuccessfully()) {
      Serial.println("Pairing completed");
      playPairingSuccessTone();
      esp32Restart();
    }
    delay(10);
  }
  if (_modeStatus != pairingBleMode) {
    if (client.connected()) {
      if (millis() - previousTimeUpdateSensor >= eventIntervalUpdateSensor) {
        updateSensorValue();
        receiveDataHydroponicController();
        previousTimeUpdateSensor = millis();
      }
      if (millis() - previousTimeActivity >= eventIntervalActivity) {
        updateActivitie();
        Serial.println("updateActivitie Processing...");
        delay(100);
        //TestUpdateActivitieFertilizer()
        previousTimeActivity = millis();
      }
    }
    if (SW_ADC() == 2) {
      currentDay++;
      sendCurrentStateDataToSlave(currentDay);
      Serial.println("currentDay: " + String(currentDay));
    }
    if (client.connected()) client.loop();
    if (otaRequired && WiFi.status() == WL_CONNECTED) {
      performOTA();
      otaRequired = false;
    }
    //checkHeapMemory();
    showWifiConnectingLed();
    delay(100);
  }
}

/* --------BLE Task--------- */
bool isPairedSuccessfully() {
  return bleConnectedFlag == true;
}

void pairDeviceWithBLE() {
  xTaskCreatePinnedToCore(
    TaskBleDevice, "BLE_Task", 3076, NULL, 1, &bleTaskHandle, 1
  );
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
      deviceConnected = true;
      Serial.print("Client connected: ");
      Serial.println(connInfo.getAddress().toString().c_str());
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
      deviceConnected = false;
      Serial.print("Client disconnected: ");
      Serial.println(connInfo.getAddress().toString().c_str());
      NimBLEDevice::startAdvertising();
    }
};

class MyCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
      std::string value = pCharacteristic->getValue();
      if (!value.empty()) {
        String receivedStr = String(value.c_str());
        BleTaskParam* param = new BleTaskParam{pCharacteristic, receivedStr};
        xTaskCreatePinnedToCore(bleWifiTask, "BLE_WiFi_Task", 4096, (void*)param, 1, &bleWifiTaskHandle, 1);
      }
    }
};

void TaskBleDevice(void *parameter) {
  Serial.println("Initializing BLE...");
  restartBLE();
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
                    );
  pCharacteristic->setValue("Ready");
  pCharacteristic->setCallbacks(new MyCallbacks());
  NimBLEDescriptor* pCCCD = pCharacteristic->createDescriptor(
                              "2902",
                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
                            );
  pService->start();
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setAppearance(0x00);
  NimBLEAdvertisementData advData;
  advData.setName("SmartFarmLowCabon-101");
  advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();
  Serial.println("BLE Advertising Started!");
  vTaskDelete(NULL);
}

void restartBLE() {
  if (bleInitialized) {
    NimBLEDevice::deinit(true);
    delay(300);
    bleInitialized = false;
  }
  NimBLEDevice::init("SmartFarmLowCabon-101");
  NimBLEDevice::setMTU(256);
  bleInitialized = true;
}

void bleWifiTask(void *parameter) {
  if (parameter == NULL) {
    Serial.println("Invalid Parameter Passed to Task!");
    vTaskDelete(NULL);
  }
  BleTaskParam* taskParam = (BleTaskParam*)parameter;
  BLECharacteristic* pCharacteristic = taskParam->characteristic;
  String receivedData = taskParam->Bledata;
  delete taskParam;
  Serial.println("BLE Task Started!");
  Serial.println("Received Data: " + receivedData);
  //BLEDevice::deinit();
  //delay(500);
  DeserializationError error = deserializeJson(doc, receivedData);
  if (!error) {
    MqttClientId  = doc["mqttClientId"].as<String>();
    MqttSecret    = doc["mqttSecret"].as<String>();
    MqttToken     = doc["mqttToken"].as<String>();
    WifiPassword  = doc["wifiPassword"].as<String>();
    WifiSSID      = doc["wifiSSID"].as<String>();
    EepromStream eeprom(0, 512);
    serializeJson(doc, eeprom);
    eeprom.flush();
    Serial.println("EEPROM Updated!");

    Serial.println("mqttClientId : " + MqttClientId);
    Serial.println("MqttSecret   : " + MqttSecret);
    Serial.println("MqttToken    : " + MqttToken);
    Serial.println("wifiPassword : " + WifiPassword);
    Serial.println("wifiSSID     : " + WifiSSID);
  } else {
    Serial.println("Failed to parse JSON from BLE!");
  }
  bool _wifiConnected = false;
  if (WiFi.status() != WL_CONNECTED) {
    _wifiConnected = connectToWiFi();
  } else {
    Serial.println("WiFi Already Connected!");
    _wifiConnected = true;
  }
  bool mqttConnected = false;
  if (_wifiConnected && !client.connected()) {
    mqttConnected = connectToMQTT();
  } else if (client.connected()) {
    Serial.println("MQTT Already Connected!");
    mqttConnected = true;
  }
  //WiFi.disconnect(true);
  //WiFi.mode(WIFI_OFF);
  //delay(500);
  //restartBLE();
  //delay(500);
  if (pCharacteristic != NULL) {
    String statusMessage = mqttConnected ? "{\"status\": 200}" : "{\"status\": 500}";
    pCharacteristic->setValue(statusMessage);
    pCharacteristic->notify();
    Serial.println("Sent BLE Status!" + String(statusMessage));
    delay(2000);
  }
  //BLEDevice::deinit();
  delay(500);
  bleConnectedFlag = true;
  vTaskDelete(NULL);
}

/* --------Wifi Task--------- */
void TaskWifiStatus(void *parameter) {
  for (;;) {
    if (_modeStatus != connectedWifiMode) {
      Serial.println("TaskWifi: Mode changed, deleting task.");
      WifiStatus = NULL;
      vTaskDelete(NULL);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi Disconnected. Reconnecting...");
      WiFi.disconnect();
      WiFi.mode(WIFI_STA);
      WiFi.begin(WifiSSID.c_str(), WifiPassword.c_str());
      Serial.println("WifiSSID = " + String(WifiSSID.c_str()) + " WifiPassword = " + String(WifiPassword.c_str()) + " Connecting... ");
      unsigned long startAttemptTime = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        Serial.print(".");
        delay(500);
        if (_modeStatus != connectedWifiMode) {
          Serial.println("TaskWifi Stopped (Mode Changed)");
          WifiStatus = NULL;
          vTaskDelete(NULL);
        }
      }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi Connected! in task");
        connectWifiStatus = wifiConnected;
      } else {
        Serial.println("WiFi Connection Failed!");
        connectWifiStatus = cannotConnect;
      }
    }
    if (WiFi.status() == WL_CONNECTED && !client.connected()) {
      Serial.println("Connecting to MQTT...");
      client.setServer("mqtt.nexpie.io", 1883);
      client.setCallback(MqttCallback);
      timeClient.begin();
      if (client.connect(MqttClientId.c_str(), MqttToken.c_str(), MqttSecret.c_str())) {
        Serial.println("MQTT Connected!, task WiFi");
        connectWifiStatus = serverConnected;
        client.setBufferSize(4096); // 1024 * 5
        client.subscribe("@msg/#");
        client.subscribe("@private/#");
      } else {
        Serial.println("MQTT Connection Failed!");
      }
      updateRTCFromInternet();
    }
    if (ESP.getFreeHeap() < 10000) {
      Serial.println("Low Heap, Close WiFi & MQTT...");
      WiFi.disconnect();
      WiFi.mode(WIFI_OFF);
      client.disconnect();
    }
    delay(5000);
  }
  Serial.println("TaskWifi Stopped (Mode Changed)");
  WifiStatus = NULL;
  vTaskDelete(NULL);
}

/* --------connectToWiFi, When BLE pairing mode--------- */
bool connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Already Connected.");
    return true;
  }
  Serial.println("Connecting to WiFi...");
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  delay(5000);
  WiFi.begin(WifiSSID.c_str(), WifiPassword.c_str());
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected!");
    return true;
  } else {
    Serial.println("WiFi Connection Failed!");
    return false;
  }
}

bool connectToMQTT() {
  if (client.connected()) {
    Serial.println("MQTT Already Connected.");
    return true;
  }
  Serial.println("Connecting to MQTT...");
  client.setServer("mqtt.nexpie.io", 1883);
  client.setCallback(MqttCallback);
  if (client.connect(MqttClientId.c_str(), MqttToken.c_str(), MqttSecret.c_str())) {
    Serial.println("MQTT Connected!");
    return true;
  } else {
    Serial.println("MQTT Connection Failed!");
    return false;
  }
}

/* --------BUZZER Noti--------- */
void playPairingMelody() {
  tone(BUZZER_PIN, 262, 200); delay(250);
  tone(BUZZER_PIN, 330, 200); delay(250);
  tone(BUZZER_PIN, 392, 200); delay(250);
  tone(BUZZER_PIN, 523, 300); delay(350);
  noTone(BUZZER_PIN);
}

void playPairingCancelTone() {
  for (int i = 0; i < 2; i++) {
    tone(BUZZER_PIN, 196, 100); delay(150);
    noTone(BUZZER_PIN);
    delay(100);
  }
}

void playPairingSuccessTone() {
  tone(BUZZER_PIN, 523, 150); delay(200);
  tone(BUZZER_PIN, 784, 400); delay(450);
  noTone(BUZZER_PIN);
}

/* --------checkHeapMemory--------- */
void checkHeapMemory() {
  Serial.print("Free Heap Memory: ");
  Serial.println(ESP.getFreeHeap());
  delay(100);
}

void esp32Restart() {
  delay(1000);
  ESP.restart();
}

/* --------OTA Update--------- */
void performOTA() {
  if (firmwareUrl.length() == 0) {
    Serial.println("Invalid firmware URL");
    return;
  }
  Serial.println("Starting OTA Update...");
  Serial.println("Downloading from: " + firmwareUrl);
  httpUpdate.setLedPin(LED_BUILTIN_OTA, LOW);
  httpUpdate.onStart([]() {
    Serial.println("OTA Update started");
  });
  httpUpdate.onEnd([]() {
    Serial.println("OTA Update finished");
  });
  httpUpdate.onProgress([](int progress, int total) {
    int percentage = (progress / (total / 100));
    Serial.printf("OTA Progress: %d%%\n", percentage);
  });
  httpUpdate.onError([](int error) {
    Serial.printf("OTA Error: %d\n", error);
  });
  t_httpUpdate_return ret = httpUpdate.update(espClient, firmwareUrl);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP Update failed: Error (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP Update: No updates available");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("HTTP Update successful! Restarting...");
      break;
  }
}
