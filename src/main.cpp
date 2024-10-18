#include <Arduino.h>
#include <ArduinoWebsockets.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DFRobotDFPlayerMini.h>
#include <HTTPClient.h>

using namespace websockets;

// WiFi credentials
const char* ssid = "Piedpiper";
const char* password = "piedpipermr";

// Alchemy WebSocket details
const char* websocket_url = "wss://eth-sepolia.g.alchemy.com/v2/2tFzzpbSHKxUmfmVaeMcF4H3ACJVxG9d";  // Full WSS URL

// Ethereum HTTP endpoint (for JSON-RPC requests)
const char* httpEndpoint = "https://eth-sepolia.g.alchemy.com/v2/2tFzzpbSHKxUmfmVaeMcF4H3ACJVxG9d";

// Merchant details
const char* merchantAddress = "0x7263B2E0D541206724a20f397296Bf43d86005F8";

// Exchange rate API endpoint
const char* exchangeRateApi = "https://api.coingecko.com/api/v3/simple/price?ids=ethereum&vs_currencies=inr";

// Button pin
const int BUTTON_PIN = 2;  // Change this to the actual pin you're using

// Global variables
Preferences preferences;
DFRobotDFPlayerMini myDFPlayer;
WebsocketsClient client;
float lastTransactionAmount = 0.0; // Debounce time in milliseconds

// Constants for WebSocket management
const unsigned long KEEP_ALIVE_INTERVAL = 30000;  // 30 seconds
const unsigned long RECONNECT_INTERVAL = 5000;    // 5 seconds
const int MAX_RECONNECT_ATTEMPTS = 5;

// Constants for rate limiting
const int MAX_WS_CONNECTIONS = 20000;  // per API key
const int MAX_SUBSCRIPTIONS = 1000;    // per connection
const int MAX_BATCH_SIZE = 20;         // per WebSocket request

// Global variables for connection management
unsigned long lastPingTime = 0;
unsigned long lastReconnectAttempt = 0;
int reconnectAttempts = 0;
bool isSubscribed = false;

// Function declarations
void connectToWifi();
bool setupWebSocket();
void onMessageCallback(WebsocketsMessage message);
void onEventsCallback(WebsocketsEvent event, String data);
void playSound(float amount);
void saveLastProcessedTx(const String &txHash, float amount);
String getLastProcessedTx();
void processTransaction(const String& txHash, const String& value);
void playNumberSound(int number);
void playDecimalSound(int decimal);
float getEthToInrRate();
String makeHttpRequest(const String& method, const String& params);
void subscribeToTransactions();

void onMessageCallback(WebsocketsMessage message) {
    Serial.println("Received message: " + message.data());
    
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, message.data());
    
    if (error) {
        Serial.println("Failed to parse WebSocket payload: " + String(error.c_str()));
        return;
    }
    
    // Handle subscription confirmation
    if (doc.containsKey("id") && doc.containsKey("result")) {
        String subscriptionId = doc["result"].as<String>();
        Serial.println("Subscription confirmed with ID: " + subscriptionId);
        return;
    }
    
    // Handle transaction notifications - specific to alchemy_minedTransactions format
    if (doc.containsKey("params") && 
        doc["params"].containsKey("result") && 
        doc["params"]["result"].containsKey("transaction")) {
        
        JsonObject transaction = doc["params"]["result"]["transaction"];
        bool removed = doc["params"]["result"]["removed"] | false;
        
        if (!removed) {  // Only process transactions that haven't been removed
            const char* to = transaction["to"];
            if (to && strcasecmp(to, merchantAddress) == 0) {
                String txHash = transaction["hash"].as<String>();
                String value = transaction["value"].as<String>();
                
                if (txHash != getLastProcessedTx()) {
                    Serial.println("New confirmed transaction detected!");
                    Serial.println("Hash: " + txHash);
                    processTransaction(txHash, value);
                }
            }
        }
    }
}

void processTransaction(const String& txHash, const String& value) {
    // Convert hex value to Wei
    uint64_t wei = strtoull(value.c_str(), NULL, 16);
    // Convert Wei to Ether
    float ether = (float)wei / 1e18;
    
    // Get current exchange rate
    float ethToInr = getEthToInrRate();
    float inrAmount = ether * ethToInr;
    
    Serial.printf("Transaction amount: %f ETH = %f INR\n", ether, inrAmount);
    
    // Play sound and save transaction
    playSound(inrAmount);
    saveLastProcessedTx(txHash, inrAmount);
}

void onEventsCallback(WebsocketsEvent event, String data) {
  if(event == WebsocketsEvent::ConnectionOpened) {
    Serial.println("WebSocket Connection Opened");
    // Subscribe to pending transactions
    String subscribeMsg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_subscribe\",\"params\":[\"alchemy_pendingTransactions\",{\"toAddress\":\"" + String(merchantAddress) + "\"}]}";
    client.send(subscribeMsg);
  } else if(event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("WebSocket Connection Closed");
  } else if(event == WebsocketsEvent::GotPing) {
    Serial.println("Got a Ping!");
  } else if(event == WebsocketsEvent::GotPong) {
    Serial.println("Got a Pong!");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Cryptobox Startup");
  delay(1000);
  
  Serial2.begin(9600);
  if (!myDFPlayer.begin(Serial2)) {
    Serial.println("DFPlayer Mini initialization failed.");
    // Don't return here, continue with the rest of the setup
  } else {
    Serial.println("DFPlayer Mini initialized successfully.");
  }

  Serial.println("Initializing preferences...");
  preferences.begin("crypto-box", false);
  Serial.println("Preferences initialized.");
  
  Serial.println("Setting up button pin...");
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("Button pin setup complete.");
  
  Serial.println("Attempting to connect to WiFi...");
  connectToWifi();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed. Restarting in 10 seconds...");
    delay(10000);
    ESP.restart();

  }

  
  // Attempt to connect to WebSocket
  Serial.println("Attempting to connect to WebSocket...");
  if (!setupWebSocket()) {
    Serial.println("Initial WebSocket connection failed. Will retry in loop.");
  } else {
    Serial.println("WebSocket connected successfully.");
  }
  
  Serial.println("Setup complete. Listening for transactions...");
}

void loop() {
    unsigned long currentTime = millis();
    
    // Check WebSocket connection
    if (!client.available()) {
        if (currentTime - lastReconnectAttempt >= RECONNECT_INTERVAL) {
            lastReconnectAttempt = currentTime;
            
            if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                Serial.println("Attempting to reconnect...");
                reconnectAttempts++;
                isSubscribed = false;
                
                if (setupWebSocket()) {
                    Serial.println("Reconnected successfully");
                } else {
                    Serial.printf("Reconnection attempt %d failed\n", reconnectAttempts);
                }
            } else {
                Serial.println("Max reconnection attempts reached. Restarting device...");
                ESP.restart();
            }
        }
    } else {
        // Handle WebSocket maintenance
        if (currentTime - lastPingTime >= KEEP_ALIVE_INTERVAL) {
            lastPingTime = currentTime;
            client.ping();
        }
        
        client.poll();
    }
}

bool setupWebSocket() {
  client.onMessage(onMessageCallback);
  client.onEvent(onEventsCallback);
  //  client.setInsecure(); //enable ssl

  Serial.println("Connecting to WebSocket...");
  bool connected = client.connect(websocket_url);

  if (connected) {
    Serial.println("Connected to WebSocket server");
    // Subscribe to pending transactions using Alchemy's specific method
    reconnectAttempts = 0;
        subscribeToTransactions();
        return true;
  }
    Serial.println("WebSocket connection failed!");
    return false;
  
}

void subscribeToTransactions() {
    if (!isSubscribed) {
        // Subscribe to mined transactions instead of pending
        String subscribeMsg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_subscribe\",\"params\":[\"alchemy_pendingTransactions\",{\"toAddress\":\"" + String(merchantAddress) + "\"}]}";
        client.send(subscribeMsg);
        isSubscribed = true;
        Serial.println("Subscribed to mined transactions");
    }
}


String makeHttpRequest(const String& method, const String& params) {
  HTTPClient http;
  http.begin(httpEndpoint);
  http.addHeader("Content-Type", "application/json");
  
  String payload = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\",\"params\":" + params + ",\"id\":1}";
  int httpResponseCode = http.POST(payload);
  
  String response = "{}";
  if (httpResponseCode == 200) {
    response = http.getString();
  } else {
    Serial.println("Error on HTTP request: " + String(httpResponseCode));
  }
  
  http.end();
  return response;
}

float getEthToInrRate() {
  HTTPClient http;
  http.begin(exchangeRateApi);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      float rate = doc["ethereum"]["inr"].as<float>();
      Serial.printf("Current ETH to INR rate: %.2f\n", rate);
      return rate;
    }
  }
  Serial.println("Failed to get current exchange rate. Using fallback value.");
  return 80000;
}

void playSound(float amount) {
  int rupees = (int)amount;
  int paise = (int)((amount - rupees) * 100);
  
  // Play the amount
  if (rupees >= 10000000) {
    playNumberSound(rupees / 10000000);
    myDFPlayer.play(204);  // "crore"
    rupees %= 10000000;
    delay(1000);
  }
  
  if (rupees >= 100000) {
    playNumberSound(rupees / 100000);
    myDFPlayer.play(203);  // "lakh"
    rupees %= 100000;
    delay(1000);
  }
  
  if (rupees >= 1000) {
    playNumberSound(rupees / 1000);
    myDFPlayer.play(202);  // "thousand"
    rupees %= 1000;
    delay(1000);
  }
  
  if (rupees >= 100) {
    playNumberSound(rupees / 100);
    myDFPlayer.play(201);  // "hundred"
    rupees %= 100;
    delay(1000);
  }
  
  if (rupees > 0) {
    playNumberSound(rupees);
    delay(1000);
  }
  
  if (paise > 0) {
    myDFPlayer.play(200);  // "point"
    delay(1000);
    playDecimalSound(paise);
    delay(1000);
  }
  
  myDFPlayer.play(205);  // "rupees"
  delay(1000);
}

void playNumberSound(int number) {
  if (number <= 20) {
    myDFPlayer.play(number);
  } else if (number < 100) {
    myDFPlayer.play((number / 10) * 10);
    delay(800);
    if (number % 10 != 0) {
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
    delay(800);
    if (ones > 0) {
      playNumberSound(ones);
    }
  }
}

void saveLastProcessedTx(const String &txHash, float amount) {
  preferences.putString("lastTx", txHash);
  preferences.putFloat("lastAmount", amount);
  lastTransactionAmount = amount;
  Serial.println("Saved last processed transaction: " + txHash + " Amount: " + String(amount, 2) + " INR");
}

String getLastProcessedTx() {
  String tx = preferences.getString("lastTx", "");
  lastTransactionAmount = preferences.getFloat("lastAmount", 0.0);
  Serial.println("Retrieved last processed transaction: " + tx + " Amount: " + String(lastTransactionAmount, 2) + " INR");
  return tx;
}

// void checkButton() {
//   int buttonState = digitalRead(BUTTON_PIN);
//   if (buttonState == LOW) {
//     unsigned long currentMillis = millis();
//     if (currentMillis - lastButtonPress > debounceDelay) {
//       lastButtonPress = currentMillis;
//       Serial.println("Button pressed. Replaying last transaction sound.");
//       playSound(lastTransactionAmount);
//     }
//   }
// }

void connectToWifi() {
  Serial.println("Inside connectToWifi function");
  WiFi.mode(WIFI_STA);
  Serial.printf("Attempting to connect to SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  
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
    Serial.print("Signal strength (RSSI): ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("\nFailed to connect to WiFi.");
    Serial.print("WiFi status: ");
    Serial.println(WiFi.status());
  }
}

void handleWebSocketError(int errorCode) {
    switch(errorCode) {
        case 32600:  // Exceeding batch size limit
            Serial.println("Error: Batch size limit exceeded");
            delay(5000);  // Wait before retrying
            setupWebSocket();
            break;
            
        case -1:  // Connection error (custom code)
            if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                unsigned long backoff = RECONNECT_INTERVAL * (1 << reconnectAttempts);
                Serial.printf("Connection error. Retrying in %lu ms\n", backoff);
                delay(backoff);
                setupWebSocket();
            } else {
                Serial.println("Max reconnection attempts reached. Restarting...");
                ESP.restart();
            }
            break;
            
        default:
            Serial.printf("Unknown WebSocket error: %d\n", errorCode);
            break;
    }
}

bool isRateLimited = false;
unsigned long rateLimitResetTime = 0;

void checkRateLimits() {
    if (isRateLimited) {
        if (millis() > rateLimitResetTime) {
            isRateLimited = false;
        } else {
            return;
        }
    }
    
    // Implementation would depend on your rate tracking logic
}