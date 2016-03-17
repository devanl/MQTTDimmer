#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <PubSubClient.h>

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#define CHANNEL_COUNT 3

WiFiClient espClient;
PubSubClient client(espClient);
byte dim[CHANNEL_COUNT];
bool active[CHANNEL_COUNT];

#define DIM_TOPIC     "/dim"
#define ACTIVE_TOPIC  "/active"

#define ZERO_DETECT_PIN 13
byte pins[] = {4,5,12};

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40];
char mqtt_port[6] = "8080";
char blynk_token[34] = "YOUR_BLYNK_TOKEN";

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

int dimmer_ch_from_topic(char* topic) {
  String my_buffer = String(topic);
  byte ch_start, ch_end;

  ch_start = my_buffer.indexOf('/');  // ChipID
  if(ch_start != -1) {
    ch_start = my_buffer.indexOf('/', ch_start + 1);  // Channel ID
    if(ch_start != -1) {
      ch_start += 1;
      ch_end = my_buffer.lastIndexOf('/');
      if(ch_end != -1) {
        return atoi(String(topic).substring(ch_start, ch_end).c_str());
      }
    }
  }
  
//  for(byte ch_start = 1; ch_start < strlen(topic); ch_start++){
//    if(topic[ch_start - 1] == '/') {
//      for(byte ch_end = ch_start; ch_end < strlen(topic); ch_end++){
//        if(topic[ch_end] == '/') {
//          return atoi(String(topic).substring(ch_start, ch_end).c_str());
//        }
//      }
//    }
//  }
  return -1;
}

void dimmer_topic_from_ch(String &topic, int channel) {
  topic = "dimmer/";
  topic += ESP.getChipId();
  topic += "/";
  topic += channel;  
}

static char val_buff[4];
static byte val;
static char* action;
static byte ch_num;

void dimmer_callback(char* topic, unsigned char* payload, unsigned int length) {
  Serial.println("dimmer_callback()");
  Serial.print("\t topic: ");
  Serial.println(String((const char *)topic));
  Serial.print("\t payload: ");
  Serial.println(String((const char *)payload));
  Serial.print("\t length: ");
  Serial.println(length);

  ch_num = dimmer_ch_from_topic(topic);
  Serial.print("\t channel: ");
  Serial.println(ch_num);

  for(action = topic + strlen(topic); (action > topic) && (*action != '/'); action--);
  Serial.print("\t action: ");
  Serial.println(action);

  if(length > 3) length = 3;  // Assume max payload is '255'
  memset(val_buff, 0, sizeof(val_buff));
  memcpy(val_buff, payload, length);
  val = atoi(val_buff);

  if(strcmp(DIM_TOPIC, action) == 0) {
    Serial.print("Update DIM ");
    dim[ch_num] = val;
  } else if (strcmp(ACTIVE_TOPIC, action) == 0) {
    Serial.print("Update ACTIVE ");
    active[ch_num] = val;
  }
  Serial.println((const int)val);
  
}

inline void activate_channel(byte ch_num) {
  
}

inline void deactivate_channel(byte ch_num) {
  
}

volatile unsigned long last_zero;

void zero_hndlr() {
  last_zero = micros();
  //enable channels
  for(byte ch_num = 0; ch_num < CHANNEL_COUNT; ch_num++) {
    if(active[ch_num]) activate_channel(ch_num);
    else deactivate_channel(ch_num);
  } 
}

#define PWM_FACTOR_US ((1000*1000) / (60 * 256))

void manage_channels() {
  unsigned long next_time;
  unsigned long now = micros();
  
  for(byte ch_num = 0; ch_num < CHANNEL_COUNT; ch_num++) {
    next_time = last_zero + (dim[ch_num] * PWM_FACTOR_US);
    if(((last_zero <= next_time) && (now < next_time)) ||
       ((last_zero > next_time) && ((now < next_time) || (now >= last_zero)))) {
        if(active[ch_num]) activate_channel(ch_num);
       } else {
        deactivate_channel(ch_num);
       }
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println();

  //clean FS, for testing
  //SPIFFS.format();

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(blynk_token, json["blynk_token"]);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read



  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 5);
  WiFiManagerParameter custom_blynk_token("blynk", "blynk token", blynk_token, 32);

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set static ip
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
  
  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_blynk_token);

  //reset settings - for testing
  //wifiManager.resetSettings();

  //set minimu quality of signal so it ignores AP's under that quality
  //defaults to 8%
  //wifiManager.setMinimumSignalQuality();
  
  //sets timeout until configuration portal gets turned off
  //useful to make it all retry or go to sleep
  //in seconds
  //wifiManager.setTimeout(120);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect("AutoConnectAP", "password")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(blynk_token, custom_blynk_token.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["blynk_token"] = blynk_token;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save

    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
  }

  Serial.println("local ip");
  Serial.println(WiFi.localIP());

  client.setServer(mqtt_server, atoi(mqtt_port));
  client.setCallback(&dimmer_callback);
  
  attachInterrupt(digitalPinToInterrupt(ZERO_DETECT_PIN), zero_hndlr, CHANGE);
}

bool subscribe_dimmer(int ch_num) {
  String topic_name;
  dimmer_topic_from_ch(topic_name, ch_num);

  if(client.subscribe(String(topic_name + DIM_TOPIC).c_str(), 0)){
    Serial.println(String("Subscribed to topic: " + topic_name + DIM_TOPIC));
    if(client.subscribe(String(topic_name + ACTIVE_TOPIC).c_str(), 0))
      Serial.println(String("Subscribed to topic: " + topic_name + ACTIVE_TOPIC));
      return true;
  }

  return false;
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP8266Client")) {
      Serial.println("connected");

      for(char ch_num = 0; ch_num < CHANNEL_COUNT; ch_num++) {
        if(!subscribe_dimmer(ch_num)){
          Serial.print("Failed to subscribe to channel ");
          Serial.println(ch_num);
          client.disconnect();
          delay(5000);
          break;
        } else {
          client.loop();
        }
      }
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

long lastMsg = 0;

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  manage_channels();

  long now = millis();
  if (now - lastMsg > 5000) {
    lastMsg = now;

    for(char ch_num = 0; ch_num < CHANNEL_COUNT; ch_num++) {
      Serial.print("Channel: ");
      Serial.println((const int)ch_num);
      Serial.print("\tDIM: ");
      Serial.println((const int)dim[ch_num]);
      Serial.print("\tactive: ");
      Serial.println((const int)(active[ch_num]));
    }
    Serial.println(" ");

  }
}
