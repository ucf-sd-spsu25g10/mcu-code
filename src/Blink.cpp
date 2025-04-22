/* Blink Example with Web Server for ESP32
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <Arduino.h>
#include <WiFi.h>        // ESP32 WiFi library
#include <WebServer.h>   // ESP32 WebServer
#include <ArduinoJson.h> // Add JSON library
#include <Wire.h>        // Include I2C library
#include "credentials.h" // Include the credentials header file
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Web server on port 80
WebServer server(80);

// Define the GPIO for blinking - using CONFIG_BLINK_GPIO from build flags
#ifndef CONFIG_BLINK_GPIO
#define CONFIG_BLINK_GPIO 2  // Default to GPIO2 (onboard LED for many ESP32 boards)
#endif

// Define I2C pins
#define I2C_SDA 21
#define I2C_SCL 22

// Define PWM pins and configuration
#define PWM_PIN 5        // PWM output pin
#define PWM_CHANNEL 0    // PWM channel (ESP32 has 16 channels)
#define PWM_RESOLUTION 8 // 8-bit resolution (0-255)
#define PWM_FREQ_HZ 10   // Initial frequency 10Hz
#define PWM_DUTY_PCT 50  // Initial duty cycle 50%

// Define timing variables
bool ledState = false;

// Array to store received numbers
const int MAX_NUMBERS = 20;
int receivedNumbers[MAX_NUMBERS];
int numbersCount = 0;

// Mutex for protecting shared resources
SemaphoreHandle_t xMutex = NULL;

// Task handles
TaskHandle_t uartTaskHandle = NULL;
TaskHandle_t webServerTaskHandle = NULL;
TaskHandle_t i2cTaskHandle = NULL; // Add task handle for I2C task
TaskHandle_t pwmTaskHandle = NULL; // Add new PWM task handle

// Add a flag for web server status
volatile bool webServerActive = true;

// Add PWM configuration variables
int pwmFrequency = PWM_FREQ_HZ;    // PWM frequency in Hz
int pwmDutyCycle = PWM_DUTY_PCT;   // PWM duty cycle percentage

void handleRoot() {
  String html = "<html><body>";
  html += "<h1>ESP32 API Server</h1>";
  html += "<p>LED is currently: ";
  
  if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
    html += (ledState ? "ON" : "OFF");
    xSemaphoreGive(xMutex);
  }
  
  html += "</p>";
  html += "<p>Send a POST request to /api/cartList with a JSON array of numbers</p>";
  
  // Display received numbers if any
  if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
    if (numbersCount > 0) {
      html += "<h2>Received Numbers:</h2><ul>";
      for (int i = 0; i < numbersCount; i++) {
        html += "<li>" + String(receivedNumbers[i]) + "</li>";
      }
      html += "</ul>";
    }
    xSemaphoreGive(xMutex);
  }
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleToggle() {
  if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
    ledState = !ledState;
    digitalWrite(CONFIG_BLINK_GPIO, ledState);
    xSemaphoreGive(xMutex);
  }
  
  server.sendHeader("Location", "/");
  server.send(303);  // Redirect back to root
}

void handleNumbersApi() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
    return;
  }

  String postBody = server.arg("plain");
  Serial.println("Received POST data: " + postBody);
  
  // Allocate JsonDocument
  DynamicJsonDocument doc(1024);
  
  // Deserialize the JSON document
  DeserializationError error = deserializeJson(doc, postBody);
  
  // Test if parsing succeeds
  if (error) {
    String errorMsg = "{\"error\":\"" + String(error.c_str()) + "\"}";
    server.send(400, "application/json", errorMsg);
    Serial.println("JSON parse failed: " + String(error.c_str()));
    return;
  }
  
  // Verify we received an array
  if (!doc.is<JsonArray>()) {
    server.send(400, "application/json", "{\"error\":\"Expected JSON array of numbers\"}");
    return;
  }
  
  // Extract the numbers from the document
  JsonArray array = doc.as<JsonArray>();
  
  if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
    // Reset numbers array
    numbersCount = 0;
    
    // Store each number
    for (JsonVariant value : array) {
      if (value.is<int>() && numbersCount < MAX_NUMBERS) {
        receivedNumbers[numbersCount++] = value.as<int>();
      }
    }
    
    // Toggle LED to indicate successful reception
    ledState = !ledState;
    digitalWrite(CONFIG_BLINK_GPIO, ledState);
    
    xSemaphoreGive(xMutex);
  }
  
  // Send response
  String response = "{\"status\":\"success\",\"count\":" + String(numbersCount) + "}";
  server.send(200, "application/json", response);
  
  // Mark web server as inactive to stop it after sending response
  webServerActive = false;
  Serial.println("Valid JSON received. Web server will disconnect.");
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  
  server.send(404, "text/plain", message);
}

// UART task - handles LED blinking and UART streaming
void uartTask(void *parameter) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(500); // 500ms blink interval
  xLastWakeTime = xTaskGetTickCount();
  bool streamingStarted = false;
  int currentIndex = 0;
  
  for (;;) {
    // Toggle LED
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      digitalWrite(CONFIG_BLINK_GPIO, !digitalRead(CONFIG_BLINK_GPIO));
      xSemaphoreGive(xMutex);
    }
    
    if (webServerActive) {
      Serial.println("Server running, IP: " + WiFi.localIP().toString());
    } else {
      // Server is done, stream numbers over UART
      if (!streamingStarted) {
        Serial.println("\n----- STREAMING RECEIVED NUMBERS OVER UART -----");
        streamingStarted = true;
      }
      
      // Stream one number per cycle over UART
      if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
        if (currentIndex < numbersCount) {
          Serial.print("Number ");
          Serial.print(currentIndex + 1);
          Serial.print("/");
          Serial.print(numbersCount);
          Serial.print(": ");
          Serial.println(receivedNumbers[currentIndex]);
          currentIndex++;
        }
        else if (currentIndex == numbersCount && numbersCount > 0) {
          Serial.println("----- ALL NUMBERS STREAMED -----");
          // Reset to start streaming again after a delay
          if (currentIndex >= numbersCount + 10) { // Wait 10 cycles before repeating
            currentIndex = 0;
          } else {
            currentIndex++;
          }
        }
        xSemaphoreGive(xMutex);
      }
    }
    
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// Web server task - handles client requests
void webServerTask(void *parameter) {
  // Connect to WiFi network using credentials from header file
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("");
  Serial.print("Connecting to WiFi");
  
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    // Blink LED while connecting to indicate progress
    digitalWrite(CONFIG_BLINK_GPIO, !digitalRead(CONFIG_BLINK_GPIO));
  }
  
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WIFI_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Set up web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/toggle", HTTP_GET, handleToggle);
  server.on("/api/cartList", HTTP_POST, handleNumbersApi);
  server.onNotFound(handleNotFound);
  
  // Start the server
  server.begin();
  Serial.println("HTTP server started");
  
  for (;;) {
    if (webServerActive) {
      server.handleClient();
    } else {
      // Disconnect WiFi if web server is inactive
      WiFi.disconnect();
      Serial.println("WiFi disconnected.");
      vTaskDelete(NULL); // Delete this task
    }
    vTaskDelay(1); // Small delay to allow other tasks to run
  }
}

// I2C master task - runs on core 1
void i2cTask(void *parameter) {
  // Initialize I2C with pins defined earlier
  Wire.begin(I2C_SDA, I2C_SCL);
  
  Serial.println("I2C initialized as master on Core " + String(xPortGetCoreID()));
  
  const int I2C_SLAVE_ADDR = 0x08; // Example slave address
  uint8_t requestCount = 0;
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 1 second interval
  
  xLastWakeTime = xTaskGetTickCount();
  
  for(;;) {
    if (!webServerActive && numbersCount > 0) {
      // Only start I2C communications after web server is done
      
      if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
        // Get the current number index based on request count
        int currentIdx = requestCount % numbersCount;
        int valueToSend = receivedNumbers[currentIdx];
        xSemaphoreGive(xMutex);
        
        // Send the value to I2C slave
        Wire.beginTransmission(I2C_SLAVE_ADDR);
        Wire.write((uint8_t)(valueToSend & 0xFF));         // Low byte
        Wire.write((uint8_t)((valueToSend >> 8) & 0xFF));  // High byte
        uint8_t result = Wire.endTransmission();
        
        Serial.print("[I2C Core ");
        Serial.print(xPortGetCoreID());
        Serial.print("] Sent value ");
        Serial.print(valueToSend);
        Serial.print(" to slave (");
        Serial.print(currentIdx + 1);
        Serial.print("/");
        Serial.print(numbersCount);
        Serial.print(") - Result: ");
        
        switch(result) {
          case 0: Serial.println("Success"); break;
          case 1: Serial.println("Data too long"); break;
          case 2: Serial.println("NACK on address"); break;
          case 3: Serial.println("NACK on data"); break;
          case 4: Serial.println("Other error"); break;
          default: Serial.println("Unknown error"); break;
        }
        
        requestCount++;
      }
    }
    
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// PWM task - drives PWM output with configurable rate and duty cycle
void pwmTask(void *parameter) {
  // Initialize PWM on designated channel
  ledcSetup(PWM_CHANNEL, pwmFrequency, PWM_RESOLUTION);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  
  // Calculate initial duty value (for 8-bit resolution)
  uint32_t dutyCycleValue = (pwmDutyCycle * 255) / 100;
  ledcWrite(PWM_CHANNEL, dutyCycleValue);
  
  Serial.print("PWM initialized on pin ");
  Serial.print(PWM_PIN);
  Serial.print(" with frequency ");
  Serial.print(pwmFrequency);
  Serial.print(" Hz, duty cycle ");
  Serial.print(pwmDutyCycle);
  Serial.print("% (");
  Serial.print(dutyCycleValue);
  Serial.println("/255)");
  
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(5000); // Status update every 5 seconds
  
  for (;;) {
    // After JSON received, you can optionally modify PWM based on received values
    if (!webServerActive && numbersCount > 0) {
      if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
        // Example: Use the first number to set frequency if it's within valid range
        if (numbersCount > 0 && receivedNumbers[0] >= 1 && receivedNumbers[0] <= 1000) {
          int newFreq = receivedNumbers[0];
          if (newFreq != pwmFrequency) {
            pwmFrequency = newFreq;
            ledcChangeFrequency(PWM_CHANNEL, pwmFrequency, PWM_RESOLUTION);
            Serial.print("[PWM] Frequency changed to ");
            Serial.print(pwmFrequency);
            Serial.println(" Hz");
          }
        }
        
        // Use second number for duty cycle if available and valid
        if (numbersCount > 1 && receivedNumbers[1] >= 0 && receivedNumbers[1] <= 100) {
          int newDuty = receivedNumbers[1];
          if (newDuty != pwmDutyCycle) {
            pwmDutyCycle = newDuty;
            dutyCycleValue = (pwmDutyCycle * 255) / 100;
            ledcWrite(PWM_CHANNEL, dutyCycleValue);
            Serial.print("[PWM] Duty cycle changed to ");
            Serial.print(pwmDutyCycle);
            Serial.println("%");
          }
        }
        xSemaphoreGive(xMutex);
      }
    }
    
    // Print periodic status message
    Serial.print("[PWM Core ");
    Serial.print(xPortGetCoreID());
    Serial.print("] Running at ");
    Serial.print(pwmFrequency);
    Serial.print(" Hz with ");
    Serial.print(pwmDutyCycle);
    Serial.println("% duty cycle");
    
    // Wait for next update cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Blink Example with Web Server");
  
  // Create mutex for protecting shared resources
  xMutex = xSemaphoreCreateMutex();
  
  // Setup LED pin
  pinMode(CONFIG_BLINK_GPIO, OUTPUT);
  
  // Create RTOS tasks
  xTaskCreatePinnedToCore(
    uartTask,           // Function that implements the task
    "UartTask",         // Text name for the task
    2048,               // Stack size in words, not bytes
    NULL,               // Parameter passed into the task
    1,                  // Priority at which the task is created
    &uartTaskHandle,    // Used to pass out the created task's handle
    0                   // Run on core 0
  );
  
  xTaskCreate(
    webServerTask,      // Function that implements the task
    "WebServerTask",    // Text name for the task
    4096,               // Stack size in words, not bytes (larger for web server)
    NULL,               // Parameter passed into the task
    1,                  // Priority at which the task is created
    &webServerTaskHandle // Used to pass out the created task's handle
  );

  xTaskCreatePinnedToCore(
    i2cTask,            // Function that implements the task
    "I2CTask",          // Text name for the task
    2048,               // Stack size in words, not bytes
    NULL,               // Parameter passed into the task
    1,                  // Priority at which the task is created
    &i2cTaskHandle,     // Used to pass out the created task's handle
    1                   // Run on core 1
  );

  xTaskCreatePinnedToCore(
    pwmTask,            // Function that implements the task
    "PWMTask",          // Text name for the task
    2048,               // Stack size in words, not bytes
    NULL,               // Parameter passed into the task
    1,                  // Priority at which the task is created
    &pwmTaskHandle,     // Used to pass out the created task's handle
    0                   // Run on core 0
  );
}

void loop() {
  // Empty loop - tasks are handling everything
  vTaskDelay(portMAX_DELAY); // Just wait forever, effectively suspending this task
}
