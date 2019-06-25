//---------------------------------------------------------------------------------------------------------
// Freezer Door Monitor 
// Monitor the freezer door with a magnetic reed switch. 
// If the garage freezer door is open for more than 60 seconds, sounds audible alarm and sends text message.
// Web page shows the current time, uptime, freezer temperature and door status.
// Every 2 minutes, post the freezer temperature to a ThingSpeak graph.
//
// Version 1.0    initial release
//         2.0    pin change interrupt replaces pin polling, reworked main loop
//         2.1    added OTA updates
//---------------------------------------------------------------------------------------------------------

#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <NtpClientLib.h>
#include "Gsender.h"
#include <Ticker.h>
#include <DallasTemperature.h>

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

#define WIFI_SSID "********"              // WIFI network name
#define WIFI_PASSWORD "********"          // WIFI network password

#define REDLED D6                         // red part of RGB LED connected to D6
#define GREENLED D0                       // green part of RGB LED connected to D0
#define BLUELED D1                        // blue part of RGB LED connected to D1

#define SPEAKERPIN D5                     // 2N3904 transistor used to drive the speaker connected to D5
#define SWITCHPIN D2                      // reed switch for freezer door connected to D2
#define INTERVAL 50                       // 50 milliseconds for switch debouncing

//#define TESTING                         // when TESTING is defined, reduces door open timout period to 5 seconds, does not send text message

#ifdef TESTING
   #define ALARM_DELAY 5                  // if testing, freezer door open for 5 seconds sounds alarm
#else   
   #define ALARM_DELAY 60                 // if not testing, freezer door open for 60 seconds sounds alarm
#endif

#define ONE_WIRE_BUS D3                   // DS18B20 1-wire temperature sensor connected to D3

// ThingSpeak Settings
String thingspeakWriteAPIKey = "**************"; // write API key for the ThingSpeak Channel
const char* thingspeakServer = "api.thingspeak.com";

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);            // 1-wire

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

// for the text message...
Gsender *gsender;                         // Gmail sender

Ticker ledTimer,toneTimer,doorOpenTimer;  // timers
WiFiClient client;                        // WiFi
WiFiServer server(80);                    // web server
NTPSyncEvent_t ntpEvent;                  // last NTP triggered event
WiFiEventHandler event1,event2,event3;    // WiFi event handlers
boolean syncEventTriggered = false;       // true if a time sync event has been triggered
boolean syncSuccessful = false;           // result of last time sync attempt
int timeZone=-5;                          // default to EST
volatile boolean stateChanged;            // flag
volatile uint8_t doorOpenCounter;         // counter       
volatile uint8_t freezerState;            // state machine
volatile boolean switchState;             // status of reed switch
char temperature[5];                      // ambient temperature from DS18B20 as string
uint8_t sensorCount;                      // number of DS18xxx family devices on 1-wire bus


//--------------------------------------------------------------------------
// Setup
//--------------------------------------------------------------------------
void setup() {
   /*---------- OTA Setup ------------*/
   // Hostname defaults to esp8266-[ChipID]
   ArduinoOTA.setHostname("FreezerAlarm");
   // No authentication by default
   //ArduinoOTA.setPassword((const char *)"123");
   ArduinoOTA.onStart([]() {
      Serial.println("\nOTA Start");
   });
   ArduinoOTA.onEnd([]() {
      Serial.println("\nOTA End");
   });
   ArduinoOTA.onProgress([](unsigned int progress,unsigned int total) {
      Serial.printf("OTA Progress: %u%%\r",(progress/(total/100)));
    });
   ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ",error);
      if (error==OTA_AUTH_ERROR) Serial.println("OTA Auth Failed");
      else if (error==OTA_BEGIN_ERROR) Serial.println("OTA Begin Failed");
      else if (error==OTA_CONNECT_ERROR) Serial.println("OTA Connect Failed");
      else if (error==OTA_RECEIVE_ERROR) Serial.println("OTA Receive Failed");
      else if (error==OTA_END_ERROR) Serial.println("OTA End Failed");
   });
   ArduinoOTA.begin(); 
   /*------ End OTA Setup -------*/
  
   gsender=Gsender::Instance();                             // get pointer to class instance for Gmail sender    

   NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {ntpEvent=event;syncEventTriggered=true;});

   event1=WiFi.onStationModeConnected(onSTAConnected);      // when first connected to router
   event2=WiFi.onStationModeGotIP(onSTAGotIP);              // when IP address received from router
   event3=WiFi.onStationModeDisconnected(onSTADisconnected);// when disconnected from router

   sensors.begin();                                         // start up 1-wire
   sensorCount = sensors.getDS18Count();                    // see if there are any sensors on the 1-wire bus
   if (sensorCount) {
      sensors.setWaitForConversion(false);                  // we'll do our own timing 
      sensors.requestTemperatures();                        // send the first command to get temperatures    
   }

   Serial.begin(115200);                                    // console for debugging and status
   Serial.println("----------------------------------");
   Serial.println("Freezer Door Monitor Version 2.1.0");
   Serial.printf("Sketch size: %u\n",ESP.getSketchSize());
   Serial.printf("Free size: %u\n",ESP.getFreeSketchSpace());
   Serial.println("----------------------------------");

   Serial.println("Waiting for WiFi connection...");
   WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
   while (WiFi.status() != WL_CONNECTED) {                  // wait here until connected to WiFi
      delay(500);
   }

   NTP.begin("0.us.pool.ntp.org",timeZone,true);            // now that we have an IP address, start the NTP client

  #ifdef TESTING
     Serial.println("Testing.");
  #endif 
      
   pinMode(D4,OUTPUT);                                      // configure pin for blue LED on the D1 Mini board
   pinMode(SPEAKERPIN,OUTPUT);                              // configure pin for the speaker
   pinMode(REDLED,OUTPUT);                                  // configure pin for the red part of the RGB LED
   pinMode(GREENLED,OUTPUT);                                // configure pin for the green part of the RGB LED
   pinMode(BLUELED,OUTPUT);                                 // configure pin for blue part of the RGB LED
   pinMode(SWITCHPIN,INPUT_PULLUP);                         // configure pin for the reed switch
   digitalWrite(SWITCHPIN,HIGH);                            // turn of pullup
   digitalWrite(D4,HIGH);                                   // turn off the built-in blue LED
   digitalWrite(SPEAKERPIN,LOW);                            // turn off the speaker
   digitalWrite(REDLED,LOW);                                // turn off the rgb leds
   digitalWrite(GREENLED,LOW); 
   digitalWrite(BLUELED,LOW); 

   // wait here flashing the blue led until the freezer door is closed  
   while (digitalRead(SWITCHPIN)==HIGH) {
      digitalWrite(BLUELED,HIGH);
      delay(500);
      digitalWrite(BLUELED,LOW);
      delay(500);
   }

   // now that we know the freezer door is closed...
   freezerState = 1;
   stateChanged = true;
   switchState = LOW;
   doorOpenCounter = 0;
   
   ledTimer.attach_ms(500,ledTimerISR);                     // start the 500 millisecond led timer
   toneTimer.attach_ms(250,toneTimerISR);                   // start the 250 millisecond speaker tone timer   
   doorOpenTimer.attach_ms(1000,doorOpenTimerISR);          // start the 1000 millisecond door open timer 
     
   server.begin();                                          // start up the web server

   attachInterrupt(digitalPinToInterrupt(SWITCHPIN),pinInterruptISR,CHANGE);
}

//--------------------------------------------------------------------------
// Main loop
//--------------------------------------------------------------------------
void loop() {
   static unsigned long previousMillis = 0;
   static uint8_t minuteCount = 0;

   ArduinoOTA.handle(); 

   unsigned long currentMillis = millis();
   if (currentMillis-previousMillis >= 1000*60) {                   // once every minute...
      previousMillis = currentMillis;
      
      if (sensorCount) {
         if (sensors.isConversionComplete()) {
            sprintf(temperature,"%.1f",sensors.getTempFByIndex(0));  // read the last temperature conversion       
         }
         sensors.requestTemperatures();                              // send the command for next temperature conversion, we'll read it next second
      }   
              
      if (++minuteCount==2) {
         minuteCount = 0;
         if (sensorCount) {
            postToThingSpeak();
         }
      }
   }   

   if (syncEventTriggered) {                                // if a time sync event has occured...
      processSyncEvent(ntpEvent);                           // process the event
      syncEventTriggered = false;
   }

   serveHTMLpage();                                       // service the web page
  
   if (stateChanged) {
     switch (freezerState) {
        case 1:
           Serial.println("The freezer door is closed at "+NTP.getTimeDateString()+".");
           break;
        case 2:
           Serial.println("The freezer door is open at "+NTP.getTimeDateString()+".");
           break;
        case 3:
            Serial.println("The alarm is on at "+NTP.getTimeDateString()+".");
            sendText();
            break;
     }
     stateChanged = false; 
  }
}

//--------------------------------------------------------------------------
// Send Text Alert
//--------------------------------------------------------------------------
void sendText(){
   String subject = "Freezer Door Alarm!";
   String msgBody = "The freezer door is open at "+NTP.getTimeDateString()+".";
   String sendTo1 = "**********@vtext.com";      // Jim's number
   String sendTo2 = "**********@vtext.com";      // El's number
   #ifndef TESTING 
      if(gsender->Subject(subject)->Send(sendTo1,msgBody)) 
         Serial.println("Text message send to "+sendTo1+".");
      if(gsender->Subject(subject)->Send(sendTo2,msgBody)) 
         Serial.println("Text message send to "+sendTo2+".");
   #endif
}

//--------------------------------------------------------------------------
// Send Freezer Temperature to ThingSpeak
//--------------------------------------------------------------------------
void postToThingSpeak() {
  if (client.connect(thingspeakServer,80)) {
    Serial.println("Connected to ThingSpeak at "+NTP.getTimeDateString()+".");
    String body = "field1="+String(temperature);
    client.println("POST /update HTTP/1.1");
    client.println("Host: api.thingspeak.com");
    client.println("User-Agent: ESP8266 (nothans)/1.0");
    client.println("Connection: close");
    client.println("X-THINGSPEAKAPIKEY: "+thingspeakWriteAPIKey);
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Content-Length: "+String(body.length()));
    client.println("");
    client.print(body);
  }
  client.stop();
}  

//--------------------------------------------------------------------------
// handle events triggered by NTP
//--------------------------------------------------------------------------
void processSyncEvent(NTPSyncEvent_t ntpEvent) {
  if (ntpEvent) {
     syncSuccessful=false;
     Serial.print("Time Sync error: ");
     if (ntpEvent == noResponse) Serial.println("NTP server not reachable");
     else if (ntpEvent == invalidAddress) Serial.println("Invalid NTP server address");
  }
  else {
     syncSuccessful= true;
     Serial.print("Got NTP time: ");
     Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
  }
}

//--------------------------------------------------------------------------
// play an audio tone by toggling the speaker, duration in milliseconds, frequency in hertz
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
// speaker timer interrupt fires every 250 milliseconds
//--------------------------------------------------------------------------
void toneTimerISR() {
   uint16_t alarmTones[] = {300,600,900,600};                   // array of alarm tones in Hz  
   static uint8_t alarmToneptr = 0;
   if (freezerState == 3) {
      playTone(200,alarmTones[++alarmToneptr&0x03]);
   }
}

//--------------------------------------------------------------------------
// door open timer interrupt fires every 1000 milliseconds
//--------------------------------------------------------------------------
void doorOpenTimerISR() {
  if ((doorOpenCounter) && (freezerState ==2)) { 
     --doorOpenCounter;
     if (!doorOpenCounter) {
        freezerState = 3;
        stateChanged = true;
     }
  }
}

//--------------------------------------------------------------------------
// led timer interrupt fires every 500 milliseconds
//--------------------------------------------------------------------------
void ledTimerISR() {
  static byte ledCounter = 0;
  if (switchState==HIGH){                             // if the freezer door is open... 
     digitalWrite(GREENLED,LOW);                      // turn off the green LED
     digitalWrite(REDLED,(++ledCounter&0x01));        // toggle the red LED (HIGH or on for odd counts, LOW or off for even counts)
  }
  else {                                              // else the freezer door is closed...
     digitalWrite(REDLED,LOW);                        // turn off the red LED
     digitalWrite(GREENLED,(++ledCounter&0x01));      // toggle the green LED (HIGH or on for odd counts, LOW or off for even counts)     
  }
}

//--------------------------------------------------------------------------
// handle event triggered when connected to the router
//--------------------------------------------------------------------------
void onSTAConnected(WiFiEventStationModeConnected) {
   Serial.print("Connected to ");
   Serial.println(WIFI_SSID);
}

//--------------------------------------------------------------------------
// handle event triggered when when an IP address is received from the router
//--------------------------------------------------------------------------
void onSTAGotIP(WiFiEventStationModeGotIP ipInfo) {
   Serial.printf("IP address: %s\r\n",ipInfo.ip.toString().c_str());    // print the IP address
}

//--------------------------------------------------------------------------
// handle event triggered when disconnected from router
//--------------------------------------------------------------------------
void onSTADisconnected(WiFiEventStationModeDisconnected event_info) {
   Serial.printf("\nDisconnected from SSID: %s\n",event_info.ssid.c_str());
   Serial.printf("Reason: %d\n",event_info.reason);
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
         String tempF = "";
         if (switchState==HIGH) doorStatus = "open"; else doorStatus = "closed";
         if (sensorCount) tempF = String(temperature)+" F"; else tempF = "not available"; 
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
         htmlPage+=         "<p>Temperature: "+tempF+"</p>";
         htmlPage+=         "<p>The freezer door is "+doorStatus+".</p>";
         htmlPage+=       "</body>";
         htmlPage+=    "</html>";
         client.print(htmlPage);
      }
   }
}

//--------------------------------------------------------------------------
// return the uptime as a formatted string
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

//--------------------------------------------------------------------------
// interrupt service routine triggered when the door reed switch changes state
//--------------------------------------------------------------------------
void pinInterruptISR() {
   unsigned long debounce_time = millis()+INTERVAL;
   while(millis() < debounce_time);               // wait 50 milliseconds for the switch contacts to stop bouncing
   switchState = digitalRead(SWITCHPIN);
   switch (freezerState) {
      case 1:
         if (switchState==HIGH) {
            freezerState = 2;
            doorOpenCounter = ALARM_DELAY;            
            stateChanged = true;
         }
         break;
      case 2:
         if (switchState==LOW) {
            freezerState = 1;
            stateChanged = true;
         }
         break;
      case 3:
         if (switchState==LOW) {
            freezerState = 1;
            stateChanged = true;
         }
         break;
  }
}
 
