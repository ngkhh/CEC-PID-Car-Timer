#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Preferences.h>
#include <EEPROM.h> // For persistent storage
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>

// Function Prototypes
void handleRoot();
void handleResults();
void handleSetIP();
void resetTimer();
void updateDisplay();
void displayElapsedTime(float timeSeconds);
void countdownBeforeStart();
void saveResult(int session, float result);
void loadPreviousResults();
void printPreviousResults();
void resetPreviousResults();
void sendResultToServer(unsigned long scoreMillis);
String getSavedIP();
void saveIP(String ipAddress);
void displayOtaProgress(unsigned int progress, unsigned int total);

// Define hardware type and number of devices
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 8
#define CS_PIN 5

// Create an instance of MD_Parola
MD_Parola myDisplay = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// Define IR sensor pin
#define IR_SENSOR 32

// Define reset button pin
#define RESET_BUTTON 36

// WiFi credentials (replace with your actual credentials)
const char* ssid = "CLPHS_CEC_IOT";
const char* password = "@ceciot2024";

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

// Persistent storage
Preferences preferences;
int sessionNumber = 0;

// EEPROM addresses
#define EEPROM_SIZE 512
#define RESULTS_START_ADDRESS 0
#define NUM_STORED_RESULTS 5
#define RESULT_SIZE sizeof(float)
#define IP_ADDRESS_START 256 // Start address for storing IP
#define MAX_IP_LENGTH 16     // Maximum length of an IPv4 address string

// Button hold reset variables
unsigned long buttonPressStartTime = 0;
bool buttonHeld = false;
bool longPressDetected = false;
bool isResetting = false;
bool isOtaUpdating = false; // Flag to indicate OTA update in progress

// Function to get the saved IP address from EEPROM
String getSavedIP() {
    String ip = "";
    for (int i = 0; i < MAX_IP_LENGTH; i++) {
        char c = EEPROM.read(IP_ADDRESS_START + i);
        if (c == '\0' || i >= 15) {
            break;
        }
        ip += c;
    }
    return ip;
}

// Function to save the IP address to EEPROM
void saveIP(String ipAddress) {
    for (int i = 0; i < MAX_IP_LENGTH; i++) {
        if (i < ipAddress.length()) {
            EEPROM.write(IP_ADDRESS_START + i, ipAddress[i]);
        } else {
            EEPROM.write(IP_ADDRESS_START + i, '\0');
        }
    }
    EEPROM.commit();
    Serial.print("Saved IP address: ");
    Serial.println(ipAddress);
}

// Function to handle the root web page
void handleRoot() {
    String ipAddress = getSavedIP();
    String html = "<!DOCTYPE html><html><head><title>Timer Settings</title></head><body>";
    html += "<h1>Timer Settings</h1>";
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
void handleSetIP() {
    if (server.hasArg("ip")) {
        String newIP = server.arg("ip");
        saveIP(newIP);
        server.send(200, "text/plain", "IP address saved: " + newIP);
    } else {
        server.send(400, "text/plain", "Error: IP address not provided.");
    }
}

// Function to handle the /results web page
String resultsPage;
void handleResults() {
    resultsPage = "<h1>Timer Results</h1><pre>";
    int startSession = sessionNumber - NUM_STORED_RESULTS + 1;
    if (startSession < 1) {
        startSession = 1;
    }

    for (int i = 0; i < NUM_STORED_RESULTS; i++) {
        int currentSessionToDisplay = startSession + i;
        int index = (currentSessionToDisplay - 1) % NUM_STORED_RESULTS;
        float result;
        EEPROM.get(RESULTS_START_ADDRESS + index * RESULT_SIZE, result);
        resultsPage += "Session [" + String(currentSessionToDisplay) + "]: " + String(result, 3) + "\n";
    }
    resultsPage += "</pre><p><a href='/'>Back to Settings</a></p>";
    server.send(200, "text/html", resultsPage);
}

void resetTimer() {
    Serial.println("Reset button pressed. Resetting timer.");
    isCounting = false;
    inDelay = false;
    hasFinished = false;
    lastDisplayedTime = -1.0;
    myDisplay.displayClear();
    myDisplay.print("Saving..");
    isResetting = true;
    delay(1000);
    countdownBeforeStart();
    isResetting = false;
}

void updateDisplay() {
    if (isCounting) {
        unsigned long elapsedTime = millis() - startTime;
        float elapsedSeconds = elapsedTime / 1000.0;

        if (elapsedSeconds != lastDisplayedTime) {
            displayElapsedTime(elapsedSeconds);
            lastDisplayedTime = elapsedSeconds;
        }
    }
}

void displayElapsedTime(float timeSeconds) {
    char buffer[10];
    dtostrf(timeSeconds, 7, 3, buffer);
    buffer[7] = 's';
    buffer[8] = '\0';

    myDisplay.setTextAlignment(PA_LEFT);
    myDisplay.print(buffer);
}

void countdownBeforeStart() {
    const char* numbers[] = {"3", "2", "1"};

    for (int i = 0; i < 3; i++) {
        String text = numbers[i];

        for (int dots = 0; dots <= 3; dots++) {
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

void saveResult(int session, float result) {
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
    sendResultToServer(scoreMillis);
}

void loadPreviousResults() {
    Serial.println("Loading previous results:");
    for (int i = 0; i < NUM_STORED_RESULTS; i++) {
        float result;
        EEPROM.get(RESULTS_START_ADDRESS + i * RESULT_SIZE, result);
        Serial.print("Session [");
        Serial.print(i + 1);
        Serial.print("]: ");
        Serial.println(result, 3);
    }
}

void printPreviousResults() {
    Serial.println("--- Last 5 Timer Results ---");
    int startSession = sessionNumber - NUM_STORED_RESULTS + 1;
    if (startSession < 1) {
        startSession = 1;
    }

    for (int i = 0; i < NUM_STORED_RESULTS; i++) {
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

void resetPreviousResults() {
    Serial.println("Clearing stored timer results.");
    for (int i = 0; i < NUM_STORED_RESULTS; i++) {
        EEPROM.put(RESULTS_START_ADDRESS + i * RESULT_SIZE, 0.0f);
    }
    EEPROM.commit();
    Serial.println("Stored timer results cleared.");
    loadPreviousResults();
}

void sendResultToServer(unsigned long scoreMillis) {
    String serverIP = getSavedIP();
    if (WiFi.status() == WL_CONNECTED && !serverIP.isEmpty()) {
        HTTPClient http;
        String serverPath = "http://" + serverIP + ":3001/esp/1/" + String(scoreMillis);

        http.begin(serverPath);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded"); // Or another appropriate header

        int httpResponseCode = http.POST("");

        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println("HTTP Response body: " + response);
        } else {
            Serial.print("Error sending POST request to ");
            Serial.print(serverPath);
            Serial.print(": ");
            Serial.println(http.errorToString(httpResponseCode));
        }

        http.end();
    } else {
        Serial.println("WiFi not connected or POST IP not set. Cannot send POST request.");
    }
}

void displayOtaProgress(unsigned int progress, unsigned int total) {
    isOtaUpdating = true;
    char buffer[20];
    int percentage = (progress * 100) / total;
    sprintf(buffer, "OTA: %d%%", percentage);
    myDisplay.setTextAlignment(PA_CENTER);
    myDisplay.print(buffer);
    Serial.printf("OTA Progress: %u%%\r", percentage);
}

void setup() {
    Serial.begin(115200);
    myDisplay.begin();
    myDisplay.setIntensity(1);
    myDisplay.displayClear();

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
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Initialize ArduinoOTA
    ArduinoOTA
        .setHostname("ambatukms")
        .setPassword("ChooChooHann0000");

    ArduinoOTA.onStart([]() {
        Serial.println("Start updating");
        myDisplay.displayClear();
        myDisplay.setTextAlignment(PA_CENTER);
        myDisplay.print("OTA Starting...");
        isOtaUpdating = true;
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd updating; Rebooting...");
        myDisplay.displayClear();
        myDisplay.setTextAlignment(PA_CENTER);
        myDisplay.print("OTA Done! Rebooting...");
        isOtaUpdating = true; // Keep the flag set until reboot
    });
    ArduinoOTA.onProgress(displayOtaProgress);
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        String errorMsg;
        if (error == OTA_AUTH_ERROR) errorMsg = "Auth Failed";
        else if (error == OTA_BEGIN_ERROR) errorMsg = "Begin Failed";
        else if (error == OTA_CONNECT_ERROR) errorMsg = "Connect Failed";
        else if (error == OTA_RECEIVE_ERROR) errorMsg = "Receive Failed";
        else if (error == OTA_END_ERROR) errorMsg = "End Failed";
        else errorMsg = "Unknown Error";
        Serial.println(errorMsg);
        myDisplay.displayClear();
        myDisplay.setTextAlignment(PA_CENTER);
        myDisplay.print("OTA Error!");
        delay(3000); // Show error for a bit
        isOtaUpdating = false;
    });

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
        delay(10); // Small delay during OTA to allow display updates
        return;     // Don't run normal timer logic during OTA
    }

    int buttonState = digitalRead(RESET_BUTTON);
    unsigned long currentTime = millis();

    if (Serial.available() > 0) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command == "RESULT") {
            printPreviousResults();
        }
    }

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

    if (hasFinished) return;

int irState = digitalRead(IR_SENSOR);

if (!isResetting && isCounting) {
    updateDisplay();
}

if (inDelay) {
    if (currentTime - delayStart >= 300) {
        inDelay = false;
        Serial.println("300ms delay ended. IR detection active.");
    }
    return;
}

if (irState == LOW && previousIrState == HIGH) {
    if (!isCounting) {
        startTime = currentTime;
        isCounting = true;
        inDelay = true;
        delayStart = currentTime;
        Serial.println("Detection! Timer started instantly. Entering 300ms delay...");
        displayElapsedTime(0.000);
    } else {
        Serial.println("Detection 2! Object detected again, waiting for it to move away...");
        while (digitalRead(IR_SENSOR) == LOW) {
            if (!isResetting) {
                updateDisplay();
            }
        }
        unsigned long elapsedTime = currentTime - startTime;
        float elapsedSeconds = elapsedTime / 1000.0;
        currentResult = elapsedSeconds;
        Serial.print("Object left! Total time: ");
        Serial.print(elapsedSeconds, 3);
        Serial.println(" s");
        displayElapsedTime(elapsedSeconds);
        isCounting = false;
        hasFinished = true;
        saveResult(sessionNumber, elapsedSeconds);
    }
}

previousIrState = irState;
}
