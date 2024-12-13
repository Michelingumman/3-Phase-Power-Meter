#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>
#include <HTTPClient.h>
#include <secrets.h>
#include <string.h>
#include <ArduinoJson.h>
#include <EmonLib.h>


//Lolin 32 Lite pinout and specs
//https://mischianti.org/esp32-wemos-lolin32-lite-high-resolution-pinout-and-specs/


const char* SSID = WIFI_SSID;
const char* PASSWORD = WIFI_PASSWORD;

const char* mqtt_server = MQTT_SERVER; // Home Asistant IP
const int mqqt_port = BROKER_PORT;
const char* brokerUser = BROKER_USER;
const char* brokerPass = BROKER_PASS;
const char* broker = BROKER;

const char *api_url = "https://mgrey.se/espot?format=json";

// pin definitions
#define CURRENT_PHASE1 36
#define CURRENT_PHASE2 39
#define CURRENT_PHASE3 35

#define VOLT_PHASE1 32
#define VOLT_PHASE2 33
#define VOLT_PHASE3 34



//objects

WiFiClient espClient;
PubSubClient client(espClient);

EnergyMonitor emon_p1; // e monitor phase 1
EnergyMonitor emon_p2; // e monitor phase 2
EnergyMonitor emon_p3; // e monitor phase 3


unsigned long startMillis;
unsigned long currentMillis;




// put function declarations here:

boolean has_it_been_minutes(int seconds);
void send_data_via_MQTT(void);
void get_readings();
float get_current_reading(int CurrentPin);
float calculatePhaseShift(float voltage, float current);
void setupWifi();
void mqtt_reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void fetchAPIData();
void extract_spot_price_Values(String json, String area_key, String hour_key, String price_key);






// setup code here
void setup(){
  Serial.begin(9600);
  Serial.println("Starting...");
  setupWifi();
  client.setServer(mqtt_server, mqqt_port);


  startMillis = millis();
  currentMillis = startMillis;

  emon_p1.voltage(VOLT_PHASE1, 190, 2.7); // Voltage: input pin, calibration, phase_shift
  emon_p1.current(CURRENT_PHASE1, 1.2); // Current: input pin, calibration.

  emon_p2.voltage(VOLT_PHASE2, 190, 2.7);
  emon_p2.current(CURRENT_PHASE2, 1.2); 

  emon_p3.voltage(VOLT_PHASE3, 200, 2.7);
  emon_p3.current(CURRENT_PHASE3, 1.2); 
}


typedef struct power{
  float Phase1;
  float Phase2;
  float Phase3;
  float Total;
  float TotalActive;
  float TotalRecative;
  float PowerFactorR;
  float PowerFactorS;
  float PowerFactorT;
} Power;

Power power = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

float volt_Phase1 = 0.0;
float volt_Phase2 = 0.0;
float volt_Phase3 = 0.0;

float current_Phase1 = 0.0;
float current_Phase2 = 0.0;
float current_Phase3 = 0.0;

float phaseShift = 0.0;

float hour = 0.0;
float previous_hour = 0.0;
float spot_price = 0.0;
float k_watt_hours = 0.0;
float k_watt_hours_per_day = 0.0;
float day_cost = 0.0;




int kwh_count = 0;

// main loop here
void loop(){
  if (!client.connected()) {
    mqtt_reconnect();
  }
  client.loop();


  emon_p1.calcVI(20, 2000);
  emon_p2.calcVI(20, 2000);
  emon_p3.calcVI(20, 2000);
  
  Serial.print("\nApparent power:\n");
  Serial.print(emon_p1.apparentPower);
  Serial.print(" | ");
  Serial.print(emon_p2.apparentPower);
  Serial.print(" | ");
  Serial.println(emon_p3.apparentPower);

  Serial.print("\n\n\n\nActive power:\n");
  Serial.print(emon_p1.realPower);
  Serial.print(" | ");
  Serial.print(emon_p2.realPower);
  Serial.print(" | ");
  Serial.println(emon_p3.realPower);


  Serial.print("\nPF:\n");
  Serial.print(emon_p1.powerFactor);
  Serial.print(" | ");
  Serial.print(emon_p2.powerFactor);
  Serial.print(" | ");
  Serial.println(emon_p3.powerFactor);

  Serial.print("\nVrms:\n");
  Serial.print(emon_p1.Vrms);
  Serial.print(" | ");
  Serial.print(emon_p2.Vrms);
  Serial.print(" | ");
  Serial.println(emon_p3.Vrms);

  Serial.print("\nIrms:\n");
  Serial.print(emon_p1.Irms);
  Serial.print(" | ");
  Serial.print(emon_p2.Irms);
  Serial.print(" | ");
  Serial.println(emon_p3.Irms);
  Serial.println("_____________________________");

power.TotalActive = emon_p1.realPower + emon_p2.realPower + emon_p3.realPower;
power.Total = emon_p1.apparentPower + emon_p2.apparentPower + emon_p3.apparentPower;

float p1_reactive = sqrt(pow(emon_p1.apparentPower, 2) - pow(emon_p1.realPower, 2));
float p2_reactive = sqrt(pow(emon_p2.apparentPower, 2) - pow(emon_p2.realPower, 2));
float p3_reactive = sqrt(pow(emon_p3.apparentPower, 2) - pow(emon_p3.realPower, 2));
power.TotalRecative = p1_reactive + p2_reactive + p3_reactive;





  // check for if 15 min has been if so then send spot prices etc
  if(has_it_been_minutes(15) == true) {
    fetchAPIData(); //update value for every 5 minuts for safety since 60 minuts would be close to the chagning point
    if(previous_hour != hour && previous_hour < 23){ // if a a new hour is detected then add that to the total cost per day
      day_cost = day_cost + (0.01 * spot_price * k_watt_hours);
      previous_hour = hour;
      client.publish("Home-Power/energy/day_cost", String(day_cost).c_str(), true); // update day cost for every hour
      client.publish("Home-Power/energy/spot_price", String(spot_price).c_str(), true);
    }
    else if(previous_hour != hour && previous_hour == 23){ // if a new day is detected then reset the daily cost
      day_cost = 0.0;
      previous_hour = hour;
      client.publish("Home-Power/energy/day_cost", String(day_cost).c_str(), true); // update day cost for every hour
      client.publish("Home-Power/energy/spot_price", String(spot_price).c_str(), true);
    }
  }



  // send emon power readings via MQTT
  send_data_via_MQTT();

}




boolean has_it_been_minutes(int minutes){
  currentMillis = millis();
  if(currentMillis - startMillis >= minutes * 1000 * 60){
    startMillis = currentMillis;
    return true;
  }
  else{
    return false;
  }
}





void send_data_via_MQTT(void){
  //voltage
  client.publish("Home-Power/volt/Phase1", String(emon_p1.Vrms).c_str(), true);
  client.publish("Home-Power/volt/Phase2", String(emon_p2.Vrms).c_str(), true);
  client.publish("Home-Power/volt/Phase3", String(emon_p3.Vrms).c_str(), true);

  //current
  client.publish("Home-Power/current/Phase1", String(emon_p1.Irms).c_str(), true);
  client.publish("Home-Power/current/Phase2", String(emon_p2.Irms).c_str(), true);
  client.publish("Home-Power/current/Phase3", String(emon_p3.Irms).c_str(), true);

  //power
  client.publish("Home-Power/power/Phase1", String(emon_p1.apparentPower).c_str(), true);
  client.publish("Home-Power/power/Phase2", String(emon_p2.apparentPower).c_str(), true);
  client.publish("Home-Power/power/Phase3", String(emon_p3.apparentPower).c_str(), true);
  
  client.publish("Home-Power/power/Total", String(power.Total).c_str(), true);
  client.publish("Home-Power/power/Total_active", String(power.TotalActive).c_str(), true);
  client.publish("Home-Power/power/Total_reactive", String(power.TotalRecative).c_str(), true);

  //PF
  client.publish("Home-Power/power/Power_factorR", String(emon_p1.powerFactor*100).c_str(), true);
  client.publish("Home-Power/power/Power_factorS", String(emon_p2.powerFactor*100).c_str(), true);
  client.publish("Home-Power/power/Power_factorT", String(emon_p3.powerFactor*100).c_str(), true);

}





void mqtt_reconnect(){
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.println("\nAttempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP32Client", brokerUser, brokerPass)) {
      client.subscribe("InTopic");
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}






void setupWifi() {
  delay(100);
  Serial.println("\nConnecting to");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while(WiFi.status() != WL_CONNECTED){
    delay(100);
    Serial.print("-");
  }
  Serial.println("\nConnected to WiFi");
  Serial.println(WiFi.localIP());
}





void callback(char* topic, byte* payload, unsigned int length){
  payload[length] = '\0';
  String message = (char*)payload;
  Serial.println(message);
}






void fetchAPIData() {
  HTTPClient http;

  // Send GET request
  http.begin(api_url);  // Specify the URL
  int httpCode = http.GET();

  if (httpCode > 0) {  // Check for a successful response
    Serial.printf("\n\n\n\nHTTP GET request code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Received payload:");
      Serial.println(payload);
      Serial.println("\n");


      //updates the hour and the spot price for the choosen area
      extract_spot_price_Values(payload, "SE3", "hour", "price_sek");
      
      Serial.print("Hour: ");
      Serial.println(hour);
      Serial.print("Price in Öre: ");
      Serial.println(spot_price);



    }

  } else {
    Serial.println("Error in HTTP GET request");
  }

  http.end();  // Close connection

}





// Function to extract a value from JSON string based on key
void extract_spot_price_Values(String json, String area_key, String hour_key, String price_key) {

  // Parse JSON data
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.print("Error parsing JSON: ");
    Serial.println(error.c_str());
    return;
  }

  // Extract data for SE3
  JsonArray Data = doc[area_key];
  if (Data.isNull() || Data.size() == 0) {
    Serial.print("No data found for ");
    Serial.println(area_key);
    return;
  }

  // Extract hour and price_sek values from the first entry of SE3
  JsonObject Entry = Data[0];
  hour = Entry[hour_key];
  spot_price = Entry[price_key];

}