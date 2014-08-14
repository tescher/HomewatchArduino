
#include <SPI.h>
#include <Ethernet.h>
#include <OneWire.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <EEPROM.h>   ;*TWE++ 4/8/12 Read config from EEPROM rather than hard coded

#define MAX_SENSORS 4
#define REQUEST_KEY_MAGIC 271
#define fileKey 12345   //Secret for filing data
#define ONE_WIRE_PIN 9  //Where the OneWire bus is connected *TWE 4/8/12 - Changed to pin 9 (Ethernet board uses pin 10).
#define WD_INTERVAL 500  //Milliseconds between each Watchdog Timer reset
#define CODE_LOG_LOC 100   //Location in the EEPROM where to store the current execution point

// Default config info in case nothing is in the EEPROM
byte mac[] = { 
  0x90, 0xA2, 0xDA, 0x00, 0x30, 0x7D };
char server[25] = "www.escherhomewatch.com";   // Up to 24 characters for the server name
char controller[30] = "BasementArduino";  //ID for this sensor controller, up to 29 chars
int last_code_log;

int sensorCount = 0;
boolean haveOneWire = false;  // Flag to say that we should be getting data from One-Wire sensors
boolean int_is_seconds = false;
EthernetClient client;
int minInterval=32767;     //Minimum measurement interval from our sensor list. This will be used for everyone.
OneWire ds(ONE_WIRE_PIN);
struct sensorConfig {
  int id;
  unsigned int addressH;
  unsigned int addressL;
  unsigned int interval;
  unsigned int type;      // 0-generic, 1-OneWIre
};
struct sensorConfig sensors[MAX_SENSORS];

// Byte array string compare

bool bComp(char* a1, char* a2) {
  for(int i=0; ; i++) {
    if ((a1[i]==0x00)&&(a2[i]==0x00)) return true;
    if(a1[i]!=a2[i]) return false;
  }
}

// Delay function with watchdog resets

void delay_with_wd(int ms) {
  int loop_count = ms / WD_INTERVAL;
  int leftover = ms % WD_INTERVAL;

  for (int i=0; i < loop_count; i++) {
    delay(WD_INTERVAL);
    wdt_reset();
  }
  delay(leftover);
  wdt_reset();
}



// Get the EEPROM config info

void getEEPROM() {
  // First 2 bytes should be 0xFE 0xEF to signal that we have data in here:
  if ((EEPROM.read(0) << 8) + EEPROM.read(1) == 0xFEEF) {
    int i = 0;
    char c = ' ';

    //Next 6 bytes is the MAC address
    for (i=2; i<8; i++) {
      mac[i-2] = EEPROM.read(i);
    }

    //Next batch of bytes (until 0x00) is the server in ASCII
    do {
      c = EEPROM.read(i++);
      server[i-9] = c;
    } 
    while (c != 0x00);

    //Next batch of bytes (until 0x00) is the controller ID
    int current_ptr = i;
    do { 
      c = EEPROM.read(i++);
      controller[i - current_ptr - 1] = c;
    }  
    while (c != 0x00);
  }
}

// Store a marker where we are so when we restart we can log where we hung up

void code_log(byte location) {
  EEPROM.write(CODE_LOG_LOC, location);
}

int get_code_log() {
  return (EEPROM.read(CODE_LOG_LOC) & 0x00FF);
}
  
// Read value from a DS OneWire sensor
float getDSValue(OneWire ds, byte addr[8]) {
  byte i;
  byte present = 0;
  byte type_s;
  byte data[12];
  float celsius, fahrenheit;

  // the first ROM byte indicates which chip
  switch (addr[0]) {
  case 0x10:
    // Serial.println("  Chip = DS18S20");  // or old DS1820
    type_s = 1;
    break;
  case 0x28:
    // Serial.println("  Chip = DS18B20");
    type_s = 0;
    break;
  case 0x22:
    // Serial.println("  Chip = DS1822");
    type_s = 0;
    break;
  default:
    Serial.println("Device is not a DS18x20 family device.");
    return 0;
  } 

  ds.reset();
  ds.select(addr);
  ds.write(0x44,1);         // start conversion, with parasite power on at the end

  delay_with_wd(1000);     // maybe 750ms is enough, maybe not

  present = ds.reset();
  ds.select(addr);    
  ds.write(0xBE);         // Read Scratchpad

  // Serial.print("  Data = ");
  // Serial.print(present,HEX);
  // Serial.print(" ");
  for ( i = 0; i < 9; i++) {           // we need 9 bytes
    data[i] = ds.read();
    // Serial.print(data[i], HEX);
    // Serial.print(" ");
  }
  // Serial.print(" CRC=");
  // Serial.print(OneWire::crc8(data, 8), HEX);
  Serial.println();

  // convert the data to actual temperature

  // unsigned int raw = (data[1] << 8) | data[0];
  int raw = (data[1] << 8) | data[0];
  if (type_s) {
    raw = raw << 3; // 9 bit resolution default
    if (data[7] == 0x10) {
      // count remain gives full 12 bit resolution
      raw = (raw & 0xFFF0) + 12 - data[6];
    }
  } 
  else {
    byte cfg = (data[4] & 0x60);
    if (cfg == 0x00) raw = raw << 3;  // 9 bit resolution, 93.75 ms
    else if (cfg == 0x20) raw = raw << 2; // 10 bit res, 187.5 ms
    else if (cfg == 0x40) raw = raw << 1; // 11 bit res, 375 ms
    // default is 12 bit resolution, 750 ms conversion time
  }
  celsius = (float)raw / 16.0;
  fahrenheit = celsius * 1.8 + 32.0;
  // Serial.print("  Temperature = ");
  // Serial.print(celsius);
  // Serial.print(" Celsius, ");
  // Serial.print(fahrenheit);
  // Serial.println(" Fahrenheit");
  return fahrenheit;
}

// Send value found to the DB
void sendValue(int sensorID, float value) {
  byte fail = 0;
  char buf[20];

  // Serial.print("Filing data to ");
  // Serial.println(server);

  // while (Ethernet.begin(mac) == 0) {
  //  Serial.println("Failed to configure Ethernet using DHCP");
  //  Serial.println("retrying...");
  //  delay(2000);  // No watchdog reset here, want to reset the board if it fails repeatedly
  //}

  code_log(15);
  delay_with_wd(1000);

  if (client.connect(server, 80)) {
    // Serial.println("connected");
    code_log(16);

    client.print("POST http://");
    Serial.print("POST http://");
    client.print(server);
    Serial.print(server);
    client.print("/measurements?sensor_id=");
    Serial.print("/measurements?sensor_id=");
    client.print(sensorID);
    Serial.print(sensorID);
    dtostrf(value, 3, 1, buf);
    client.print("&value=");
    Serial.print("&value=");
    client.print(buf);
    Serial.print(buf);
    client.print("&key=");
    Serial.print("&key=");
    client.print(request_key(buf));
    Serial.print(request_key(buf));
    client.println(" HTTP/1.0");
    Serial.println(" HTTP/1.0");
    client.print("Host: ");
    Serial.print("Host: ");
    client.println(server);
    Serial.println(server);
    client.println("Content-Length: 0");
    Serial.println("Content-Length: 0");
    client.println();
    Serial.println();
  } 
  else {
    // if you didn't get a connection to the server:
    code_log(17);
    Serial.println("connection failed");
    for(;;)
      ;
  }

  code_log(18);
  while ((!client.available()) && (fail < 500)) {
    delay_with_wd(100);
    fail++;
  }

  code_log(19);
  if (fail >= 500) {
    for(;;)
      ;
  }    
  // while(!client.available()){
  //   delay_with_wd(1);
  // }

  code_log(20);
  bool success = false;
  while (client.connected()) {  // Check for success on file. If fails, reset
    int msg_cnt = 0;
    char msg[8] = "success";
    code_log(21);
    while(client.available()) {
      char c = client.read();
      if (c == msg[msg_cnt++]) {
        Serial.print(c);
        if (msg_cnt == 7) {
          success = true;
          Serial.println("Success!");
        }
      } else {
        msg_cnt = 0;
      }
      Serial.print(c);
    }
  }
  if (success == false) {
    code_log(22);
    for(;;)
      ;
  }


  // while(client.connected()){
  //   Serial.println("Waiting for server to disconnect");
  // }

  code_log(23);
  client.stop();
}

void setup() {
  // start the serial library:
  Serial.begin(9600);
  
  // Find out where we crashed
  last_code_log = get_code_log();

  // Get the connection config values
  getEEPROM();

  // Start the watchdog
  wdt_enable(WDTO_8S);

  // start the Ethernet connection, try up to 5 times
  Serial.println("Attempting to configure Ethernet...");
  code_log(1);
  int eth_retry = 0;
  while ((Ethernet.begin(mac) == 0) && (eth_retry < 6)) {
    Serial.println("Failed to configure Ethernet using DHCP");
    Serial.println("retrying...");
    delay_with_wd(5000);
    eth_retry++;
  }
  if (eth_retry > 5) {
    code_log(24);
    for (;;)
      ;
   }
  
  // give the Ethernet shield a second to initialize:
  delay_with_wd(1000);

  // Get the sensor config information
  Serial.println("connecting...");

  if (client.connect(server, 80)) {
    code_log(2);
    // Serial.println("connected");

    client.print("GET ");
    Serial.print("GET ");
    client.print("/sensors/getconfig?cntrl=");
    Serial.print("/sensors/getconfig?cntrl=");
    client.print(controller);
    Serial.print(controller);
    client.print("&key=");
    Serial.print("&key=");
    client.print(request_key(controller));
    Serial.print(request_key(controller));
    client.print("&log=Code%20restarted%20from%20location%20");
    Serial.print("&log=Code%20restarted%20from%20location%20");
    client.print(last_code_log);
    Serial.print(last_code_log);
    client.println(" HTTP/1.0");
    Serial.println(" HTTP/1.0");
    client.print("Host: ");
    Serial.print("Host: ");
    client.println(server);
    Serial.println(server);
    client.println();
  } 
  else {
    code_log(3);
    // if you didn't get a connection to the server:
    Serial.println("connection failed");
    for(;;)
      ;
  }

  bool jsonStarted = false;
  bool objectStarted = false;
  bool keyStarted = false;
  bool haveKey = false;
  bool valueStarted = false;
  bool stringStarted = false;
  bool numStarted = false;
  char key[20];
  char value[20];
  int i = 0;
  while (client.connected()) {
    code_log(4);
    if (client.available()) {
      code_log(5);
      char c = client.read();
      Serial.print(c);
      if (!jsonStarted && (c == '[')) {
        jsonStarted = true;
      } 
      else if (jsonStarted && !objectStarted && (c == '{')) {
        objectStarted = true;
        sensorCount += 1;
        Serial.println(sensorCount);
      } 
      else if (objectStarted && !keyStarted && (c == '"')) {
        keyStarted = true;
        i = 0;
        Serial.println("keyStart");
      } 
      else if (keyStarted && !haveKey && (c != '"')) {
        key[i++] = c;
      } 
      else if (keyStarted && !haveKey && (c == '"')) {
        haveKey = true;
        key[i] = 0x00;
        Serial.println("haveKey");
      } 
      else if (haveKey && !valueStarted && (c != ' ') && (c != ':')) {
        valueStarted = true;
        Serial.println("valueStart");
        if (c != '"') {
          numStarted = true;
          i = 0;
          value[i++] = c;
        } 
        else {
          stringStarted = true;
          i = 0;
        }
      } 
      else if (valueStarted) {
        if ((stringStarted && (c == '"')) || (numStarted && ((c == ',') || (c == '}')))) {
          value[i] = 0x00;
          valueStarted = false;
          haveKey = false;
          stringStarted = false;
          if (numStarted && (c == '}')) objectStarted = false;
          numStarted = false;
          keyStarted = false;
          Serial.println("valueDone");
          if (bComp(key,"id")) sensors[sensorCount-1].id = atoi(value);
          else if (bComp(key,"addressH")) sensors[sensorCount-1].addressH = atoi(value);
          else if (bComp(key,"addressL")) sensors[sensorCount-1].addressL = atoi(value);
          else if (bComp(key,"interval")) {
            if (value[i-1] == 's') {
              int_is_seconds = true;
              value[i-1] = 0x00;
            }
            sensors[sensorCount-1].interval = atoi(value);
          }
          else if (bComp(key,"type") && (value[0] == 'd') && (value[1] == 's')) {
            haveOneWire = true;
            sensors[sensorCount-1].type = 1; 
          } 
        }
        else value[i++] = c;
      } 
      else if (objectStarted & !keyStarted && (c == '}')) {
        objectStarted = false;
      } 
      else if (jsonStarted && !objectStarted && (c == ']')) {
        jsonStarted = false;
      }
    } 
    else {
      code_log(6);
      Serial.println("No more data, waiting for server to disconnect");
      delay_with_wd(1000);
    }
  }

  while (client.available()) {
    code_log(7);
    char c = client.read();  //Just clean up anything left
    Serial.print(c);
  }
  
  code_log(8);
  client.stop();
  delay_with_wd(1000);
  
  if (sensorCount < 1) {
    code_log(9);
    Serial.println("No sensors received");
        for(;;)
      ;
  }
 

  // Parse the sensor info
  // configInput.toCharArray(buff, 1000);
  for (i = 0; i < sensorCount; i++) {
    if ((sensors[i].interval > 0) && (sensors[i].interval < minInterval)) {
      minInterval = sensors[i].interval;
    }
    Serial.print(sensors[i].id);
    Serial.print(":");
    Serial.print(sensors[i].addressH);
    Serial.print(":");
    Serial.print(sensors[i].addressL);
    Serial.print(":");
    Serial.println(sensors[i].interval);
    Serial.print(":");
    Serial.println(sensors[i].type);
    
  }
}

void querydsSensors(OneWire ds) {
  // Deal with any DS18S20 sensors connected
  byte addr[8];   
  float value;
  unsigned int addrL, addrH;

  while (ds.search(addr)) {
    code_log(10);
    bool found = false;
    addrL = (addr[1] << 8) | addr[0];
    addrH = (addr[3] << 8) | addr[2];
    Serial.print("AddrL: ");
    Serial.print(addrL);
    Serial.print(" AddrH: ");
    Serial.println(addrH);
    for (int i=0; (i < sensorCount) && !found; i++) {  //Find the matching config
      if ((sensors[i].type == 1) && (sensors[i].addressL == addrL) && (sensors[i].addressH == addrH)) {  //Found our config
        value = getDSValue(ds, addr);
        Serial.print("Value: ");
        Serial.println(value);
        Serial.print("Sensor ID: ");
        Serial.println(sensors[i].id);
        if (value < 180) {  // Skip spurious startup values
          sendValue(sensors[i].id, value);
        }
        found = true;
        wdt_reset();
      }
    }
  }
}

void loop()
{
  // Serial.println("Free Memory: "+String(freeMemory()));

  if (haveOneWire) {
    code_log(11);
    querydsSensors(ds);
    ds.reset_search();
  }
  
  code_log(12);
  for (int i=0; (i < sensorCount); i++) {
    if (sensors[i].type != 1) {
      float value = analogRead(sensors[i].addressL);   //Pin specified in addressL
      Serial.print("Value: ");
      Serial.println(value);
      Serial.print("Sensor ID: ");
      Serial.println(sensors[i].id);
      sendValue(sensors[i].id, value);
      wdt_reset();
    }
  }  
  
  

  if (int_is_seconds) {
    code_log(13);
    for (int i=0; i < minInterval; i++) {
      delay_with_wd(1000);
      Serial.println(i);
    }
  } else {
    code_log(14);
    delay_with_wd(minInterval);
  }
}

unsigned int request_key(char *str)
{
  int key = 0;
  for (int i=0; str[i] != 0x00; i++) {
    key += str[i];
  }
  return key * REQUEST_KEY_MAGIC;
}

