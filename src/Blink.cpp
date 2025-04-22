/* Blink Example
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <Arduino.h>

// Define the GPIO for blinking - using CONFIG_BLINK_GPIO from build flags
#ifndef CONFIG_BLINK_GPIO
#define CONFIG_BLINK_GPIO 2  // Default to GPIO2 (D4 on NodeMCU)
#endif

#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // Default built-in LED pin for ESP8266
#endif

void setup() {
    Serial.begin(115200);
    Serial.println("ESP8266 Blink Example");
    
    // Setup LED pins
    pinMode(CONFIG_BLINK_GPIO, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    // Blink the configured GPIO
    digitalWrite(CONFIG_BLINK_GPIO, HIGH);
    delay(500);
    digitalWrite(CONFIG_BLINK_GPIO, LOW);
    
    // Blink built-in LED with opposite pattern
    digitalWrite(LED_BUILTIN, HIGH);  // Note: Some ESP8266 boards use inverted logic (LOW=ON)
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    
    Serial.println("Hello from ESP8266!");
    delay(500);
}
