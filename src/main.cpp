#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DFRobotDFPlayerMini.h>

// WiFi credentials ( will add later in .env)
const char* ssid = "Piedpiper";
const char* password = "piedpipermr";

// API details
const char* apiEndpoint = "https://api-sepolia.etherscan.io/api";
const char* apiKey = "Q7YB5BHRA9J4R7KS5T9BHV4W4Q3C74JG6C";

// Merchant-details
const char* merchantAddress = "0x7263B2E0D541206724a20f397296Bf43d86005F8";

unsigned long lastPaymentCheck = 0;
const unsigned long paymentCheckInterval = 10000; //checking every 10secs
String lastProcessedTx = "";
float ethToInr = 150000; 

Preferences preferences;
DFRobotDFPlayerMini myDFPlayer;

// Functions
void connectToWifi();
void checkForNewTransactions();
void playSound(float amount);
void saveLastProcessedTx(const String &txHash);
String getLastProcessedTx();
void playNumberSound(int number);
void playDecimalSound(int decimal);

void setup() {
  Serial.begin(115200);
  Serial.println("Cryptobox Startup"); //loggings(will remove later)
  
  Serial2.begin(9600);
  if (!myDFPlayer.begin(Serial2)) {
    Serial.println("DFPlayer Mini initialization failed.");
    return;
  }
  myDFPlayer.volume(30);  // volume (0-30)
  
  preferences.begin("crypto-box", false);
  lastProcessedTx = getLastProcessedTx();
  
  connectToWifi();
  Serial.println("Setup complete. Ready to check for transactions.");
}

//still some bugs
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    connectToWifi();
    return;
  }
  
  unsigned long currentMillis = millis();
  if (currentMillis - lastPaymentCheck >= paymentCheckInterval) {
    Serial.println("Checking for new transactions...");
    checkForNewTransactions();
    lastPaymentCheck = currentMillis;
  }
}

void connectToWifi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
}

void checkForNewTransactions() {
  HTTPClient http;
  String url = String(apiEndpoint) + 
               "?module=account" +
               "&action=txlist" +
               "&address=" + merchantAddress +
               "&startblock=0" +
               "&endblock=99999999" +
               "&sort=desc" +
               "&offset=1" +  
               "&apikey=" + apiKey;
  
  Serial.println("Sending HTTP request to: " + url);
  
  http.begin(url);
  int httpResponseCode = http.GET();
  
  Serial.println("HTTP Response Code: " + String(httpResponseCode));
  
  if (httpResponseCode == 200) {
    String response = http.getString();
    Serial.println("API Response: " + response);
    
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error && doc["status"] == "1" && doc["message"] == "OK") {
      JsonArray txs = doc["result"].as<JsonArray>();
      
      if (txs.size() > 0) {
        JsonObject tx = txs[0];
        String hash = tx["hash"].as<String>();
        String to = tx["to"].as<String>();
        String value = tx["value"].as<String>();
        
        Serial.println("Latest transaction hash: " + hash);
        Serial.println("To address: " + to);
        Serial.println("Value: " + value);
        
        if (hash != lastProcessedTx && to.equalsIgnoreCase(merchantAddress)) {
          float ether = value.toFloat() / 1e18;
          float inrAmount = ether * ethToInr;
          
          Serial.println("New transaction detected!");
          Serial.println("Hash: " + hash);
          Serial.println("Amount: " + String(inrAmount, 2) + " INR");
          
          playSound(inrAmount);
          saveLastProcessedTx(hash);
        } else {
          Serial.println("No new transaction detected.");
        }
      } else {
        Serial.println("No transactions found in the response.");
      }
    } else {
      Serial.println("Error parsing JSON or invalid API response");
      if (error) {
        Serial.println("Deserialization error: " + String(error.c_str()));
      }
    }
  } else {
    Serial.println("Error on HTTP request. Response code: " + String(httpResponseCode));
  }
  http.end();
}

void playSound(float amount) {
  int rupees = (int)amount;
  int paise = (int)((amount - rupees) * 100);
  
  if (rupees >= 10000000) {
    playNumberSound(rupees / 10000000);
    myDFPlayer.play(204);  // "crore"
    rupees %= 10000000;
  }
  
  if (rupees >= 100000) {
    playNumberSound(rupees / 100000);
    myDFPlayer.play(203);  // "lakh"
    rupees %= 100000;
  }
  
  if (rupees >= 1000) {
    playNumberSound(rupees / 1000);
    myDFPlayer.play(202);  // "thousand"
    rupees %= 1000;
  }
  
  if (rupees >= 100) {
    playNumberSound(rupees / 100);
    myDFPlayer.play(201);  // "hundred"
    rupees %= 100;
  }
  
  if (rupees > 0) {
    playNumberSound(rupees);
  }
  
  if (paise > 0) {
    myDFPlayer.play(200);  // "point"
    playDecimalSound(paise);
  }
  
  myDFPlayer.play(205);  // "rupees"
  delay(1000);  // Wait for "rupees" to finish playing
}

void playNumberSound(int number) {
  if (number <= 20) {
    myDFPlayer.play(number);
  } else if (number < 100) {
    myDFPlayer.play((number / 10) * 10);
    if (number % 10 != 0) {
      delay(800);
      myDFPlayer.play(number % 10);
    }
  }
  delay(800); 
}

void playDecimalSound(int decimal) {
  if (decimal < 20) {
    playNumberSound(decimal);
  } else {
    int tens = decimal / 10;
    int ones = decimal % 10;
    playNumberSound(tens * 10);
    if (ones > 0) {
      delay(800);
      playNumberSound(ones);
    }
  }
}

void saveLastProcessedTx(const String &txHash) {
  preferences.putString("lastTx", txHash);
  lastProcessedTx = txHash;
  Serial.println("Saved last processed transaction: " + txHash);
}

String getLastProcessedTx() {
  String tx = preferences.getString("lastTx", "");
  Serial.println("Retrieved last processed transaction: " + tx);
  return tx;
}
