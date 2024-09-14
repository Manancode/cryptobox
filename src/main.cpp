#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "SD.h"
#include "FS.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// WiFi credentials
const char* ssid = "Piedpiper";
const char* password = "piedpipermr";

// API details
const char* apiEndpoint = "https://api-sepolia.etherscan.io/api";
const char* apiKey = "Q7YB5BHRA9J4R7KS5T9BHV4W4Q3C74JG6C";

// Currency conversion API (example using exchangerate-api.com)
const char* exchangeRateAPI = "https://v6.exchangerate-api.com/v6/YOUR_API_KEY/latest/USD";

// Merchant details
const char* merchantAddress = "0x7263B2E0D541206724a20f397296Bf43d86005F8";

// Global variables
bool isWifiConnected = false;
unsigned long lastPaymentCheck = 0;
const unsigned long paymentCheckInterval = 5000;  // Check every 5 seconds
String lastProcessedTx = "";
float ethToUsd = 0;
float usdToInr = 0;

Preferences preferences;

// Audio components
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;
AudioFileSourceID3 *id3 = nullptr;

// Button pin
const int BUTTON_PIN = 2;  // For previous transaction

// SD Card CS pin
const int SD_CS = 5;  // Adjust this according to your wiring

void connectToWifi();
void checkForNewTransactions();
void playSound(const String &amount);
void saveLastProcessedTx(const String &txHash);
String getLastProcessedTx();
void playPreviousTransaction();
void playSoundTrack(const String &trackName);
void updateExchangeRates();

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Realtime Crypto Payment Sound Box...");
  
  preferences.begin("crypto-box", false);
  lastProcessedTx = getLastProcessedTx();
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize SD card
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed!");
    return;
  }
  Serial.println("SD card initialized successfully");
  
  // Initialize I2S output
  out = new AudioOutputI2S();
  out->SetPinout(26, 25, 22);  // BCLK, LRC, DIN
  Serial.println("I2S output initialized");
  
  connectToWifi();
  updateExchangeRates();
}

void loop() {
  if (!isWifiConnected) {
    connectToWifi();
    return;
  }
  
  if (millis() - lastPaymentCheck > paymentCheckInterval) {
    checkForNewTransactions();
    updateExchangeRates();
    lastPaymentCheck = millis();
  }
  
  // Check if button is pressed to play previous transaction
  if (digitalRead(BUTTON_PIN) == LOW) {
    playPreviousTransaction();
    delay(300);  // Debounce
  }
  
  if (mp3) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) mp3->stop();
    } else {
      delete mp3;
      mp3 = nullptr;
    }
  }
}

void updateExchangeRates() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    // Fetch ETH to USD rate
    http.begin("https://api.coingecko.com/api/v3/simple/price?ids=ethereum&vs_currencies=usd");
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      ethToUsd = doc["ethereum"]["usd"];
    }
    http.end();
    
    // Fetch USD to INR rate
    http.begin(exchangeRateAPI);
    httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      usdToInr = doc["conversion_rates"]["INR"];
    }
    http.end();
    
    Serial.println("Exchange rates updated - ETH/USD: " + String(ethToUsd) + ", USD/INR: " + String(usdToInr));
  }
}

void checkForNewTransactions() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = String(apiEndpoint) + 
                 "?module=account" +
                 "&action=txlist" +
                 "&address=" + merchantAddress +
                 "&startblock=0" +
                 "&endblock=99999999" +
                 "&sort=desc" +
                 "&offset=1" +  // Fetch only the latest transaction
                 "&apikey=" + apiKey;
    
    http.begin(url);
    int httpResponseCode = http.GET();
    
    if (httpResponseCode > 0) {
      String response = http.getString();
      
      DynamicJsonDocument doc(16384);
      DeserializationError error = deserializeJson(doc, response);
      
      if (!error && doc["status"] == "1" && doc["message"] == "OK") {
        JsonArray txs = doc["result"].as<JsonArray>();
        
        if (txs.size() > 0) {
          JsonObject tx = txs[0];
          String hash = tx["hash"].as<String>();
          String to = tx["to"].as<String>();
          String value = tx["value"].as<String>();
          
          if (hash != lastProcessedTx && to.equalsIgnoreCase(merchantAddress)) {
            Serial.println("New transaction detected!");
            Serial.print("Hash: ");
            Serial.println(hash);
            Serial.print("Amount (wei): ");
            Serial.println(value);
            
            // Convert wei to ether
            float ether = value.toFloat() / 1e18;
            // Convert ether to INR
            float inrAmount = ether * ethToUsd * usdToInr;
            String inrStr = String(inrAmount, 2);
            
            playSound(inrStr);
            saveLastProcessedTx(hash);
          }
        }
      } else {
        Serial.println("Error parsing JSON or invalid API response");
      }
    } else {
      Serial.println("Error on HTTP request");
    }
    http.end();
  } else {
    Serial.println("WiFi Disconnected");
    isWifiConnected = false;
  }
}

void playSound(const String &amount) {
  Serial.println("Playing sound for transaction!");
  Serial.print("Amount received: ");
  Serial.print(amount);
  Serial.println(" INR");
  
  // Play "received an amount of" sound
  playSoundTrack("received.mp3");
  
  // Break down the amount and play corresponding sounds
  String intPart = amount.substring(0, amount.indexOf('.'));
  String decimalPart = amount.substring(amount.indexOf('.') + 1);
  
  for (int i = 0; i < intPart.length(); i++) {
    String digit = String(intPart[i]);
    String trackName = digit + ".mp3";
    playSoundTrack(trackName);
    
    // Play position sound (thousand, lakh, crore, etc.)
    int position = intPart.length() - i - 1;
    if (position > 0) {
      String positionTrack;
      switch (position) {
        case 3: positionTrack = "thousand.mp3"; break;
        case 5: positionTrack = "lakh.mp3"; break;
        case 7: positionTrack = "crore.mp3"; break;
        default: positionTrack = ""; break;
      }
      if (positionTrack != "") {
        playSoundTrack(positionTrack);
      }
    }
  }
  
  // Play decimal part if exists
  if (decimalPart != "00") {
    playSoundTrack("point.mp3");
    for (char c : decimalPart) {
      String digit = String(c);
      String trackName = digit + ".mp3";
      playSoundTrack(trackName);
    }
  }
  
  playSoundTrack("rupees.mp3");
}

void playSoundTrack(const String &trackName) {
  String fullPath = "/sounds/" + trackName;
  file = new AudioFileSourceSD(fullPath.c_str());
  if (!file->isOpen()) {
    Serial.printf("Failed to open file: %s\n", fullPath.c_str());
    delete file;
    return;
  }
  
  id3 = new AudioFileSourceID3(file);
  mp3 = new AudioGeneratorMP3();
  
  if (!mp3->begin(id3, out)) {
    Serial.println("mp3->begin failed");
    delete mp3;
    delete id3;
    delete file;
    return;
  }
  
  while (mp3->isRunning()) {
    if (!mp3->loop()) mp3->stop();
  }
  
  delete mp3;
  delete id3;
  delete file;
  mp3 = nullptr;
  id3 = nullptr;
  file = nullptr;
}

void saveLastProcessedTx(const String &txHash) {
  preferences.putString("lastTx", txHash);
  lastProcessedTx = txHash;
}

String getLastProcessedTx() {
  return preferences.getString("lastTx", "");
}

void playPreviousTransaction() {
  String prevTx = getLastProcessedTx();
  if (prevTx != "") {
    Serial.println("Playing sound for previous transaction");
    // Here you would typically fetch the transaction details and amount
    // For demonstration, we'll just play a generic sound
    playSoundTrack("previous_transaction.mp3");
  } else {
    Serial.println("No previous transaction found");
  }
}

void connectToWifi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    isWifiConnected = true;
  } else {
    Serial.println("\nFailed to connect to WiFi. Please check your credentials.");
    isWifiConnected = false;
  }
}