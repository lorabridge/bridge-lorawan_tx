
LoRaBridge LoRaWAN Transmitter
==============================

This repository includes the C implementation of a Class-A LoRaWAN transmitter, which is necessary
to forward measurements collected at a LoRaBridge Bridge unit towards a LoRaBridge gateway. This
proof-of-concept implementation is based on the Raspberry PI port of LMIC (Author Wolgang Klenk).


Features
--------

- Establishment of LoRaWAN connection via Over-the-Air Activation procedure
- Scheduling of LoRaWAN packets 
- Removal of stale measurements
- Signaling of LoRaWAN mac events (e.g. Link dead) towards user interface

Supported Hardware
------------------

- Adafruit LoRa Radio Bonnet with OLED display 


Limitations/Disclaimer
----------------------

The non-realtime operation of Raspberry PI OS might lead to an added latency, which can cause packet
loss in LoRaWAN Class-A operation. While we have not experienced this with installations using
Raspberry PI 4B, other installations with different Raspberry PI hardware and/or OS might suffer from 
the issue. 

License
-------

TODO


