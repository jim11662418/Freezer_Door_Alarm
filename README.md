# Freezer Door Alarm
Uses Wemos D1 Mini ESP8266 module and a magnetic reed switch to monitor a freezer door. If the freezer door is left ajar for more than 60 seconds, the ESP8266 sounds an audible alarm and sends a text message. The ESP8266 serves a web page that shows the door status (open or closed) and freezer temperature (read from a Dallas Semiconductor DS18B20 1-wire temperature sensor mounted inside the freezer). Every 2 minutes, the freezer temperature is sent to Thingspeak for graphing.
<p align="center"><img src="Freezer Alarm.PNG"/>
