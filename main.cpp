#include "mbed.h"
#include "TextLCD.h"

// Pin Definitions
InterruptIn flowSensor(PA_4);     // YF-S201 sensor pin
DigitalIn button1(PA_1);          // Display/Mode button
DigitalIn button2(PA_2);          // Reset/Menu button
DigitalIn button3(PA_3);          // Select/Confirm button
I2C i2c(PB_11, PB_10);            // SDA, SCL for DS3231
TextLCD lcd(PA_8, PA_9, PB_12, PB_13, PB_14, PB_15); // RS, E, D4, D5, D6, D7

// EEPROM and RTC addresses
#define EEPROM_ADDRESS (0x57 << 1)  // AT24C32 EEPROM
#define RTC_ADDRESS (0x68 << 1)     // DS3231 RTC

// EEPROM Memory Map
#define ADDR_TOTAL_LITERS 0
#define ADDR_CURRENT_MONTH 4
#define ADDR_PREVIOUS_MONTH 8
#define ADDR_CURRENT_DAY 12
#define ADDR_CALIBRATION 16
#define ADDR_LAST_MONTH 20
#define ADDR_LAST_DAY 21
#define ADDR_COST_PER_LITER 22
#define ADDR_DAILY_LOG_START 30

// Flow sensor calibration
#define PULSES_PER_LITER 450.0f

// Display modes
enum DisplayMode {
  MODE_FLOW_RATE,
  MODE_SESSION,
  MODE_TODAY,
  MODE_THIS_MONTH,
  MODE_LAST_MONTH,
  MODE_TOTAL,
  MODE_COST_TODAY,
  MODE_COST_MONTH,
  MODE_WEEKLY,
  MODE_COUNT
};

// RTC DateTime structure
struct DateTime {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t dayOfWeek;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

// Global variables
volatile unsigned long pulseCount = 0;
float flowRate = 0.0f;
float sessionVolume = 0.0f;
DisplayMode currentMode = MODE_FLOW_RATE;
bool displayNeedsUpdate = true;

Timer flowTimer;
Timer saveTimer;
Timer displayTimer;
Timer debounceTimer;

// BCD conversion functions
uint8_t bcd2dec(uint8_t val) {
    return val - 6 * (val >> 4);
}

uint8_t dec2bcd(uint8_t val) {
    return val + 6 * (val / 10);
}

// EEPROM Functions
void writeEEPROM(uint16_t address, uint8_t data) {
    char cmd[3];
    cmd[0] = (address >> 8) & 0xFF;
    cmd[1] = address & 0xFF;
    cmd[2] = data;
    i2c.write(EEPROM_ADDRESS, cmd, 3);
    wait_ms(5);
}

uint8_t readEEPROM(uint16_t address) {
    char cmd[2];
    char data;
    cmd[0] = (address >> 8) & 0xFF;
    cmd[1] = address & 0xFF;
    i2c.write(EEPROM_ADDRESS, cmd, 2, true);
    i2c.read(EEPROM_ADDRESS, &data, 1);
    return data;
}

void writeFloat(uint16_t address, float value) {
    uint8_t* ptr = (uint8_t*)&value;
    for (int i = 0; i < 4; i++) {
        writeEEPROM(address + i, ptr[i]);
    }
}

float readFloat(uint16_t address) {
    float value;
    uint8_t* ptr = (uint8_t*)&value;
    for (int i = 0; i < 4; i++) {
        ptr[i] = readEEPROM(address + i);
    }
    return value;
}

void writeUnsignedLong(uint16_t address, unsigned long value) {
    uint8_t* ptr = (uint8_t*)&value;
    for (int i = 0; i < 4; i++) {
        writeEEPROM(address + i, ptr[i]);
    }
}

unsigned long readUnsignedLong(uint16_t address) {
    unsigned long value;
    uint8_t* ptr = (uint8_t*)&value;
    for (int i = 0; i < 4; i++) {
        ptr[i] = readEEPROM(address + i);
    }
    return value;
}

// RTC Functions
void rtcWrite(uint8_t reg, uint8_t data) {
    char cmd[2];
    cmd[0] = reg;
    cmd[1] = data;
    i2c.write(RTC_ADDRESS, cmd, 2);
}

uint8_t rtcRead(uint8_t reg) {
    char cmd = reg;
    char data;
    i2c.write(RTC_ADDRESS, &cmd, 1, true);
    i2c.read(RTC_ADDRESS, &data, 1);
    return data;
}

DateTime rtcGetTime() {
    DateTime dt;
    dt.second = bcd2dec(rtcRead(0x00) & 0x7F);
    dt.minute = bcd2dec(rtcRead(0x01));
    dt.hour = bcd2dec(rtcRead(0x02));
    dt.dayOfWeek = bcd2dec(rtcRead(0x03));
    dt.day = bcd2dec(rtcRead(0x04));
    dt.month = bcd2dec(rtcRead(0x05));
    dt.year = bcd2dec(rtcRead(0x06)) + 2000;
    return dt;
}

void rtcSetTime(DateTime dt) {
    rtcWrite(0x00, dec2bcd(dt.second));
    rtcWrite(0x01, dec2bcd(dt.minute));
    rtcWrite(0x02, dec2bcd(dt.hour));
    rtcWrite(0x03, dec2bcd(dt.dayOfWeek));
    rtcWrite(0x04, dec2bcd(dt.day));
    rtcWrite(0x05, dec2bcd(dt.month));
    rtcWrite(0x06, dec2bcd(dt.year - 2000));
}

float rtcGetTemperature() {
    int8_t msb = rtcRead(0x11);
    uint8_t lsb = rtcRead(0x12);
    return (float)msb + (lsb >> 6) * 0.25f;
}

// Interrupt handler for flow sensor
void flowPulseISR() {
    pulseCount++;
}

// Update flow measurements
void updateFlowMeasurements() {
    static uint32_t lastCheck = 0;
    uint32_t currentTime = flowTimer.read_ms();
    
    if (currentTime - lastCheck >= 1000) {
        unsigned long pulses = pulseCount;
        pulseCount = 0;
        
        uint32_t timeDiff = currentTime - lastCheck;
        flowRate = (pulses * 60.0f * 1000.0f) / (PULSES_PER_LITER * timeDiff);
        sessionVolume += (pulses / PULSES_PER_LITER);
        
        lastCheck = currentTime;
        displayNeedsUpdate = true;
    }
}

// Save data to EEPROM
void saveToEEPROM() {
    static uint32_t lastSave = 0;
    uint32_t currentTime = saveTimer.read();
    
    if (currentTime - lastSave >= 60) { // Save every 60 seconds
        DateTime now = rtcGetTime();
        
        unsigned long totalLiters = readUnsignedLong(ADDR_TOTAL_LITERS);
        unsigned long currentMonth = readUnsignedLong(ADDR_CURRENT_MONTH);
        unsigned long currentDay = readUnsignedLong(ADDR_CURRENT_DAY);
        uint8_t lastMonth = readEEPROM(ADDR_LAST_MONTH);
        uint8_t lastDay = readEEPROM(ADDR_LAST_DAY);
        
        unsigned long sessionLitersInt = (unsigned long)sessionVolume;
        totalLiters += sessionLitersInt;
        currentMonth += sessionLitersInt;
        currentDay += sessionLitersInt;
        sessionVolume -= sessionLitersInt;
        
        // Check if day changed
        if (now.day != lastDay) {
            uint8_t logIndex = (lastDay - 1) % 30;
            writeUnsignedLong(ADDR_DAILY_LOG_START + (logIndex * 4), currentDay);
            currentDay = 0;
            writeEEPROM(ADDR_LAST_DAY, now.day);
        }
        
        // Check if month changed
        if (now.month != lastMonth) {
            writeUnsignedLong(ADDR_PREVIOUS_MONTH, currentMonth);
            currentMonth = 0;
            writeEEPROM(ADDR_LAST_MONTH, now.month);
        }
        
        writeUnsignedLong(ADDR_TOTAL_LITERS, totalLiters);
        writeUnsignedLong(ADDR_CURRENT_MONTH, currentMonth);
        writeUnsignedLong(ADDR_CURRENT_DAY, currentDay);
        
        lastSave = currentTime;
        
        // Show save indicator briefly
        lcd.cls();
        lcd.locate(0, 0);
        lcd.printf("Saving...");
        wait(0.5);
        displayNeedsUpdate = true;
    }
}

// Display current mode
void displayCurrentMode() {
    static uint32_t lastDisplay = 0;
    uint32_t currentTime = displayTimer.read_ms();
    
    if (!displayNeedsUpdate && (currentTime - lastDisplay < 2000)) {
        return;
    }
    
    DateTime now = rtcGetTime();
    unsigned long totalLiters = readUnsignedLong(ADDR_TOTAL_LITERS);
    unsigned long currentMonth = readUnsignedLong(ADDR_CURRENT_MONTH);
    unsigned long previousMonth = readUnsignedLong(ADDR_PREVIOUS_MONTH);
    unsigned long currentDay = readUnsignedLong(ADDR_CURRENT_DAY);
    float costPerLiter = readFloat(ADDR_COST_PER_LITER);
    
    lcd.cls();
    
    // Show date/time on first line for 1 second
    lcd.locate(0, 0);
    lcd.printf("%02d/%02d %02d:%02d", now.day, now.month, now.hour, now.minute);
    wait(1.0);
    lcd.cls();

    switch (currentMode) {
        case MODE_FLOW_RATE:
            lcd.locate(0, 0);
            lcd.printf("FLOW RATE:");
            lcd.locate(0, 1);
            if (flowRate < 0.1f) {
                lcd.printf("[No Flow]");
            } else {
                lcd.printf("%.2f L/min", flowRate);
            }
            break;
            
        case MODE_SESSION:
            lcd.locate(0, 0);
            lcd.printf("SESSION VOL:");
            lcd.locate(0, 1);
            lcd.printf("%.3f L", sessionVolume);
            break;
            
        case MODE_TODAY:
            lcd.locate(0, 0);
            lcd.printf("TODAY:");
            lcd.locate(0, 1);
            lcd.printf("%lu L", currentDay);
            break;
            
        case MODE_THIS_MONTH:
            lcd.locate(0, 0);
            lcd.printf("THIS MONTH:");
            lcd.locate(0, 1);
            lcd.printf("%lu L", currentMonth);
            break;
            
        case MODE_LAST_MONTH:
            lcd.locate(0, 0);
            lcd.printf("LAST MONTH:");
            lcd.locate(0, 1);
            lcd.printf("%lu L", previousMonth);
            break;
            
        case MODE_TOTAL:
            lcd.locate(0, 0);
            lcd.printf("TOTAL:");
            lcd.locate(0, 1);
            lcd.printf("%lu L", totalLiters);
            break;
            
        case MODE_COST_TODAY:
            lcd.locate(0, 0);
            lcd.printf("TODAY COST:");
            lcd.locate(0, 1);
            lcd.printf("Rs %.2f", currentDay * costPerLiter);
            break;
            
        case MODE_COST_MONTH:
            lcd.locate(0, 0);
            lcd.printf("MONTH COST:");
            lcd.locate(0, 1);
            lcd.printf("Rs %.2f", currentMonth * costPerLiter);
            break;
            
        case MODE_WEEKLY:
            lcd.locate(0, 0);
            lcd.printf("LAST 7 DAYS:");
            // Show scrolling view of last 7 days
            for (int i = 0; i < 7; i++) {
                int dayIndex = (now.day - i - 1 + 30) % 30;
                unsigned long consumption = readUnsignedLong(ADDR_DAILY_LOG_START + (dayIndex * 4));
                lcd.locate(0, 1);
                lcd.printf("D-%d: %lu L    ", i, consumption);
                wait(1.5);
            }
            break;
    }
    
    displayNeedsUpdate = false;
    lastDisplay = currentTime;
}

// Initialize EEPROM
void initializeEEPROM() {
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("Initializing...");
    
    writeUnsignedLong(ADDR_TOTAL_LITERS, 0);
    writeUnsignedLong(ADDR_CURRENT_MONTH, 0);
    writeUnsignedLong(ADDR_PREVIOUS_MONTH, 0);
    writeUnsignedLong(ADDR_CURRENT_DAY, 0);
    writeFloat(ADDR_CALIBRATION, PULSES_PER_LITER);
    writeFloat(ADDR_COST_PER_LITER, 0.05f);
    
    DateTime now = rtcGetTime();
    writeEEPROM(ADDR_LAST_MONTH, now.month);
    writeEEPROM(ADDR_LAST_DAY, now.day);
    
    for (int i = 0; i < 30; i++) {
        writeUnsignedLong(ADDR_DAILY_LOG_START + (i * 4), 0);
    }
    
    lcd.locate(0, 1);
    lcd.printf("Done!");
    wait(2.0);
    displayNeedsUpdate = true;
}

// Show system info
void showSystemInfo() {
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("SYSTEM INFO");
    wait(1.5);
    
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("Calibration:");
    lcd.locate(0, 1);
    lcd.printf("%.1f pls/L", readFloat(ADDR_CALIBRATION));
    wait(2.0);
    
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("Cost Rate:");
    lcd.locate(0, 1); 
    lcd.printf("Rs %.3f/L", readFloat(ADDR_COST_PER_LITER));
    wait(2.0);
    
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("RTC Temp:");
    lcd.locate(0, 1);
    lcd.printf("%.2f C", rtcGetTemperature());
    wait(2.0);
    
    displayNeedsUpdate = true;
}

// Reset session counter
void resetSession() {
    sessionVolume = 0;
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("Session Reset!");
    wait(1.5);
    displayNeedsUpdate = true;
}

// Reset today's consumption
void resetToday() {
    writeUnsignedLong(ADDR_CURRENT_DAY, 0);
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("Today Reset!");
    wait(1.5);
    displayNeedsUpdate = true;
}

// Reset month's consumption
void resetMonth() {
    writeUnsignedLong(ADDR_CURRENT_MONTH, 0);
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("Month Reset!");
    wait(1.5);
    displayNeedsUpdate = true;
}

// Button handlers
void handleButtons() {
    static uint32_t lastBtn1 = 0, lastBtn2 = 0, lastBtn3 = 0;
    static bool btn1WasPressed = false, btn2WasPressed = false, btn3WasPressed = false;
    uint32_t currentTime = debounceTimer.read_ms();
    
    // Button 1 - Cycle display mode
    if (button1 == 0 && !btn1WasPressed && (currentTime - lastBtn1 > 200)) {
        currentMode = (DisplayMode)((currentMode + 1) % MODE_COUNT);
        displayNeedsUpdate = true;
        lastBtn1 = currentTime;
        btn1WasPressed = true;
    } else if (button1 == 1) {
        btn1WasPressed = false;
    }
    
    // Button 2 - Reset session counter (short press)
    if (button2 == 0 && !btn2WasPressed && (currentTime - lastBtn2 > 200)) {
        resetSession();
        lastBtn2 = currentTime;
        btn2WasPressed = true;
    } else if (button2 == 1) {
        btn2WasPressed = false;
    }
    
    // Button 3 - Show system info (short press)
    if (button3 == 0 && !btn3WasPressed && (currentTime - lastBtn3 > 200)) {
        showSystemInfo();
        lastBtn3 = currentTime;
        btn3WasPressed = true;
    } else if (button3 == 1) {
        btn3WasPressed = false;
    }
}

// Check for long button press (hold for 3 seconds)
void checkLongPress() {
    static uint32_t btn2PressStart = 0;
    static uint32_t btn3PressStart = 0;
    static bool btn2LongHandled = false;
    static bool btn3LongHandled = false;
    
    uint32_t currentTime = debounceTimer.read_ms();
    
    // Button 2 long press - Reset today
    if (button2 == 0) {
        if (btn2PressStart == 0) {
            btn2PressStart = currentTime;
        } else if (!btn2LongHandled && (currentTime - btn2PressStart > 3000)) {
            resetToday();
            btn2LongHandled = true;
        }
    } else {
        btn2PressStart = 0;
        btn2LongHandled = false;
    }
    
    // Button 3 long press - Initialize EEPROM
    if (button3 == 0) {
        if (btn3PressStart == 0) {
            btn3PressStart = currentTime;
        } else if (!btn3LongHandled && (currentTime - btn3PressStart > 3000)) {
            lcd.cls();
            lcd.locate(0, 0);
            lcd.printf("Hold 2 more sec");
            lcd.locate(0, 1);
            lcd.printf("to factory reset");
            wait(2.0);
            if (button3 == 0) {
                initializeEEPROM();
            }
            btn3LongHandled = true;
        }
    } else {
        btn3PressStart = 0;
        btn3LongHandled = false;
    }
}

// Main function
int main() {
    // Enable internal pull-ups for buttons
    button1.mode(PullUp);
    button2.mode(PullUp);
    button3.mode(PullUp);
    
    // Startup screen
    lcd.cls();
    lcd.locate(0, 0);
    lcd.printf("WATER METER");
    lcd.locate(0, 1);
    lcd.printf("System v2.0");
    wait(2.0);
    
    // Initialize I2C
    i2c.frequency(100000); // 100kHz
    
    // Attach interrupt for flow sensor
    flowSensor.rise(&flowPulseISR);
    
    // Start timers
    flowTimer.start();
    saveTimer.start();
    displayTimer.start();
    debounceTimer.start();
    
    // Show initial display
    displayNeedsUpdate = true;
    displayCurrentMode();
    
    while (1) {
        updateFlowMeasurements();
        saveToEEPROM();
        handleButtons();
        checkLongPress();
        
        // Auto-refresh display every 5 seconds
        static uint32_t lastAutoRefresh = 0;
        uint32_t currentDisplayTime = displayTimer.read_ms();
        if (currentDisplayTime - lastAutoRefresh >= 5000) {
            displayNeedsUpdate = true;
            lastAutoRefresh = currentDisplayTime;
        }
        displayCurrentMode();
        
        wait_ms(50); // Small delay to prevent CPU hogging
    }
}
