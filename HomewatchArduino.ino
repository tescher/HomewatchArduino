
#include <SPI.h>
#include <Ethernet.h>
#include <OneWire.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <EEPROM.h>   ;*TWE++ 4/8/12 Read config from EEPROM rather than hard coded

// Default config info in case nothing is in the EEPROM
byte mac[] = { 
  0x90, 0xA2, 0xDA, 0x00, 0x30, 0x7D };
char server[25] = "www.escherhomewatch.com";   // Up to 24 characters for the server name
char controller[30] = "BasementArduino";  //ID for this sensor controller, up to 29 chars


const int MAX_SENSORS = 2;
const int REQUEST_KEY_MAGIC = 271;
int sensorCount = 0;
EthernetClient client;
int minInterval=32767;     //Minimum measurement interval from our sensor list. This will be used for everyone.
unsigned int fileKey = 12345;   //Secret for filing data
const int ONE_WIRE_PIN = 9;  //Where the OneWire bus is connected *TWE 4/8/12 - Changed to pin 9 (Ethernet board uses pin 10).
OneWire ds(ONE_WIRE_PIN);
const int WD_INTERVAL = 500;  //Milliseconds between each Watchdog Timer reset
struct sensorConfig {
  int id;
  unsigned int addressH;
  unsigned int addressL;
  unsigned int interval;
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

  for (int i; i < loop_count; i++) {
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
    Serial.println("  Chip = DS18S20");  // or old DS1820
    type_s = 1;
    break;
  case 0x28:
    Serial.println("  Chip = DS18B20");
    type_s = 0;
    break;
  case 0x22:
    Serial.println("  Chip = DS1822");
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
  // Serial.println();

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

  delay_with_wd(1000);

  if (client.connect(server, 80)) {
    // Serial.println("connected");

    client.print("POST http://");
    Serial.print("POST http://");
    client.print(server);
    Serial.print(server);
    client.print("/measurements/file?sensor_id=");
    Serial.print("/measurements/file?sensor_id=");
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
    client.println();
    Serial.println();
  } 
  else {
    // if you didn't get a connection to the server:
    Serial.println("connection failed");
    for(;;)
      ;
  }

  while ((!client.available()) && (fail < 50)) {
    delay_with_wd(100);
    fail++;
  }

  if (fail >= 50) {
    for(;;)
      ;
  }    
  // while(!client.available()){
  //   delay_with_wd(1);
  // }

  while (client.connected()) {
    while(client.available()) {
      char c = client.read();
      Serial.print(c);
    }
  }


  // while(client.connected()){
  //   Serial.println("Waiting for server to disconnect");
  // }


  client.stop();
}

void setup() {
  // start the serial library:
  Serial.begin(9600);

  // Get the connection config values
  getEEPROM();

  // Start the watchdog
  wdt_enable(WDTO_8S);

  // start the Ethernet connection:
  Serial.println("Attempting to configure Ethernet...");
  while (Ethernet.begin(mac) == 0) {
    Serial.println("Failed to configure Ethernet using DHCP");
    Serial.println("retrying...");
    delay_with_wd(5000);
  }
  // give the Ethernet shield a second to initialize:
  delay_with_wd(1000);

  // Get the sensor config information
  Serial.println("connecting...");

  if (client.connect(server, 80)) {
    // Serial.println("connected");

    client.print("GET http://");
    Serial.print("GET http://");
    client.print(server);
    Serial.print(server);
    client.print("/sensors/getconfig?cntrl=");
    Serial.print("/sensors/getconfig?cntrl=");
    client.print(controller);
    Serial.print(controller);
    client.print("&key=");
    Serial.print("&key=");
    client.print(request_key(controller));
    Serial.print(request_key(controller));
    client.println(" HTTP/1.0");
    Serial.println(" HTTP/1.0");
    client.println();
  } 
  else {
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
    if (client.available()) {
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
          else if (bComp(key,"interval")) sensors[sensorCount-1].interval = atoi(value);
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
      Serial.println("No more data, waiting for server to disconnect");
      delay_with_wd(1000);
    }
  }

  while (client.available()) {
    char c = client.read();  //Just clean up anything left
    // Serial.print(c);
  }

  client.stop();
  delay_with_wd(1000);


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
  }
}

void querySensors(OneWire ds) {
  // Deal with any DS18S20 sensors connected
  byte addr[8];   
  float value;
  unsigned int addrL, addrH;

  while (ds.search(addr)) {
    bool found = false;
    for (int i=0; (i < sensorCount) && !found; i++) {  //Find the matching config
      addrL = (addr[1] << 8) | addr[0];
      addrH = (addr[3] << 8) | addr[2];
      if ((sensors[i].addressL == addrL) && (sensors[i].addressH == addrH)) {  //Found our config
        value = getDSValue(ds, addr);
        Serial.print("Value: ");
        Serial.println(value);
        Serial.print("Sensor ID: ");
        Serial.println(sensors[i].id);
        sendValue(sensors[i].id, value);
        found = true;
        wdt_reset();
      }
    }
  }
}

void loop()
{
  // Serial.println("Free Memory: "+String(freeMemory()));

  querySensors(ds);

  ds.reset_search();
  delay_with_wd(minInterval);
}

int request_key(char *str)
{
  int key = 0;
  for (int i=0; str[i] != 0x00; i++) {
    key += str[i];
  }
  return key * REQUEST_KEY_MAGIC;
}

