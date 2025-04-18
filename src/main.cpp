#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Preferences.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <map>
#include <string>
#include <WiFiUdp.h> // Include for ArduinoOTA
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


// Function Prototypes
void handleRoot();
void handleResults();
void handleSetIP();
void resetTimer();
void updateDisplay();
void displayElapsedTime(float timeSeconds);
void countdownBeforeStart();
void saveResult(int session, float result, int deviceId);
void loadPreviousResults();
void printPreviousResults();
void resetPreviousResults();
void sendResultToServer(unsigned long scoreMillis, int deviceId);
String getSavedIP();
void saveIP(String ipAddress);
void displayOtaProgress(unsigned int progress, unsigned int total);
void displayOtaMessage(const char *msg); // Function to display OTA status messages
void handleOtaError(ota_error_t error);
void sendDataToGoogleSheets(unsigned long scoreMillis, int deviceId); // New function to send data to Google Sheets

// Define hardware type and number of devices
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 8
#define CS_PIN 5

// Create an instance of MD_Parola
MD_Parola myDisplay = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// Define IR sensor pin
#define IR_SENSOR 22

// Define reset button pin
#define RESET_BUTTON 36

// WiFi credentials (replace with your actual credentials)
const char *ssid = "CLPHS_CEC_IOT";
const char *password = "@ceciot2024";

WebServer server(80);

// Timing variables
unsigned long startTime = 0;
unsigned long delayStart = 0;
bool isCounting = false;
bool inDelay = false;
bool hasFinished = false;
bool previousIrState = HIGH;
bool previousButtonState = HIGH;
float lastDisplayedTime = -1.0;
float currentResult = 0.0;
int wifiResetTimer = 0;
unsigned long otaStartTime = 0; // Add this
volatile unsigned long lastScoreMillis = 0;
volatile int lastDeviceId = 0;

// Persistent storage
Preferences preferences;
int sessionNumber = 0;
int deviceId = 0; // Declare deviceId here

// EEPROM addresses
#define EEPROM_SIZE 512
#define RESULTS_START_ADDRESS 0
#define NUM_STORED_RESULTS 5
#define RESULT_SIZE sizeof(float)
#define IP_ADDRESS_START 256 // Start address for storing IP
#define MAX_IP_LENGTH 16    // Maximum length of an IPv4 address string

// Google Sheets API details (Replace with your actual details)
const char *GOOGLE_SCRIPT_ID = "AKfycbwpUa0yymVmrIUaRdIw2Nwa72_Hv7qX6JS6dxWzw7fBIwgTJhB0AHMD-qPR2N94Ak-e"; // Replace with your Google Apps Script ID
const char *SHEET_NAME = "Time";       // Replace with your sheet name
const char *GOOGLE_SHEETS_URL_BASE = "https://script.google.com/macros/s/";

// Button hold reset variables
unsigned long buttonPressStartTime = 0;
bool buttonHeld = false;
bool longPressDetected = false;
bool isResetting = false;
bool isOtaUpdating = false; // Flag to indicate OTA update in progress


QueueHandle_t uploadQueue;

struct UploadData {
  String deviceId;
  unsigned long scoreMillis;
};

// Function to get the saved IP address from EEPROM
String getSavedIP()
{
  String ip = "";
  for (int i = 0; i < MAX_IP_LENGTH; i++)
  {
    char c = EEPROM.read(IP_ADDRESS_START + i);
    if (c == '\0' || i >= 15)
    {
      break;
    }
    ip += c;
  }
  return ip;
}

// Function to save the IP address to EEPROM
void saveIP(String ipAddress)
{
  for (int i = 0; i < MAX_IP_LENGTH; i++)
  {
    if (i < ipAddress.length())
    {
      EEPROM.write(IP_ADDRESS_START + i, ipAddress[i]);
    }
    else
    {
      EEPROM.write(IP_ADDRESS_START + i, '\0');
    }
  }
  EEPROM.commit();
  Serial.print("Saved IP address: ");
  Serial.println(ipAddress);
}

// Function to handle the root web page
void handleRoot()
{
  String ipAddress = getSavedIP();
  String macAddress = WiFi.macAddress(); // Get the MAC address
  String html = "<!DOCTYPE html><html><head><title>Timer Settings</title></head><body>";
  html += "<h1>Timer Settings</h1>";
  html += "<p>Device ID: <strong>" + String(deviceId) + "</strong></p>"; // Display Device ID
  html += "<p>MAC Address: <strong>" + macAddress + "</strong></p>"; // Display MAC Address
  html += "<p>Current POST IP Address: <strong>" + (ipAddress.isEmpty() ? "Not Set" : ipAddress) + "</strong></p>";
  html += "<h2>Set POST IP Address</h2>";
  html += "<form action='/setip' method='post'>";
  html += "<label for='ip'>IP Address:</label>";
  html += "<input type='text' id='ip' name='ip' maxlength='" + String(MAX_IP_LENGTH - 1) + "'><br><br>";
  html += "<input type='submit' value='Save IP Address'>";
  html += "</form>";
  html += "<p><a href='/results'>View Results</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Function to handle setting the IP address
void handleSetIP()
{
  if (server.hasArg("ip"))
  {
    String newIP = server.arg("ip");
    saveIP(newIP);
    server.send(200, "text/plain", "IP address saved: " + newIP);
  }
  else
  {
    server.send(400, "text/plain", "Error: IP address not provided.");
  }
}

// Function to handle the /results web page
String resultsPage;
void handleResults()
{
  resultsPage = "<h1>Timer Results</h1><pre>";
  int startSession = sessionNumber - NUM_STORED_RESULTS + 1;
  if (startSession < 1)
  {
    startSession = 1;
  }

  for (int i = 0; i < NUM_STORED_RESULTS; i++)
  {
    int currentSessionToDisplay = startSession + i;
    int index = (currentSessionToDisplay - 1) % NUM_STORED_RESULTS;
    float result;
    EEPROM.get(RESULTS_START_ADDRESS + index * RESULT_SIZE, result);
    resultsPage += "Session [" + String(currentSessionToDisplay) + "]: " + String(result, 3) + "\n";
  }
  resultsPage += "</pre><p><a href='/'>Back to Settings</a></p>";
  server.send(200, "text/html", resultsPage);
}

void resetTimer()
{
  Serial.println("Reset button pressed. Resetting timer.");
  isCounting = false;
  inDelay = false;
  hasFinished = false;
  lastDisplayedTime = -1.0;
  myDisplay.displayClear();
  myDisplay.print("Resetting..");
  isResetting = true;
  delay(300);
  countdownBeforeStart();
  isResetting = false;
}

void updateDisplay()
{
  if (isCounting)
  {
    unsigned long elapsedTime = millis() - startTime;
    float elapsedSeconds = elapsedTime / 1000.0;

    if (elapsedSeconds != lastDisplayedTime)
    {
      displayElapsedTime(elapsedSeconds);
      lastDisplayedTime = elapsedSeconds;
    }
  }
}

void displayElapsedTime(float timeSeconds)
{
  char buffer[10];
  dtostrf(timeSeconds, 7, 3, buffer);
  buffer[7] = 's';
  buffer[8] = '\0';

  myDisplay.setTextAlignment(PA_LEFT);
  myDisplay.print(buffer);
}

void countdownBeforeStart()
{
  const char *numbers[] = {"3", "2", "1"};

  for (int i = 0; i < 3; i++)
  {
    String text = numbers[i];

    for (int dots = 0; dots <= 3; dots++)
    {
      text += ".";
      myDisplay.setTextAlignment(PA_CENTER);
      myDisplay.print(text.c_str());
      Serial.println(text);
      delay(250);
    }
  }

  myDisplay.setTextAlignment(PA_CENTER);
  myDisplay.print("GO!");
  Serial.println("GO!");
  delay(1000);
  myDisplay.displayClear();
  displayElapsedTime(0.000);
}


void sendDataToGoogleSheetsTask(void *parameter) {
  UploadData data;

  while (true) {
    if (xQueueReceive(uploadQueue, &data, portMAX_DELAY) == pdTRUE) {
      HTTPClient http;
      String googleSheetsUrl = String(GOOGLE_SHEETS_URL_BASE) + GOOGLE_SCRIPT_ID + "/exec";
      String postData = "deviceId=" + data.deviceId + "&scoreMillis=" + String(data.scoreMillis);

      if (WiFi.status() == WL_CONNECTED) {
        http.begin(googleSheetsUrl);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        int httpResponseCode = http.POST(postData);
        Serial.print("Google Sheets HTTP Response Code: ");
        Serial.println(httpResponseCode);

        if (httpResponseCode == 200 || httpResponseCode == 302) {
          String response = http.getString();
          Serial.println("Success: " + response);
        } else {
          Serial.println("Failed: " + http.getString());
        }
        http.end();
      } else {
        Serial.println("WiFi not connected.");
      }
    }
    delay(1); //dont overload cpu after overloading core

  }
}


void saveResult(int session, float result, int deviceId) {
  int address = RESULTS_START_ADDRESS + ((session - 1) % NUM_STORED_RESULTS) * RESULT_SIZE;
  Serial.print("Saving result ");
  Serial.print(result, 3);
  Serial.print(" for session ");
  Serial.print(session);
  Serial.print(" at EEPROM address ");
  Serial.println(address);
  EEPROM.put(address, result);
  EEPROM.commit();

  unsigned long scoreMillis = static_cast<unsigned long>(result * 1000);

  // Prepare data for Google Sheets upload
  lastScoreMillis = scoreMillis;
  lastDeviceId = deviceId;

  // Trigger the Google Sheets upload task
  if (WiFi.status() == WL_CONNECTED) {
    UploadData upload = { String(lastDeviceId), lastScoreMillis };
    xQueueSend(uploadQueue, &upload, portMAX_DELAY);    
    Serial.println("Google Sheets upload task created.");
  } else if (!WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi not connected. Skipping Google Sheets upload.");
  }

  sendResultToServer(scoreMillis, deviceId); // Keep the original server send if you need it
}


void loadPreviousResults()
{
  Serial.println("Loading previous results:");
  for (int i = 0; i < NUM_STORED_RESULTS; i++)
  {
    float result;
    EEPROM.get(RESULTS_START_ADDRESS + i * RESULT_SIZE, result);
    Serial.print("Session [");
    Serial.print(i + 1);
    Serial.print("]: ");
    Serial.println(result, 3);
  }
}

void printPreviousResults()
{
  Serial.println("--- Last 5 Timer Results ---");
  int startSession = sessionNumber - NUM_STORED_RESULTS + 1;
  if (startSession < 1)
  {
    startSession = 1;
  }

  for (int i = 0; i < NUM_STORED_RESULTS; i++)
  {
    int currentSessionToDisplay = startSession + i;
    int index = (currentSessionToDisplay - 1) % NUM_STORED_RESULTS;
    float result;
    EEPROM.get(RESULTS_START_ADDRESS + index * RESULT_SIZE, result);
    Serial.print("Session [");
    Serial.print(currentSessionToDisplay);
    Serial.print("]: ");
    Serial.println(result, 3);
  }
  Serial.println("----------------------------");
}

void resetPreviousResults()
{
  Serial.println("Clearing stored timer results.");
  for (int i = 0; i < NUM_STORED_RESULTS; i++)
  {
    EEPROM.put(RESULTS_START_ADDRESS + i * RESULT_SIZE, 0.0f);
  }
  EEPROM.commit();
  Serial.println("Stored timer results cleared.");
  loadPreviousResults();
}

void sendResultToServer(unsigned long scoreMillis, int deviceId)
{
  Serial.println("Send results to server temporarily disabled for debugging");
  // String serverIP = getSavedIP();
  // if (WiFi.status() == WL_CONNECTED && !serverIP.isEmpty())
  // {
  //   HTTPClient http;
  //   String serverPath = "http://" + serverIP + ":3001/esp/" + String(deviceId) + "/" + String(scoreMillis);

  //   http.begin(serverPath);
  //   http.addHeader("Content-Type", "application/x-www-form-urlencoded"); // Or another appropriate header

  //   int httpResponseCode = http.POST("");

  //   Serial.print("HTTP Response code: ");
  //   Serial.println(httpResponseCode);

  //   if (httpResponseCode > 0)
  //   {
  //     String response = http.getString();
  //     Serial.println("HTTP Response body: " + response);
  //   }
  //   else
  //   {
  //     Serial.print("Error sending POST request to ");
  //     Serial.print(serverPath);
  //     Serial.print(": ");
  //     Serial.println(http.errorToString(httpResponseCode));
  //   }

  //   http.end();
  // }
  // else
  // {
  //   Serial.println("WiFi not connected or POST IP not set. Cannot send POST request.");
  // }
}

void displayOtaMessage(const char *msg)
{
  myDisplay.displayClear();
  myDisplay.setTextAlignment(PA_CENTER);
  myDisplay.print(msg);
}

void displayOtaProgress(unsigned int progress, unsigned int total)
{
  if (total == 0)
  {
    Serial.println("Error: Total OTA size is zero.");
    displayOtaMessage("OTA Error: Size 0!");
    isOtaUpdating = false;
    return;
  }
  isOtaUpdating = true;
  char buffer[20];
  int percentage = (progress * 100) / total;
  sprintf(buffer, "OTA: %d%%", percentage);
  myDisplay.setTextAlignment(PA_CENTER);
  myDisplay.print(buffer);
  Serial.printf("OTA Progress: %u%%\r", percentage);
}

void handleOtaError(ota_error_t error)
{
  isOtaUpdating = false; // Ensure flag is reset on any error
  switch (error)
  {
  case OTA_AUTH_ERROR:
    displayOtaMessage("OTA Failed");
    Serial.println("OTA Begin Failed");
    break;
  case OTA_BEGIN_ERROR:
    displayOtaMessage("OTA Failed");
    Serial.println("OTA Begin Failed");
    break;
  case OTA_CONNECT_ERROR:
    displayOtaMessage("OTA Failed");
    Serial.println("OTA CONNECT FAILED");
    break;
  case OTA_RECEIVE_ERROR:
    displayOtaMessage("OTA Failed");
    Serial.println("OTA Receive Failed");
    break;
  case OTA_END_ERROR:
  Serial.println("OTA END ERROR ");
    displayOtaMessage("OTA Failed");
    break;
  default:
    displayOtaMessage("OTA ??? Error");
    break;
  }
  Serial.printf("OTA Error[%u]\n", error);
  delay(3000);
}

// ==================== MAC Address to Device ID Mapping ====================
// This is where you define the MAC address to Device ID mapping.
// IMPORTANT:
// 1.  Make sure the MAC addresses are in the format "XX:XX:XX:XX:XX:XX" (uppercase).
// 2.  The Device IDs should be between 1 and 15 (inclusive).
// 3.  Add a comma and space after each closing curly brace except the last one.
std::map<std::string, int> macToId = {
    {"8C:4F:00:2D:7E:DC", 1}, // Example 1
    {"AA:BB:CC:DD:EE:FF", 2}, // Example 2
    {"1A:2B:3C:4D:5E:6F", 3}  // Example 3
};

int getDeviceIdFromMacAddress(const String &mac)
{
  // Convert the String to a std::string for map lookup
  std::string macStdString = std::string(mac.c_str());
  // Check if the MAC address is in the map.
  if (macToId.count(macStdString) > 0)
  {
    int id = macToId[macStdString];
    if (id >= 1 && id <= 15)
    {
      return id; // Return the ID if it's within the valid range.
    }
    else
    {
      Serial.print("Error: Invalid Device ID for MAC ");
      Serial.print(mac);
      Serial.print(".  ID must be between 1 and 15.  Setting ID to 0.\n");
      return 0;
    }
  }
  else
  {
    Serial.print("MAC address ");
    Serial.print(mac);
    Serial.println(" not found in ID mapping.  Setting ID to 0.");
    return 0; // Return 0 to indicate not found.
  }
}

// Function to send data to Google Sheets
void sendDataToGoogleSheets(unsigned long scoreMillis, int deviceId) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String googleSheetsUrl = String(GOOGLE_SHEETS_URL_BASE) + GOOGLE_SCRIPT_ID + "/exec";
    String postData = "deviceId=" + String(deviceId) + "&scoreMillis=" + String(scoreMillis);

    http.begin(googleSheetsUrl);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    Serial.print("URL being used: ");  // ***CRITICAL URL CHECK***
    Serial.println(googleSheetsUrl);

    Serial.print("Sending data: deviceId="); // ***CRITICAL DATA CHECK***
    Serial.print(deviceId);
    Serial.print(", scoreMillis=");
    Serial.println(scoreMillis);

    int httpResponseCode = http.POST(postData);

    Serial.print("HTTP Response Code: "); // ***CHECK THE RESPONSE CODE***
    Serial.println(httpResponseCode);

    if (httpResponseCode == 200 || httpResponseCode == 302) {  // Accepting 302 because it works and i dont want to break it again ahhhhhhhh
      String response = http.getString();                      // ts pmo
      Serial.println("Google Sheets response: " + response);
    } else {
      String response = http.getString();  // Get the response, even on error
      Serial.println("Google Sheets response: " + response);
      Serial.print("Error sending data to Google Sheets: ");
      Serial.println(http.errorToString(httpResponseCode));
    }
    http.end();
  } else {
    Serial.println("WiFi not connected.");
  }
}

void setup()
{
  Serial.begin(115200);
  myDisplay.begin();
  myDisplay.setIntensity(1);
  myDisplay.displayClear();

  pinMode(2,OUTPUT);

  preferences.begin("timer_data", false);
  sessionNumber = preferences.getInt("session_count", 0);
  sessionNumber++;
  preferences.putInt("session_count", sessionNumber);
  preferences.end();
  Serial.print("Current Session Number: ");
  Serial.println(sessionNumber);

  EEPROM.begin(EEPROM_SIZE);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    if (wifiResetTimer < 11)
    {
      wifiResetTimer++;
      Serial.print(".");
    }
    else
    {
      Serial.println("FAILED TO CONNECT TO WIFI. RESTARTING");
      ESP.restart();
    }
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Get and assign Device ID based on MAC Address
  String macAddress = WiFi.macAddress();
  deviceId = getDeviceIdFromMacAddress(macAddress);
  Serial.print("Device ID: ");
  Serial.println(deviceId);

  // Initialize ArduinoOTA
  ArduinoOTA
      .setHostname("ambatukms")
      .setPassword("ChooChooHann0000");

  ArduinoOTA.onStart([]() {
    Serial.println("Start updating");
    displayOtaMessage("Start updating...");
    isOtaUpdating = true;
    otaStartTime = millis(); // Record start time
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd updating; Rebooting...");
    displayOtaMessage("Yippee!!!!");
    delay(2000);
    isOtaUpdating = false;
  });
  ArduinoOTA.onProgress(displayOtaProgress);
  ArduinoOTA.onError(handleOtaError); // Use the new error handler
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA initialized");

  // Set up web server routes
  server.on("/", handleRoot);
  server.on("/results", handleResults);
  server.on("/setip", HTTP_POST, handleSetIP);
  server.begin();
  Serial.println("Web server started");
  Serial.print("Saved POST IP: ");
  Serial.println(getSavedIP());

  uploadQueue = xQueueCreate(5, sizeof(UploadData));
  xTaskCreate(&sendDataToGoogleSheetsTask, "UploadTask", 10000, NULL, 5, NULL);


  loadPreviousResults();
  countdownBeforeStart();
  pinMode(IR_SENSOR, INPUT_PULLUP);
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  displayElapsedTime(0.000);
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();

  if (isOtaUpdating) {
    unsigned long otaElapsedTime = millis() - otaStartTime;
    if (otaElapsedTime > 60000) { // Check for timeout (e.g., 60 seconds)
      Serial.println("OTA update timed out. Restarting.");
      displayOtaMessage("OTA Timeout!");
      delay(2000);
      ESP.restart(); // Reset the ESP32
    }
    delay(10); // Small delay during OTA to allow display updates
    return;     // Don't run normal timer logic during OTA
  }

  int buttonState = digitalRead(RESET_BUTTON);
  unsigned long currentTime = millis();

  if (buttonState == LOW && previousButtonState == HIGH) {
    buttonPressStartTime = currentTime;
    buttonHeld = true;
    longPressDetected = false;
    isResetting = false;
  }

  if (buttonState == LOW && buttonHeld && !longPressDetected && (currentTime - buttonPressStartTime >= 2000)) {
    int remainingSeconds = 5 - ((currentTime - buttonPressStartTime) / 1000);
    if (remainingSeconds >= 0) {
      String resetText = "reset in " + String(remainingSeconds);
      myDisplay.setTextAlignment(PA_CENTER);
      myDisplay.print(resetText.c_str());
    }
  }

  if (buttonState == LOW && buttonHeld && !longPressDetected && (currentTime - buttonPressStartTime >= 5000)) {
    Serial.println("Button held for 5 seconds. Resetting session number and results.");
    sessionNumber = 1;
    preferences.putInt("session_count", sessionNumber);
    preferences.end();
    Serial.print("Session Number Reset to: ");
    Serial.println(sessionNumber);
    resetPreviousResults();
    resetTimer();
    longPressDetected = true;
    isResetting = false;
  }

  if (buttonState == HIGH && previousButtonState == LOW && !longPressDetected) {
    resetTimer();
    sessionNumber++;
    preferences.putInt("session_count", sessionNumber);
    preferences.end();
    Serial.print("Session Number incremented to: ");
    Serial.println(sessionNumber);
  }

  if (buttonState == HIGH) {
    buttonHeld = false;
  }

  previousButtonState = buttonState;

  if (hasFinished)
    return;

  int irState = digitalRead(IR_SENSOR);

  if (!isResetting && isCounting) {
    updateDisplay();
  }

  if (inDelay) {
    if (currentTime - delayStart >= 1000) {
      inDelay = false;
      Serial.println("1000ms delay ended. IR detection active.");
    }
    return;
  }

  if (irState == HIGH && previousIrState == LOW) {
    if (!isCounting) {
      startTime = currentTime;
      isCounting = true;
      inDelay = true;
      delayStart = currentTime;
      Serial.println("Detection! Timer started instantly. Entering 1000ms delay...");
      displayElapsedTime(0.000);
    }
    // The following 'else' block is the one you need to modify
    else {
      unsigned long elapsedTime = currentTime - startTime;
      float elapsedSeconds = elapsedTime / 1000.0;
      currentResult = elapsedSeconds;
      Serial.print("Detection 2! Object detected again. Timer stopped. Total time: ");
      Serial.print(elapsedSeconds, 3);
      Serial.println(" s");
      displayElapsedTime(elapsedSeconds);
      isCounting = false;
      hasFinished = true;
      saveResult(sessionNumber, elapsedSeconds, deviceId); // Pass deviceId here
    }
  }

  previousIrState = irState;
  if (WiFi.status() == WL_CONNECTED){
    digitalWrite(2,HIGH);
  }else{
    digitalWrite(2,LOW);
  }
}
