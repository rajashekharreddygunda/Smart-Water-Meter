# Smart-Water-Meter
A STM32F103C8T6 (BLUE PILL) based smart water meter system for measuring water consumption of a household inspired by Electric Meter.

The system uses 
1. STM32F103C8T6 based development board[blue pill]
2. DS3231 RTC module with embedded AT24C32 EEPROM
3. 16x2 LCD
4. YF-S201 flow sensor
5. Push Buttons


# Key features

Real-Time Water Flow Measurement

Uses a Hall-effect flow sensor to detect water flow.

Calculates instantaneous flow rate (L/min) and total volume consumed (Liters).

Time-Stamped Logging with DS3231 RTC

Accurate date and time tracking using the DS3231 real-time clock.

All readings are stored and referenced with timestamps for later processing.

LCD Display for Live Monitoring

16×2 LCD shows:

  1. Current flow rate

 2. Total consumption

 3. Date and time

 4. Alerts or status messages

EEPROM Data Storage

Stores total usage and calibration constant.

Data retained even after reset or power loss.


# Applications

Residential water usage monitoring

Apartment / hostel water billing

Agricultural irrigation tracking

Industrial water consumption management

Research laboratories and smart city projects


# System Workflow

Flow sensor generates pulses proportional to water flow

MCU counts pulses and calculates flow rate

RTC provides timestamp

LCD displays the live parameters

Data is stored in EEPROM

# Future Enhancements

Wireless data transmission (LoRa, ESP8266, ESP32)

Mobile app dashboard

Cloud-based water analytics

Automatic motor/pump control

Leak analytics using machine learning

Powering the system with solar cell

Developing more robust firmware for securing data [like professional electric meters]
