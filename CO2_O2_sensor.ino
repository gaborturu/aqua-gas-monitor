// Aqua Gas Monitor
// Monitors CO₂ and O₂ levels in an aquarium using NodeMCU ESP32 and DFRobot sensors
// Version 1.0

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DFRobot_OxygenSensor.h>
#include <DFRobot_SCD4X.h>
#include "secrets.h" // Contains Wi-Fi and MQTT credentials

// Wi-Fi and MQTT settings
const int g_mqttPort = 1883;

// Device information
const char* g_deviceModel = "Aqua_gas_sensor";
const char* g_swVersion = "1.0";
const char* g_manufacturer = "hax47";
String g_deviceName = "AGS";
String g_mqttStatusTopic = "iotsensor/" + g_deviceName;

// WiFi and MQTT clients
WiFiClient g_WiFiClient;
PubSubClient g_mqttPubSub(g_WiFiClient);

String g_UniqueId;
int g_mqttCounterConn = 0;

// Sensor-specific variables
#define COLLECT_NUMBER 20 
#define FLASH_BUTTON_PIN 0 // GPIO pin connected to the calibration button

bool buttonState = HIGH;      // Current state of the button
bool lastButtonState = HIGH;  // Previous state of the button

DFRobot_OxygenSensor g_OxygenSensor;

unsigned long g_ElapsedTimeI2C = 0;
unsigned long g_ElapsedTimeSendData = 0;

float g_O2 = 0;
uint16_t g_CO2 = 0;
float g_temp = 0;
float g_humidity = 0;

DFRobot_SCD4X SCD4X(&Wire, /*i2cAddr = */SCD4X_I2C_ADDR);

void setup() {
    // Initialize Serial port
    Serial.begin(115200);
    Serial.println("Starting up...");

    // Initialize the calibration button pin
    pinMode(FLASH_BUTTON_PIN, INPUT_PULLUP);

    // Display device information
    Serial.println("----------------------------------------------");
    Serial.print("MODEL: ");
    Serial.println(g_deviceModel);
    Serial.print("DEVICE: ");
    Serial.println(g_deviceName);
    Serial.print("SW Rev: ");
    Serial.println(g_swVersion);
    Serial.println("----------------------------------------------");

    // Initialize Wi-Fi and MQTT
    setup_wifi();
    g_mqttPubSub.setServer(g_mqtt_server, g_mqttPort);
    g_mqttPubSub.setCallback(MqttReceiverCallback);
    
    // Initialize Oxygen Sensor
    Serial.println("Initializing O₂ sensor...");
    while(!g_OxygenSensor.begin(ADDRESS_2)){
        Serial.println("O₂ sensor connection error!");
        delay(1000);
    }
    Serial.println("O₂ sensor connected!");

    // Initialize CO₂ Sensor
    Serial.println("Initializing CO₂ sensor...");
    while(!SCD4X.begin()){
        Serial.println("Communication with CO₂ device failed");
        delay(3000);
    }
    Serial.println("CO₂ sensor connected!");

    // Set CO₂ sensor parameters
    SCD4X.enablePeriodMeasure(SCD4X_STOP_PERIODIC_MEASURE);
    SCD4X.setTempComp(4.0);       // Set temperature compensation
    SCD4X.setSensorAltitude(102); // Set altitude in meters
    uint16_t altitude = SCD4X.getSensorAltitude();
    Serial.print("Set the current environment altitude: ");
    Serial.print(altitude);
    Serial.println(" m");
    
    SCD4X.setAutoCalibMode(false); // Disable automatic calibration
    if(SCD4X.getAutoCalibMode()) {
        Serial.println("Automatic calibration on!");
    } else {
        Serial.println("Automatic calibration off!");
    }

    SCD4X.enablePeriodMeasure(SCD4X_START_PERIODIC_MEASURE);
    Serial.println("Sensors initialized.\n");
}

void loop() {
    // Calibration connected to flash button

    // Read the button state
    buttonState = digitalRead(FLASH_BUTTON_PIN);

    // Check if the button was pressed (state changed from HIGH to LOW)
    if (buttonState == LOW && lastButtonState == HIGH) {
        // Debounce delay
        delay(50);

        // Confirm button is still pressed
        if (digitalRead(FLASH_BUTTON_PIN) == LOW) {
            // Start calibration
            CalibrateSensors();
        }
    }
    
    // Save the current button state as the last state for the next loop iteration
    lastButtonState = buttonState;

    // Handle Wi-Fi and MQTT connection
    if (WiFi.status() == WL_CONNECTED) {
        if (!g_mqttPubSub.connected()) {
            MqttReconnect();
        } else {
            g_mqttPubSub.loop();
        }
    }

    // Read sensor data every 3 seconds
    if (millis() - g_ElapsedTimeI2C > 3000) {
        g_ElapsedTimeI2C = millis();

        // Read Oxygen sensor data
        g_O2 = g_OxygenSensor.getOxygenData(COLLECT_NUMBER);
        
        // Read CO₂ sensor data
        if(SCD4X.getDataReadyStatus()) {
            DFRobot_SCD4X::sSensorMeasurement_t data;
            SCD4X.readMeasurement(&data);

            g_CO2 = data.CO2ppm;
            g_temp = data.temp;
            g_humidity = data.humidity;
        }
    }

    // Send sensor data every 10 seconds
    if (millis() - g_ElapsedTimeSendData > 10000) {
        g_ElapsedTimeSendData = millis();

        // Print measured data to Serial
        Serial.println("----------- Data Measured -----------");
        Serial.print("Oxygen Concentration: ");
        Serial.print(g_O2);        
        Serial.println(" %");

        Serial.print("CO₂ Concentration: ");
        Serial.print(g_CO2);
        Serial.println(" ppm");

        Serial.print("Humidity: ");
        Serial.print(g_humidity);
        Serial.println(" %");

        Serial.print("Temperature: ");
        Serial.print(g_temp);
        Serial.println(" °C");
        Serial.println("-------------------------------------\n");

        // Prepare JSON payload
        StaticJsonDocument<300> payload;
        payload["temp_2"] = Round2(g_temp);
        payload["oxy_2"] = Round2(g_O2);
        payload["humi_2"] = Round2(g_humidity);
        payload["co2_2"] = g_CO2;

        String strPayload;
        serializeJson(payload, strPayload);

        // Publish data via MQTT
        if (g_mqttPubSub.connected()) {
            g_mqttPubSub.publish(g_mqttStatusTopic.c_str(), strPayload.c_str());
            Serial.println("MQTT: Data sent.");
            Serial.println();
        }
    }
}

void setup_wifi() {
    int counter = 0;
    byte mac[6];

    Serial.print("Connecting to Wi-Fi network: ");
    Serial.println(g_ssid);

    WiFi.begin(g_ssid, g_password);

    // Generate unique ID from MAC address
    WiFi.macAddress(mac);
    g_UniqueId = "";
    for (int i = 0; i < 6; ++i) {
        if (mac[i] < 16) g_UniqueId += "0";
        g_UniqueId += String(mac[i], HEX);
    }

    Serial.print("Unique ID: ");
    Serial.println(g_UniqueId);

    // Wait for connection
    while (WiFi.status() != WL_CONNECTED && counter++ < 8) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi connected.");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("Wi-Fi connection failed.");
    }
}

void MqttReconnect() {
    // Attempt to connect to MQTT broker
    while (!g_mqttPubSub.connected() && (g_mqttCounterConn++ < 4)) {
        Serial.print("Attempting MQTT connection...");
        if (g_mqttPubSub.connect(g_deviceName.c_str(), g_mqttUser, g_mqttPsw)) {
            Serial.println("connected");
            // No need to subscribe to any topics
            delay(100);
        } else {
            Serial.print("failed, rc=");
            Serial.print(g_mqttPubSub.state());
            Serial.println(" trying again in 1 second");
            delay(1000);
        }
    }
    g_mqttCounterConn = 0;
}

void MqttReceiverCallback(char* topic, byte* payload, unsigned int length) {
    // No action needed since we're not processing incoming messages
}

float Round2(float value) {
    // Round to two decimal places
    return roundf(value * 100) / 100.0;
}

void CalibrateSensors() {
    Serial.println("Calibration started.");

    if (!g_mqttPubSub.connected())
        MqttReconnect();
    
    // Notify calibration start
    g_mqttPubSub.publish(g_mqttStatusTopic.c_str(), "{\"calibration\":1}");

    // Wait for 2 minutes to allow the sensors to stabilize
    delay(2 * 60 * 1000);

    // Calibrate Oxygen sensor to 20.9% (ambient air)
    g_OxygenSensor.calibrate(20.9);
    
    // Calibrate CO₂ sensor to 400 ppm (ambient air)
    SCD4X.enablePeriodMeasure(SCD4X_STOP_PERIODIC_MEASURE);
    SCD4X.performForcedRecalibration(400);
    SCD4X.enablePeriodMeasure(SCD4X_START_PERIODIC_MEASURE);

    if (!g_mqttPubSub.connected())
        MqttReconnect();
    
    // Notify calibration end
    g_mqttPubSub.publish(g_mqttStatusTopic.c_str(), "{\"calibration\":0}");
    Serial.println("Calibration completed.");
}
