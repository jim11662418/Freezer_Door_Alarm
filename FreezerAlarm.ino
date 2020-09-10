//---------------------------------------------------------------------------------------------------------------------------------
// Copyright © 2020 Jim Loos
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
// (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//---------------------------------------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------
// Freezer Door Monitor 
// uses a magnetic reed switch to detect when the freezer door is opened.
// sounds an audible alarm and sends text message if the basement freezer 
// door is open for more than 60 seconds.
// web page shows the current time, uptime, freezer temperature and door status.
// every 2 minutes, post the freezer temperature to a ThingSpeak graph.
//
// Flashing blue LED = freezer door is open at start up.
// Flashing green LED = freezer door is closed.
// Flashing red LED = freezer door is open.

//-------------------------------------------------------------------------------------------
// Version 1.0    initial release
//         2.0    pin change interrupt replaces pin polling, reworked main loop
//-------------------------------------------------------------------------------------------
#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <NtpClientLib.h>           // https://github.com/gmag11/NtpClient
#include <EMailSender.h>            // https://github.com/xreef/EMailSender
#include <Ticker.h>                 // https://github.com/esp8266/Arduino/blob/master/libraries/Ticker/src/Ticker.h
#include <DallasTemperature.h>      // https://github.com/milesburton/Arduino-Temperature-Control-Library
#include <ThingSpeak.h>             // https://github.com/mathworks/thingspeak-arduino

// WEMOS D1 Mini and NodeMCU pins
#define D0 16           // blue LED on the left side of NodeMCU
#define D1  5
#define D2  4
#define D3  0
#define D4  2           // blue LED on the right side of NodeMCU (BUILTIN_LED on WEMOS Mini)
#define D5 14
#define D6 12
#define D7 13
#define D8 15
#define D9  3           // RX0 (Serial console)
#define D10 1           // TX0 (Serial console) 

#define INTERRUPTPIN D2 // the ESP8266 supports interrupts using any GPIO except GPIO16
#define DEBOUNCE 50     // 50 milliseconds for switch debouncing

#define WIFI_SSID "traXu74P-Ext"          // WIFI network name
#define WIFI_PASSWORD "Zjx9rUVYyT"        // WIFI network password

#define REDLED D6                         // red RGB LED connected to D6
#define GREENLED D0                       // green RGB LED connected to D0
#define BLUELED D1                        // blue RGB LED connected to D1

#define OPEN 1                            // switch is open, freezer door is open
#define CLOSED 0                          // switch is closed, freezer door is closed

#define SPEAKERPIN D5                     // pin connected to base of 2N3904 transistor used to drive the speaker
#define ALARM_DELAY 60                    // freezer door open for 60 seconds sounds alarm
#define ONE_WIRE_BUS D3                   // DS18B20 1-wire temperature sensor connection

// ISR functions should be defined with ICACHE_RAM_ATTR attribute to let the compiler know to never remove them from the IRAM. 
// If the ICACHE_RAM_ATTR attribute is missing the firmware will crash at the first call to attachInterrupt() on a ISR routine
// that happens not to be in ram at that moment. This bug will manifest itself only on some platform and some code configurations,
// as it entirely depends of where the compiler chooses to put the ISR functions.
void ICACHE_RAM_ATTR pinInterruptISR();
void oneSecondTimerISR();
void toneTimerISR();

// ThingSpeak Settings
unsigned long tsChannelNumber = 2;
const char * tsWriteAPIkey = "TTYHJ6EE7BOHGEQV";

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

// email sender for the text messages...
EMailSender emailSend("jim11662418@gmail.com", "hR637&jtaF");

Ticker oneSecondTimer,toneTimer;
WiFiClient client;
WiFiServer server(80);                    // web server
NTPSyncEvent_t ntpEvent;                  // last NTP triggered event
int timeZone=-5;                          // default to EST
volatile boolean alarmOn = false;
volatile boolean sendEmailFlag = false;
volatile uint8_t doorOpenCounter = 0;  
volatile boolean switchState = OPEN;
volatile boolean tempSensorFlag = false;
volatile boolean thingspeakFlag = false;
volatile boolean switchInterruptFlag = false;
boolean syncEventTriggered = false;       // true if an NTP time sync event has been triggered
boolean syncSuccessful = false;           // result of last NTP time sync attempt
float freezerTemperature;                 // freezer temperature from DS18B20 1-wire sensor
char temperature[5];                      // freezer temperature from DS18B20 converted into an array of characters
uint8_t sensorCount;                      // number of DS18xxx family devices on bus
 
void setup() {
   Serial.begin(115200);
   
   pinMode(INTERRUPTPIN,INPUT_PULLUP);
   attachInterrupt(digitalPinToInterrupt(INTERRUPTPIN),pinInterruptISR,CHANGE);
   switchState = digitalRead(INTERRUPTPIN);       // initial state of the switch contacts
  
   unsigned long waitTime = millis()+500;
   while(millis() < waitTime);                     // wait one half second
   
   Serial.println("\n\n----------------------------------");
   Serial.println("Freezer Door Monitor Version 2.0");
   Serial.printf("Sketch size: %u\n",ESP.getSketchSize());
   Serial.printf("Free size: %u\n",ESP.getFreeSketchSpace());
   Serial.println("----------------------------------");

   Serial.print("Waiting for WiFi");
   while (WiFi.status() != WL_CONNECTED) {
      waitTime = millis()+500;
      while(millis() < waitTime)yield();
      Serial.print(".");
   }
   Serial.println(" connected");

   server.begin();
   Serial.printf("Web server started, browse to %s.\n", WiFi.localIP().toString().c_str());

   sensors.begin();                                         // start up 1-wire
   sensorCount = sensors.getDS18Count();                    // see if there are any sensors on the 1-wire bus
   if (sensorCount) {
      Serial.printf("%u DS18xxx sensor(s) detected.\n", sensorCount);
      sensors.setWaitForConversion(false);                  // we'll do our own timing 
      sensors.requestTemperatures();                        // send the first command to get temperatures    
   }

   NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {ntpEvent=event;syncEventTriggered=true;});
   NTP.begin("0.us.pool.ntp.org",timeZone,true);            // now that we have an IP address, start the NTP client
   NTP.setInterval(20,600);

   ThingSpeak.begin(client);   

   oneSecondTimer.attach(1,oneSecondTimerISR);              // start the 1 second timer   
   toneTimer.attach_ms(250,toneTimerISR);                   // start the 250 millisecond speaker tone timer   
   
   pinMode(D4,OUTPUT);                                      // blue LED on the D1 Mini board
   pinMode(SPEAKERPIN,OUTPUT); 
   pinMode(REDLED,OUTPUT); 
   pinMode(GREENLED,OUTPUT); 
   pinMode(BLUELED,OUTPUT); 
   digitalWrite(D4,HIGH);                                   // turn off the built-in blue LED
   digitalWrite(SPEAKERPIN,LOW);                            // turn off the speaker
   digitalWrite(REDLED,LOW);                                // turn off the rgb leds
   digitalWrite(GREENLED,LOW); 
   digitalWrite(BLUELED,LOW); 

   // wait here flashing the blue led until the door is closed
   if (digitalRead(INTERRUPTPIN)==OPEN) {
      Serial.println("Close the freezer door.");
      while (digitalRead(INTERRUPTPIN)==OPEN) {
         digitalWrite(BLUELED,HIGH);
         waitTime= millis()+500;
         while(millis() < waitTime)yield();
         digitalWrite(BLUELED,LOW);
         waitTime= millis()+500;
         while(millis() < waitTime)yield();
      }      
   }
   switchState = CLOSED;
}  

void loop() {
   static unsigned long previousLEDMillis = 0; 
   static int lastdoorOpenCounter = 0; 

   if(switchInterruptFlag){                                 // if the switch has changed state...
      switchInterruptFlag = false;
      if (switchState==OPEN){                               // if the door has opened
         doorOpenCounter=ALARM_DELAY;                       // start the door open timer         
         Serial.println("The freezer door is open at "+NTP.getTimeDateString()+".");
         Serial.println("Starting countdown.");
      }   
      else {                                                // else the door has closed
         doorOpenCounter = 0;   
         lastdoorOpenCounter = 0;
         Serial.println("The freezer door is closed at "+NTP.getTimeDateString());
         if (alarmOn) {
            alarmOn = false;
            Serial.println("The alarm is off."); 
         }
      }
   }

   if (syncEventTriggered) {                                // if an NTP time sync event has occured...
      processSyncEvent(ntpEvent);                           // process the event
      syncEventTriggered = false;
   }

   if (doorOpenCounter != lastdoorOpenCounter) {            // if the door open counter has changed...
      lastdoorOpenCounter = doorOpenCounter;
      Serial.println(doorOpenCounter);
      if (alarmOn)
         Serial.println("Alarm ON!");
   }

   serveHTMLpage();                                   // freezer status web page
   
   if (tempSensorFlag) {                              // timer sets tempSensorFlag every 10 seconds
      tempSensorFlag = false; 
      readSensors();
   }

   if (thingspeakFlag) {                              // timer sets thingspeakFlag every 2 minutes
      thingspeakFlag = false;
      postToThingSpeak();
   }

   if (sendEmailFlag) {                               // sendEmailFlag is set when door has been open for 60 seconds
      
      EMailSender::EMailMessage message;
      message.subject = "Freezer Door Alarm!";
      message.message = "The freezer door is open at "+NTP.getTimeDateString()+".";
            
      Serial.println("Sending text to Jim's number...");
      EMailSender::Response resp = emailSend.send("7034022536@tmomail.net", message);
      Serial.print("Response: "); Serial.println(resp.desc); 
      if (resp.status)                                 // if successful...
         sendEmailFlag = false;
   }
   
   unsigned long currentLEDMillis = millis();
   if (currentLEDMillis-previousLEDMillis >= 500) {   // flash the LEDs every 500 milliseconds
      previousLEDMillis = currentLEDMillis;
      flashLEDs();
   }
}
////////////// end of loop() ///////////////////////


//--------------------------------------------------------------------------
// interrupt when the door reed switch changes state
//--------------------------------------------------------------------------
void pinInterruptISR() {
   unsigned long debounce_time = millis()+DEBOUNCE;
   while(millis() < debounce_time);               // wait 50 milliseconds for the switch contacts to stop bouncing
   switchState = digitalRead(INTERRUPTPIN);       // read the switch contacts
   switchInterruptFlag = true;
}

//--------------------------------------------------------------------------
// NTP event handler
//--------------------------------------------------------------------------
void processSyncEvent(NTPSyncEvent_t ntpEvent) {

  if (ntpEvent) {
     syncSuccessful=false;
     if (ntpEvent == noResponse) Serial.println("Time Sync error: NTP server not reachable");
     else if (ntpEvent == invalidAddress) Serial.println("Time Sync error: Invalid NTP server address");
  }
  else {
     syncSuccessful= true;
     Serial.print("Got NTP time: ");
     Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
  }
}

//--------------------------------------------------------------------------
// play an audio tone by toggling the speaker. duration in milliseconds, frequency in hertz
//--------------------------------------------------------------------------
void playTone(uint32_t duration,uint16_t frequency) {
   duration*=1000;
   uint16_t period=(1.0/frequency)*1000000;
   uint32_t elapsed_time=0;
   while (elapsed_time<duration) {
      digitalWrite(SPEAKERPIN,HIGH);
      delayMicroseconds(period/2);
      digitalWrite(SPEAKERPIN,LOW);
      delayMicroseconds(period/2);
      elapsed_time+=(period);
   }
}

//--------------------------------------------------------------------------
// speaker timer interrupt fires to play a tone every 250 milliseconds if 
// the alarm is on and the door is open
//--------------------------------------------------------------------------
void toneTimerISR() {
   uint16_t alarmTones[] = {300,600,900,600};                   // array of alarm tones in Hz  
   static uint8_t alarmToneptr = 0;
   if ((alarmOn) && (switchState==OPEN)) {
      playTone(200,alarmTones[++alarmToneptr&0x03]);
   }
}

//--------------------------------------------------------------------------
// Ticker interrupt fires every second. counts from 0 to 120 seconds
//--------------------------------------------------------------------------
void oneSecondTimerISR() {
   static int secondCount = 0;

   if (!(secondCount % 30)) 
      tempSensorFlag = true;        // set the flag to read the 1-wire sensor once every 30 seconds
   
   if (++secondCount==120) {
      secondCount = 0;   
      thingspeakFlag = true;        // set the flag to post to Thingspeak every 120 seconds (2 minutes)
   }
   
   if (doorOpenCounter) {           // decrement the door open counter every second
      --doorOpenCounter;
      if (!doorOpenCounter) {
         alarmOn = true;
         sendEmailFlag = true;
      }
   }
}

//--------------------------------------------------------------------------
// flash the green or red LED depending on the door switch
//--------------------------------------------------------------------------
void flashLEDs() {
  static byte ledCounter = 0;
  if (switchState==OPEN){                             // if the freezer door is open... 
     digitalWrite(GREENLED,LOW);                      // turn off the green LED
     digitalWrite(REDLED,(++ledCounter&0x01));        // toggle the red LED (HIGH or on for odd counts, LOW or off for even counts)
  }
  else {                                              // else the freezer door is closed...
     digitalWrite(REDLED,LOW);                        // turn off the red LED
     digitalWrite(GREENLED,(++ledCounter&0x01));      // toggle the green LED (HIGH or on for odd counts, LOW or off for even counts)     
  }
}

//--------------------------------------------------------------------------
// read the 1-wire tenperature sensor
//--------------------------------------------------------------------------
void readSensors() {
   if (sensorCount) {
      if (sensors.isConversionComplete()) {
         freezerTemperature = sensors.getTempFByIndex(0);
         freezerTemperature = round(freezerTemperature*10)/10;
         sprintf(temperature,"%.1f",freezerTemperature);  // read the last temperature conversion       
         Serial.println(String(temperature)+" °F");
      }
      sensors.requestTemperatures();                     // send the command for next temperature conversion, we'll read it next time
   }      
}

//--------------------------------------------------------------------------
// Send the freezer temperature to ThingSpeak
//--------------------------------------------------------------------------
void postToThingSpeak() {
  Serial.println("Attempting connection to ThingSpeak");
  int httpCode = ThingSpeak.writeField(tsChannelNumber, 1, freezerTemperature, tsWriteAPIkey);
  if (httpCode == 200) {
    Serial.println("ThingSpeak write successful at "+NTP.getTimeDateString()+".");
  }
  else {
    Serial.println("Problem writing to ThingSpeak at "+NTP.getTimeDateString()+". HTTP error code " + String(httpCode));
  }
}  

//--------------------------------------------------------------------------
// serve up the web page when the client sends a GET request
//--------------------------------------------------------------------------
void serveHTMLpage() {
   WiFiClient client=server.available();
   if (client) {
      String request = client.readStringUntil('\r');
      client.flush();
      if (request.indexOf("GET / HTTP/1.1") != -1) {
         String htmlPage = "";
         String doorStatus ="";
         if (switchState==HIGH) doorStatus = "open"; else doorStatus = "closed";
         htmlPage+="HTTP/1.1 200 OK\r\n";
         htmlPage+="Content-type: text/html\r\n";
         htmlPage+="Server: ESP8266\r\n\r\n";
         htmlPage+="<!DOCTYPE HTML>";
         htmlPage+=    "<html>";
         htmlPage+=      "<head>";
         htmlPage+=         "<META HTTP-EQUIV=\"refresh\" CONTENT=\"1\">";
         htmlPage+=         "<meta content=\"text/html; charset=utf-8\">";
         htmlPage+=         "<title>ESP8266 Freezer Door Monitor</title>";
         htmlPage+=       "</head>";
         htmlPage+=       "<body style=\"background-color:lightgrey;\">";
         htmlPage+=         "<h1>ESP8266 Freezer Door Monitor</h1>";
         htmlPage+=         "<p>Time: "+NTP.getTimeDateString()+"</p>";
         htmlPage+=         "<p>Uptime: "+getUpTime(NTP.getUptime())+"</p>";
         if (sensorCount)
            htmlPage+=      "<p>Temperature: "+String(temperature)+" &#8457</p>";         
         htmlPage+=         "<p>The freezer door is "+doorStatus+".</p>";
         htmlPage+=       "</body>";
         htmlPage+=    "</html>";
         client.print(htmlPage);
      }
   }
}

//--------------------------------------------------------------------------
// return the uptime as a formatted string for the web page
//--------------------------------------------------------------------------
String getUpTime(unsigned long uptime) {
   int d=uptime/86400;
   int h=(uptime%86400)/3600;
   int m=(uptime%3600)/60;
   int s=uptime%60;
   String daysStr="";
   if (d>0) (d==1) ? daysStr="1 day," : daysStr=String(d)+" days,";
   String hoursStr="";
   if (h>0) (h==1) ? hoursStr="1 hour," : hoursStr=String(h)+" hours,";
   String minutesStr="";
   if (m>0) (m==1) ? minutesStr="1 minute" : minutesStr=String(m)+" minutes";
   return daysStr+" "+hoursStr+" "+minutesStr;
}

 
