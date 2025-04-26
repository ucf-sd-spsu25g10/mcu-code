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
#include "freertos/queue.h" // Add for queue support

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
#define PWM_PIN 11        // PWM output pin
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

// Add UART communication parameters
#define UART_BUFFER_SIZE 64
char uartBuffer[UART_BUFFER_SIZE];
int uartBufferIndex = 0;
bool newUartDataAvailable = false;

// Add feedback control variables
float feedbackValue = 0.0;
#define I2C_HAPTIC_ADDR 0x5A  // Example address for haptic driver
#define I2C_SPEAKER_ADDR 0x34 // Example address for audio controller
bool feedbackReceived = false;

// Create a queue for passing feedback data between tasks
QueueHandle_t feedbackQueue;
#define QUEUE_LENGTH 10
#define QUEUE_ITEM_SIZE sizeof(float)

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

// UART task - handles LED blinking, UART streaming, and UART receiving
void uartTask(void *parameter) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(500); // 500ms blink interval
  xLastWakeTime = xTaskGetTickCount();
  bool streamingStarted = false;
  int currentIndex = 0;
  
  // Initialize feedback queue
  feedbackQueue = xQueueCreate(QUEUE_LENGTH, QUEUE_ITEM_SIZE);
  
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
      
      // Check for incoming UART data from target
      if (Serial.available() > 0) {
        // Read from UART
        char inChar = Serial.read();
        
        // If newline is received, process the buffer
        if (inChar == '\n') {
          uartBuffer[uartBufferIndex] = '\0'; // Null terminate
          
          // Try to parse as float
          float receivedValue = atof(uartBuffer);
          
          Serial.print("Received feedback value: ");
          Serial.println(receivedValue);
          
          // Send to queue for I2C task to process
          if (feedbackQueue != NULL) {
            xQueueSend(feedbackQueue, &receivedValue, pdMS_TO_TICKS(10));
          }
          
          // Reset buffer
          uartBufferIndex = 0;
        } 
        else if (uartBufferIndex < UART_BUFFER_SIZE - 1) {
          // Add character to buffer
          uartBuffer[uartBufferIndex++] = inChar;
        }
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
  const TickType_t xFrequency = pdMS_TO_TICKS(100); // 100ms interval for more responsive feedback
  float feedbackData = 0.0;
  
  xLastWakeTime = xTaskGetTickCount();
  
  for(;;) {
    bool dataReceived = false;
    
    // Check if feedback data is available in the queue
    if (feedbackQueue != NULL) {
      if (xQueueReceive(feedbackQueue, &feedbackData, 0) == pdTRUE) {
        dataReceived = true;
        Serial.print("[I2C] Received feedback value: ");
        Serial.println(feedbackData);
      }
    }
    
    // Send data to I2C devices based on new feedback
    if (dataReceived) {
      // Map feedback value to haptic motor intensity
      // Assuming feedback is normalized between -1.0 and 1.0
      int hapticIntensity = abs(feedbackData * 100); // Scale to 0-100
      if (hapticIntensity > 100) hapticIntensity = 100;
      
      // Send to haptic motor driver
      Wire.beginTransmission(I2C_HAPTIC_ADDR);
      Wire.write(0x01); // Command byte (example: intensity register)
      Wire.write(hapticIntensity);
      uint8_t hapticResult = Wire.endTransmission();
      
      Serial.print("[I2C] Haptic intensity set to: ");
      Serial.print(hapticIntensity);
      Serial.print(" - Result: ");
      Serial.println((hapticResult == 0) ? "Success" : "Failed");
      
      // For speaker - different mapping logic
      // For example, positive values play one sound, negative values another
      Wire.beginTransmission(I2C_SPEAKER_ADDR);
      Wire.write(0x02); // Command byte (example: sound select register)
      
      if (feedbackData > 0) {
        // Positive feedback - select sound 1 with intensity proportional to value
        uint8_t soundIntensity = feedbackData * 127;
        Wire.write(0x01);  // Sound ID 1
        Wire.write(soundIntensity);
      } else if (feedbackData < 0) {
        // Negative feedback - select sound 2 with intensity proportional to abs value
        uint8_t soundIntensity = abs(feedbackData) * 127;
        Wire.write(0x02);  // Sound ID 2
        Wire.write(soundIntensity);
      } else {
        // Zero - mute
        Wire.write(0x00);  // No sound
        Wire.write(0x00);  // Zero intensity
      }
      
      uint8_t speakerResult = Wire.endTransmission();
      
      Serial.print("[I2C] Speaker command sent - Result: ");
      Serial.println((speakerResult == 0) ? "Success" : "Failed");
    }
    
    // Standard I2C operation for sending data to target (if needed)
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
        
        if (requestCount % 10 == 0) {  // Only print status every 10 cycles to reduce serial output
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
