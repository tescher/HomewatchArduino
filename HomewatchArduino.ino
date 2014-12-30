
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
#define CODE_LOG_LOC 200   //Location in the EEPROM where to store the current execution point
#define DEBUG         // Conditional Compilation

// Default config info in case nothing is in the EEPROM
byte mac[] = { 
  0x90, 0xA2, 0xDA, 0x00, 0x30, 0x7D };
char server[25] = "www.escherhomewatch.com";   // Up to 24 characters for the server name
char controller[30] = "BasementArduino";  //ID for this sensor controller, up to 29 chars
int last_code_log = 255;
byte current_code_log = 255;

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

//initialize watchdog
void WDT_Init(void) {
  //disable interrupts
  cli();
  //reset watchdog
  wdt_reset();
  //set up WDT interrupt
  WDTCSR = (1<<WDCE)|(1<<WDE);
  //Start watchdog timer with 8s timeout, interrupt and system reset modes
  WDTCSR = (1<<WDIE)|(1<<WDE)|(1<<WDP3)|(1<<WDP0);
  //Enable global interrupts
  sei();
}

// WD Interrupt Vector - save log to EEPROM

ISR(WDT_vect) {
  EEPROM.write(CODE_LOG_LOC, current_code_log);
  while(1);
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
  current_code_log = location;
  // EEPROM.write(CODE_LOG_LOC, location);
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
  #if defined(DEBUG)
    Serial.println("Device is not a DS18x20 family device.");
  #endif
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
    client.print(server);
    client.print("/measurements?sensor_id=");
    client.print(sensorID);
    dtostrf(value, 3, 1, buf);
    client.print("&value=");
    client.print(buf);
    client.print("&key=");
    client.print(request_key(buf));
    client.println(" HTTP/1.0");
    client.print("Host: ");
    client.println(server);
    client.println("Content-Length: 0");
    client.println();
#if defined(DEBUG)
    Serial.print("POST http://");
    Serial.print(server);
    Serial.print("/measurements?sensor_id=");
    Serial.print(sensorID);
    Serial.print("&value=");
    Serial.print(buf);
    Serial.print("&key=");
    Serial.print(request_key(buf));
    Serial.println(" HTTP/1.0");
    Serial.print("Host: ");
    Serial.println(server);
    Serial.println("Content-Length: 0");
    Serial.println();
#endif
  } 
  else {
    // if you didn't get a connection to the server:
    code_log(17);
#if defined(DEBUG)
    Serial.println("connection failed");
#endif
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
#if defined(DEBUG)
        Serial.print(c);
#endif
        if (msg_cnt == 7) {
          success = true;
#if defined(DEBUG)
          Serial.println("Success!");
#endif
        }
      } else {
        msg_cnt = 0;
      }
#if defined(DEBUG)
      Serial.print(c);
#endif
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
  // Find out where we crashed
  last_code_log = get_code_log();

  // start the serial library:
#if defined(DEBUG)
  Serial.begin(9600);
#endif  

  // Get the connection config values
  getEEPROM();

  // Start the watchdog
  WDT_Init();

  // start the Ethernet connection, try up to 5 times
#if defined(DEBUG)
  Serial.println("Attempting to configure Ethernet...");
#endif
  code_log(1);
  int eth_retry = 0;
  while ((Ethernet.begin(mac) == 0) && (eth_retry < 6)) {
#if defined(DEBUG)
    Serial.println("Failed to configure Ethernet using DHCP");
    Serial.println("retrying...");
#endif
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

#if defined(DEBUG)
  // Get the sensor config information
  Serial.println("connecting...");
#endif

  int i = 0;

  if (client.connect(server, 80)) {
    code_log(2);
    // Serial.println("connected");

    client.print("GET ");
    client.print("/sensors/getconfig?cntrl=");
    client.print(controller);
    client.print("&key=");
    client.print(request_key(controller));
    client.print("&log=CodeRestart%7C");
    client.print(last_code_log);
    client.print("%7CIP%7C");
    for (i=0;i<4;i++) {
      client.print(Ethernet.localIP()[i]);
      if (i<3) client.print(".");
    }
    client.println(" HTTP/1.0");
    client.print("Host: ");
    client.println(server);
    client.println();
#if defined(DEBUG)
    Serial.print("GET ");
    Serial.print("/sensors/getconfig?cntrl=");
    Serial.print(controller);
    Serial.print("&key=");
    Serial.print(request_key(controller));
    Serial.print("&log=CodeRestarted%7C");
    Serial.print(last_code_log);
    Serial.print("%7CIP%7C");
    for (i=0;i<4;i++) {
      Serial.print(Ethernet.localIP()[i]);
      if (i<3) Serial.print(".");
    }
    Serial.println(" HTTP/1.0");
    Serial.print("Host: ");
    Serial.println(server);
#endif
  } 
  else {
    code_log(3);
#if defined(DEBUG)
    // if you didn't get a connection to the server:
    Serial.println("connection failed");
#endif
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
  while (client.connected()) {
    code_log(4);
    if (client.available()) {
      code_log(5);
      char c = client.read();
#if defined(DEBUG)
      Serial.print(c);
#endif
      if (!jsonStarted && (c == '[')) {
        jsonStarted = true;
      } 
      else if (jsonStarted && !objectStarted && (c == '{')) {
        objectStarted = true;
        sensorCount += 1;
#if defined(DEBUG)
        Serial.println(sensorCount);
#endif
      } 
      else if (objectStarted && !keyStarted && (c == '"')) {
        keyStarted = true;
        i = 0;
#if defined(DEBUG)
        Serial.println("keyStart");
#endif
      } 
      else if (keyStarted && !haveKey && (c != '"')) {
        key[i++] = c;
      } 
      else if (keyStarted && !haveKey && (c == '"')) {
        haveKey = true;
        key[i] = 0x00;
#if defined(DEBUG)
        Serial.println("haveKey");
#endif
      } 
      else if (haveKey && !valueStarted && (c != ' ') && (c != ':')) {
        valueStarted = true;
#if defined(DEBUG)
        Serial.println("valueStart");
#endif
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
#if defined(DEBUG)
          Serial.println("valueDone");
#endif
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
#if defined(DEBUG)
      Serial.println("No more data, waiting for server to disconnect");
#endif
      delay_with_wd(1000);
    }
  }

  while (client.available()) {
    code_log(7);
    char c = client.read();  //Just clean up anything left
#if defined(DEBUG)
    Serial.print(c);
#endif
  }
  
  code_log(8);
  client.stop();
  delay_with_wd(1000);
  
  if (sensorCount < 1) {
    code_log(9);
#if defined(DEBUG)
    Serial.println("No sensors received");
#endif
        for(;;)
      ;
  }
 

  // Parse the sensor info
  // configInput.toCharArray(buff, 1000);
  for (i = 0; i < sensorCount; i++) {
    if ((sensors[i].interval > 0) && (sensors[i].interval < minInterval)) {
      minInterval = sensors[i].interval;
    }
#if defined(DEBUG)
    Serial.print(sensors[i].id);
    Serial.print(":");
    Serial.print(sensors[i].addressH);
    Serial.print(":");
    Serial.print(sensors[i].addressL);
    Serial.print(":");
    Serial.println(sensors[i].interval);
    Serial.print(":");
    Serial.println(sensors[i].type);
#endif
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
#if defined(DEBUG)
    Serial.print("AddrL: ");
    Serial.print(addrL);
    Serial.print(" AddrH: ");
    Serial.println(addrH);
#endif
    for (int i=0; (i < sensorCount) && !found; i++) {  //Find the matching config
      if ((sensors[i].type == 1) && (sensors[i].addressL == addrL) && (sensors[i].addressH == addrH)) {  //Found our config
        value = getDSValue(ds, addr);
#if defined(DEBUG)
        Serial.print("Value: ");
        Serial.println(value);
        Serial.print("Sensor ID: ");
        Serial.println(sensors[i].id);
#endif
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
#if defined(DEBUG)
      Serial.print("Value: ");
      Serial.println(value);
      Serial.print("Sensor ID: ");
      Serial.println(sensors[i].id);
#endif
      sendValue(sensors[i].id, value);
      wdt_reset();
    }
  }  
  
  

  if (int_is_seconds) {
    code_log(13);
    for (int i=0; i < minInterval; i++) {
      delay_with_wd(1000);
#if defined(DEBUG)
      Serial.println(i);
#endif
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

