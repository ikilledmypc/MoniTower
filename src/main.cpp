#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// WiFi credentials
const char* ssid = "";
const char* password = "";

// Datadog API credentials
const char* DATADOG_API_KEY = "YOUR_DATADOG_API_KEY";
const char* DATADOG_APP_KEY = "YOUR_DATADOG_APP_KEY";
const char* DATADOG_HOST = "api.datadoghq.com";
const int DATADOG_PORT = 443;

// WS2812 LED Strip configuration
#define LED_PIN 8
#define LED_COUNT 16  // Change this to match your LED strip count
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiClient wifiClient;
HttpClient httpClient(wifiClient, DATADOG_HOST, DATADOG_PORT);

// Animation variables
unsigned long lastAnimationTime = 0;
int animationIndex = 0;
const int ANIMATION_DELAY = 100;  // milliseconds between animation frames
const int ACTIVE_LED_COUNT = 3;   // Number of bright LEDs in the animation
const int DIM_BRIGHTNESS = 30;    

// Function to set LED color based on monitor status
void setLEDStatus(const char* status) {
  // Helper function to get color for status
  uint32_t fullColor;
  
  if (strcmp(status, "ok") == 0) {
    fullColor = strip.Color(0, 255, 0);  // Green
  } else if (strcmp(status, "alert") == 0) {
    fullColor = strip.Color(255, 0, 0);  // Red
  } else if (strcmp(status, "warn") == 0) {
    fullColor = strip.Color(255, 165, 0);  // Orange
  } else if (strcmp(status, "no data") == 0) {
    fullColor = strip.Color(0, 0, 255);  // Blue
  } else {
    fullColor = strip.Color(128, 128, 128);  // Gray (unknown)
  }
  
  // Dim the color to 30% brightness for off LEDs
  uint8_t r = (uint8_t)(fullColor >> 16);
  uint8_t g = (uint8_t)(fullColor >> 8);
  uint8_t b = (uint8_t)fullColor;
  
  r = (r * DIM_BRIGHTNESS) / 255;
  g = (g * DIM_BRIGHTNESS) / 255;
  b = (b * DIM_BRIGHTNESS) / 255;
  
  uint32_t dimmedColor = strip.Color(r, g, b);
  
  // Animate the LEDs - 6 bright LEDs looping around
  for (int i = 0; i < strip.numPixels(); i++) {
    // Calculate distance from the current animation position
    int distance = (i - animationIndex + strip.numPixels()) % strip.numPixels();
    
    // If within active LED range, use full brightness, otherwise use dimmed
    if (distance < ACTIVE_LED_COUNT) {
      strip.setPixelColor(i, fullColor);
    } else {
      strip.setPixelColor(i, dimmedColor);
    }
  }
  
  strip.show();
}

// Function to update animation frame
void updateAnimation() {
  unsigned long currentTime = millis();
  if (currentTime - lastAnimationTime >= ANIMATION_DELAY) {
    lastAnimationTime = currentTime;
    animationIndex = (animationIndex + 1) % strip.numPixels();
  }
}

// Function to query Datadog monitor status
void checkMonitorStatus() {
  updateAnimation();  // Update animation frame
  
  String path = "/api/v1/monitor?api_key=" + String(DATADOG_API_KEY) + 
                "&application_key=" + String(DATADOG_APP_KEY);
  
  Serial.println("Querying Datadog monitor status...");
  
  httpClient.get(path);
  int statusCode = httpClient.responseStatusCode();
  String response = httpClient.responseBody();
  
  Serial.print("Status Code: ");
  Serial.println(statusCode);
  Serial.print("Response: ");
  Serial.println(response);
  
  // Parse JSON response
  if (statusCode == 200) {
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      Serial.println("Monitor Status Report:");
      // Iterate through monitors and track overall status
      JsonArray monitors = doc.as<JsonArray>();
      const char* overallStatus = "ok";
      
      for (JsonObject monitor : monitors) {
        const char* monitorStatus = monitor["overall_state"].as<const char*>();
        Serial.print("Monitor: ");
        Serial.print(monitor["name"].as<const char*>());
        Serial.print(" - Status: ");
        Serial.println(monitorStatus);
        
        // Determine overall status (alert takes priority)
        if (strcmp(monitorStatus, "alert") == 0) {
          overallStatus = "alert";
        } else if (strcmp(monitorStatus, "warn") == 0 && strcmp(overallStatus, "alert") != 0) {
          overallStatus = "warn";
        }
      }
      
      // Update LED strip with overall status
      setLEDStatus(overallStatus);
    } else {
      Serial.print("JSON parsing error: ");
      Serial.println(error.c_str());
      setLEDStatus("no data");
    }
  }
}

void checkMonitorStatusDummy() {
  updateAnimation();
  setLEDStatus("ok");  // Simulate "alert" status for dummy check
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\nDatadog Lighthouse Starting...");
  
  // Connect to WiFi
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi");
  }
  
  // Initialize LED strip
  strip.begin();
  strip.show();
  setLEDStatus("no data");  // Start with blue (no data)
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    checkMonitorStatusDummy();
  } else {
    updateAnimation();
    setLEDStatus("no data");
  }
  
  // Small delay to allow animation updates
  delay(10);
}