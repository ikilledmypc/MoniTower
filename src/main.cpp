#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <hardware/watchdog.h>

// WiFi credentials - replaced by stored credentials
const char* DATADOG_API_KEY = "YOUR_DATADOG_API_KEY";
const char* DATADOG_APP_KEY = "YOUR_DATADOG_APP_KEY";
const char* DATADOG_HOST = "api.datadoghq.com";
const int DATADOG_PORT = 443;

// WS2812 LED Strip configuration
#define LED_PIN 8
#define LED_COUNT 16
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// File system constants
#define CREDENTIALS_FILE "/credentials.json"
#define AP_SSID "MoniTower-Setup"
#define AP_PASSWORD "MoniTower123"
#define BOOT_COUNT_FILE "/boot_count.json"
#define MAX_BOOT_COUNT 3

// Animation variables
unsigned long lastAnimationTime = 0;
int animationIndex = 0;
const int ANIMATION_DELAY = 100;
const int ACTIVE_LED_COUNT = 3;
const int DIM_BRIGHTNESS = 30;
char currentStatus[20] = "no data";  // Track current status for animation updates

// WiFi and provisioning variables
WiFiClientSecure wifiClient;
HttpClient* httpClient = nullptr;
WebServer server(80);
bool inAPMode = false;
bool wifiConnectAttempted = false;
unsigned long wifiConnectStartTime = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000; // 15 seconds to connect

// Structure to hold WiFi credentials
struct Credentials {
  char ssid[33];
  char password[64];
};

Credentials storedCredentials = {"", ""};

// ===== Forward Declarations =====
void handleRoot();
void handleConfigure();
void handleNotFound();
void startAccessPoint();
void setLEDStatus(const char* status);
void updateAnimation();

// ===== File System Functions =====
bool loadCredentials() {
  if (!LittleFS.exists(CREDENTIALS_FILE)) {
    Serial.println("No credentials file found");
    return false;
  }
  
  File file = LittleFS.open(CREDENTIALS_FILE, "r");
  if (!file) {
    Serial.println("Failed to open credentials file");
    return false;
  }
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("Failed to parse credentials: ");
    Serial.println(error.c_str());
    return false;
  }
  
  if (!doc.containsKey("ssid") || !doc.containsKey("password")) {
    Serial.println("Credentials missing ssid or password");
    return false;
  }
  
  strlcpy(storedCredentials.ssid, doc["ssid"].as<const char*>(), sizeof(storedCredentials.ssid));
  strlcpy(storedCredentials.password, doc["password"].as<const char*>(), sizeof(storedCredentials.password));
  
  Serial.println("Credentials loaded successfully");
  Serial.print("SSID: ");
  Serial.println(storedCredentials.ssid);
  
  return true;
}

bool saveCredentials(const char* ssid, const char* password) {
  StaticJsonDocument<512> doc;
  doc["ssid"] = ssid;
  doc["password"] = password;
  
  File file = LittleFS.open(CREDENTIALS_FILE, "w");
  if (!file) {
    Serial.println("Failed to open credentials file for writing");
    return false;
  }
  
  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write credentials to file");
    file.close();
    return false;
  }
  
  file.close();
  strlcpy(storedCredentials.ssid, ssid, sizeof(storedCredentials.ssid));
  strlcpy(storedCredentials.password, password, sizeof(storedCredentials.password));
  
  Serial.println("Credentials saved successfully");
  return true;
}

bool deleteCredentials() {
  if (LittleFS.remove(CREDENTIALS_FILE)) {
    Serial.println("Credentials deleted successfully");
    memset(storedCredentials.ssid, 0, sizeof(storedCredentials.ssid));
    memset(storedCredentials.password, 0, sizeof(storedCredentials.password));
    return true;
  }
  Serial.println("Failed to delete credentials");
  return false;
}

// ===== Boot Loop Detection =====
bool loadBootCount(int& count) {
  if (!LittleFS.exists(BOOT_COUNT_FILE)) {
    return false;
  }
  
  File file = LittleFS.open(BOOT_COUNT_FILE, "r");
  if (!file) return false;
  
  StaticJsonDocument<64> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) return false;
  
  count = doc["count"] | 0;
  return true;
}

bool saveBootCount(int count) {
  StaticJsonDocument<64> doc;
  doc["count"] = count;
  
  File file = LittleFS.open(BOOT_COUNT_FILE, "w");
  if (!file) return false;
  
  serializeJson(doc, file);
  file.close();
  return true;
}

bool resetBootCount() {
  return LittleFS.remove(BOOT_COUNT_FILE);
}

void checkBootLoop() {
  int count = 0;
  
  // Load previous boot count
  if (loadBootCount(count)) {
    count++;  // Increment on each boot
    Serial.print("Consecutive boot count: ");
    Serial.println(count);
    
    if (count >= MAX_BOOT_COUNT) {
      Serial.println("\n*** BOOT LOOP DETECTED ***");
      Serial.println("Resetting configuration to factory defaults...");
      deleteCredentials();
      resetBootCount();
      setLEDStatus("ap mode");
      return;
    }
  } else {
    // First boot
    count = 1;
    Serial.println("First boot detected");
  }
  
  saveBootCount(count);
}

// ===== Access Point Setup =====
void startAccessPoint() {
  Serial.println("\nStarting Access Point mode...");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  IPAddress apIP(192, 168, 4, 1);
  IPAddress netmask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netmask);
  
  Serial.print("Access Point started: ");
  Serial.println(AP_SSID);
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
  
  inAPMode = true;
  
  // Set up web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/configure", HTTP_POST, handleConfigure);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Web server started on port 80");
}

// ===== Web Server Handlers =====
void handleRoot() {
  // Get MAC address
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  
  String html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>MoniTower WiFi Setup</title>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 10px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.3);
            padding: 40px;
            max-width: 400px;
            width: 100%;
        }
        h1 {
            color: #333;
            margin-bottom: 10px;
            text-align: center;
            font-size: 28px;
        }
        .subtitle {
            text-align: center;
            color: #666;
            margin-bottom: 8px;
            font-size: 14px;
        }
        .device-id {
            background: #f5f5f5;
            border: 1px solid #ddd;
            border-radius: 5px;
            padding: 10px;
            text-align: center;
            margin-bottom: 25px;
            font-family: 'Courier New', monospace;
            font-size: 12px;
            word-break: break-all;
            color: #555;
        }
        .device-id-label {
            font-size: 11px;
            color: #999;
            margin-bottom: 5px;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            color: #333;
            font-weight: 500;
            font-size: 14px;
        }
        input {
            width: 100%;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 5px;
            font-size: 14px;
            transition: border-color 0.3s;
        }
        input:focus {
            outline: none;
            border-color: #667eea;
            box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
        }
        button {
            width: 100%;
            padding: 12px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 5px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        .info {
            background: #f0f0f0;
            padding: 15px;
            border-radius: 5px;
            margin-top: 20px;
            font-size: 12px;
            color: #666;
            line-height: 1.6;
        }
        .success {
            background: #d4edda;
            color: #155724;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            display: none;
        }
        .error {
            background: #f8d7da;
            color: #721c24;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            display: none;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🌐 MoniTower</h1>
        <p class="subtitle">WiFi Configuration</p>
        
        <div class="device-id">
            <div class="device-id-label">Device MAC Address</div>
            <div>)";
  
  html += macStr;
  
  html += R"(</div>
        </div>
        
        <div id="success" class="success">
            ✓ Settings saved! Device will restart and attempt to connect.
        </div>
        <div id="error" class="error">
            ✗ Error: <span id="errorMsg"></span>
        </div>
        
        <form id="wifiForm">
            <div class="form-group">
                <label for="ssid">WiFi Network (SSID)</label>
                <input type="text" id="ssid" name="ssid" required placeholder="Enter network name">
            </div>
            
            <div class="form-group">
                <label for="password">WiFi Password <span style="font-size: 12px; color: #999;">(optional)</span></label>
                <input type="password" id="password" name="password" placeholder="Leave empty for open networks">
            </div>
            
            <button type="submit">Save & Connect</button>
            
            <div class="info">
                <strong>Instructions:</strong><br>
                1. Enter your WiFi network name<br>
                2. Enter your WiFi password<br>
                3. Click 'Save & Connect'<br>
                4. Device will restart and connect
            </div>
        </form>
    </div>
    
    <script>
        document.getElementById('wifiForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            
            const ssid = document.getElementById('ssid').value.trim();
            const password = document.getElementById('password').value;
            
            // SSID is required, password is optional
            if (!ssid) {
                showError('Please enter WiFi network name (SSID)');
                return;
            }
            
            try {
                const response = await fetch('/configure', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({
                        ssid: ssid,
                        password: password
                    })
                });
                
                if (response.ok) {
                    showSuccess();
                    document.getElementById('wifiForm').style.display = 'none';
                    setTimeout(() => location.reload(), 5000);
                } else {
                    const error = await response.text();
                    showError(error || 'Failed to save settings');
                }
            } catch (err) {
                showError('Connection error: ' + err.message);
            }
        });
        
        function showSuccess() {
            document.getElementById('success').style.display = 'block';
            document.getElementById('error').style.display = 'none';
        }
        
        function showError(msg) {
            document.getElementById('error').style.display = 'block';
            document.getElementById('errorMsg').textContent = msg;
            document.getElementById('success').style.display = 'none';
        }
    </script>
</body>
</html>
  )";
  
  server.send(200, "text/html", html);
}

void handleConfigure() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data provided");
    return;
  }
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  const char* ssid = doc["ssid"];
  const char* password = doc["password"];
  
  // SSID is required, but password can be empty for open networks
  if (!ssid || strlen(ssid) == 0) {
    server.send(400, "text/plain", "SSID is required");
    return;
  }
  
  // Handle null password for open networks
  const char* pwd = (password && strlen(password) > 0) ? password : "";
  
  if (saveCredentials(ssid, pwd)) {
    server.send(200, "text/plain", "OK");
    // Schedule restart after response is sent
    delay(100);
    watchdog_reboot(0, 0, 100);
  } else {
    server.send(500, "text/plain", "Failed to save credentials");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// ===== WiFi Connection =====
bool connectToWiFi() {
  if (storedCredentials.ssid[0] == '\0') {
    Serial.println("No stored credentials, entering AP mode");
    return false;
  }
  
  Serial.print("Attempting to connect to WiFi: ");
  Serial.println(storedCredentials.ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedCredentials.ssid, storedCredentials.password);
  
  wifiConnectAttempted = true;
  wifiConnectStartTime = millis();
  
  return true;
}

bool waitForWiFiConnection() {
  if (!wifiConnectAttempted) return false;
  
  unsigned long elapsed = millis() - wifiConnectStartTime;
  
  // Check if connected
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    wifiConnectAttempted = false;
    resetBootCount();  // Clear boot counter on successful connection
    return true;
  }
  
  // If timeout reached, connection failed
  if (elapsed >= WIFI_CONNECT_TIMEOUT) {
    Serial.println("\nFailed to connect to WiFi - timeout");
    Serial.println("Wiping credentials and entering AP mode");
    deleteCredentials();
    startAccessPoint();
    return false;
  }
  
  // Still attempting
  if (elapsed % 500 == 0) {
    Serial.print(".");
  }
  
  return false;
}


// ===== LED Functions =====
void setLEDStatus(const char* status) {
  strlcpy(currentStatus, status, sizeof(currentStatus));  // Save status for animation updates
  uint32_t fullColor;
  
  if (strcmp(status, "ok") == 0) {
    fullColor = strip.Color(0, 255, 0);  // Green
  } else if (strcmp(status, "alert") == 0) {
    fullColor = strip.Color(255, 0, 0);  // Red
  } else if (strcmp(status, "warn") == 0) {
    fullColor = strip.Color(255, 165, 0);  // Orange
  } else if (strcmp(status, "no data") == 0) {
    fullColor = strip.Color(0, 0, 255);  // Blue
  } else if (strcmp(status, "ap mode") == 0) {
    fullColor = strip.Color(255, 255, 0);  // Yellow - AP mode indicator
  } else {
    fullColor = strip.Color(128, 128, 128);  // Gray (unknown)
  }
  
  // Dim the color for off LEDs
  uint8_t r = (uint8_t)(fullColor >> 16);
  uint8_t g = (uint8_t)(fullColor >> 8);
  uint8_t b = (uint8_t)fullColor;
  
  r = (r * DIM_BRIGHTNESS) / 255;
  g = (g * DIM_BRIGHTNESS) / 255;
  b = (b * DIM_BRIGHTNESS) / 255;
  
  uint32_t dimmedColor = strip.Color(r, g, b);
  
  // Animate the LEDs
  for (int i = 0; i < strip.numPixels(); i++) {
    int distance = (i - animationIndex + strip.numPixels()) % strip.numPixels();
    
    if (distance < ACTIVE_LED_COUNT) {
      strip.setPixelColor(i, fullColor);
    } else {
      strip.setPixelColor(i, dimmedColor);
    }
  }
  
  strip.show();
}

void updateAnimation() {
  unsigned long currentTime = millis();
  if (currentTime - lastAnimationTime >= ANIMATION_DELAY) {
    lastAnimationTime = currentTime;
    animationIndex = (animationIndex + 1) % strip.numPixels();
    // Redraw the LEDs with the updated animation index
    setLEDStatus(currentStatus);
  }
}

// ===== Datadog Monitor Check =====
void checkMonitorStatus() {
  if (!WiFi.isConnected()) {
    return;
  }
  
  if (!httpClient) {
    httpClient = new HttpClient(wifiClient, DATADOG_HOST, DATADOG_PORT);
    httpClient->setHttpResponseTimeout(5000);  // 5 second timeout instead of 30
    httpClient->setHttpWaitForDataDelay(50);   // Check more frequently
  }
  
  String path = "/api/v1/monitor?api_key=" + String(DATADOG_API_KEY) + 
                "&application_key=" + String(DATADOG_APP_KEY);
  
  Serial.println("Querying Datadog monitor status...");
  
  int err = httpClient->get(path);
  if (err != 0) {
    Serial.print("Request error: ");
    Serial.println(err);
    setLEDStatus("no data");
    return;
  }
  
  int statusCode = httpClient->responseStatusCode();
  String response = httpClient->responseBody();
  
  Serial.print("Status Code: ");
  Serial.println(statusCode);
  
  if (statusCode == 200) {
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      Serial.println("Monitor Status Report:");
      JsonArray monitors = doc.as<JsonArray>();
      const char* overallStatus = "ok";
      
      for (JsonObject monitor : monitors) {
        const char* monitorStatus = monitor["overall_state"].as<const char*>();
        Serial.print("Monitor: ");
        Serial.print(monitor["name"].as<const char*>());
        Serial.print(" - Status: ");
        Serial.println(monitorStatus);
        
        if (strcmp(monitorStatus, "alert") == 0) {
          overallStatus = "alert";
        } else if (strcmp(monitorStatus, "warn") == 0 && strcmp(overallStatus, "alert") != 0) {
          overallStatus = "warn";
        }
      }
      
      setLEDStatus(overallStatus);
    } else {
      Serial.print("JSON parsing error: ");
      Serial.println(error.c_str());
      setLEDStatus("no data");
    }
  }
}

// ===== Datadog Monitor Check =====
void checkMonitorStatusGoogle() {
  if (!WiFi.isConnected()) {
    Serial.println("WiFi not connected");
    setLEDStatus("no data");
    return;
  }
  
  Serial.print("WiFi status: ");
  Serial.println(WiFi.status());
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Gateway IP: ");
  Serial.println(WiFi.gatewayIP());
  
  if (!httpClient) {
    httpClient = new HttpClient(wifiClient, "www.google.com", 443);
  httpClient->setHttpResponseTimeout(5000);
  httpClient->setHttpWaitForDataDelay(50);
  }

  
  
  Serial.println("Attempting GET request...");
  
  int err = httpClient->get("/");
  Serial.print("get() returned: ");
  Serial.println(err);
  
  if (err != 0) {
    Serial.print("Connection failed with error: ");
    Serial.println(err);
    setLEDStatus("no data");
    return;
  }
  
  int statusCode = httpClient->responseStatusCode();

  httpClient->stop();  // Close the connection after the request
  
  Serial.print("Status Code: ");
  Serial.println(statusCode);
  
  if (statusCode == 200) {
      Serial.println("Google is reachable, setting status to OK");
      setLEDStatus("ok");
    } else {
      Serial.println("Google is not reachable, setting status to ALERT");
      setLEDStatus("alert");
    }
}

void checkMonitorStatusDummy() {
  setLEDStatus("ok");
}

// ===== Setup and Loop =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  wifiClient.setInsecure();  // Disable SSL certificate verification for simplicity (not recommended for production)

  Serial.println("\n\nMoniTower Starting...");
  
  // Initialize file system
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount file system!");
  } else {
    Serial.println("File system mounted successfully");
  }
  
  // Check for boot loop EARLY
  checkBootLoop();
  
  // Initialize LED strip
  strip.begin();
  strip.show();
  setLEDStatus("no data");
  
  // Load stored credentials
  if (loadCredentials()) {
    // Try to connect with stored credentials
    if (connectToWiFi()) {
      // Wait for connection asynchronously in loop
      Serial.println("Waiting for WiFi connection...");
    }
  } else {
    // No credentials saved, start AP mode
    startAccessPoint();
    setLEDStatus("ap mode");
  }
}

void loop() {
  // Handle web server requests if in AP mode
  if (inAPMode) {
    server.handleClient();
  }
  
  // Handle WiFi connection attempts
  if (wifiConnectAttempted) {
    if (waitForWiFiConnection()) {
      // Connection successful
      inAPMode = false;
      setLEDStatus("ok");
    } else if (WiFi.status() == WL_CONNECTED) {
      // Connection succeeded
      inAPMode = false;
      setLEDStatus("ok");
    }
  }
  
  // NOTE: Animation is now handled on core 1 in loop1()
  
  // If connected, check monitor status periodically
  if (WiFi.status() == WL_CONNECTED && !inAPMode) {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 30000) {  // Check every 30 seconds
      lastCheck = millis();
      checkMonitorStatusGoogle();  // Use dummy for now
    }
  }
  
  delay(100);
}

// ===== Core 1 - Animation Loop =====
void setup1() {
  // Core 1 setup (if needed)
}

void loop1() {
  // Core 1 continuously updates animation without blocking
  updateAnimation();
  delay(10);  // Small delay to prevent consuming too much CPU
}