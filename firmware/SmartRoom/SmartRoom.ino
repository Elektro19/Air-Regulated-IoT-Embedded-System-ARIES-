#include <ld2410.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>
#include "DHT.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define MONITOR_SERIAL Serial
#define RADAR_SERIAL Serial1
#define RADAR_RX_PIN 16
#define RADAR_TX_PIN 17
#define PIR_PIN 27
#define RELAY_PIN_1 25
#define RELAY_PIN_2 26
#define MANUAL_SWITCH_1 32 // Controls LED Mode
#define MANUAL_SWITCH_2 33 // Controls Fan Mode

// --- DHT11 Temperature Sensors (Outdoor + Indoor) ---
#define DHTPIN_OUTDOOR 4   // <-- verify this pin is free on your board/wiring
#define DHTPIN_INDOOR 5    // <-- verify this pin is free on your board/wiring
#define DHTTYPE DHT11

// --- OLED Display ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C  // <-- verify against your specific module; some use 0x3D


Adafruit_INA219 ina219;
ld2410 radar;
Preferences preferences;
DHT dhtOutdoor(DHTPIN_OUTDOOR, DHTTYPE);
DHT dhtIndoor(DHTPIN_INDOOR, DHTTYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledAvailable = false;

const float TIME_SPEED_MULTIPLIER = 43200.0f;
// Scale prototype watts up to a realistic ROOM load (not a whole house):
// prototype draws ~1.7 W with both loads on; x90 ≈ 150 W, i.e. an LED-lit room
// (~75 W lighting + ~75 W fan). Even running nonstop for a simulated month
// that's ~110 kWh ≈ RM 21 under RP4 — a believable single-room share of a bill.
const float SIMULATION_SCALE_FACTOR = 90.0f;
unsigned long lastMotionTime = 0;
const unsigned long offDelay = 3000; // exhibition: 3 s here + 2 s radar hold ≈ 5 s total until loads drop
uint32_t lastReading = 0;
float accumulatedKWh = 0.0f;  // LIFETIME METER REGISTER - never reset, mirrors a real TNB meter

float simulatedDaysElapsed = 0.0f;
float lastSavedKWh = 0.0f;
bool autoloadState = false;
bool simulatedOutage = false; // set/cleared by "OUTAGE"/"RESTORE" commands from the dashboard

// --- Billing cycle tracking (separate from the lifetime meter) ---
float billingCycleStartKWh = 0.0f; // meter reading snapshot at the start of the current billing cycle

// --- Time-of-Use split (RP4 ToU: peak = 2pm-10pm weekdays, off-peak otherwise) ---
float cyclePeakKWh = 0.0f;
float cycleOffKWh  = 0.0f;

float totalEnergy_Wh = 0;
float lastSavedEnergy_Wh = 0;
unsigned long previousSaveMillis = 0;
const unsigned long SAVE_INTERVAL = 5000; // 5 seconds
unsigned long previousEnergyCalculationTime = 0;

// --- DHT11 temperature state ---
const float TEMP_DIFF_THRESHOLD = 2.0f; // outdoor must be at least this many °C cooler than indoor before fan auto-activates
const unsigned long TEMP_READ_INTERVAL = 2000; // DHT11 is slow (per Adafruit example) - don't sample faster than this
unsigned long lastTempReadTime = 0;
float outdoorTempC = 0.0f;
float indoorTempC = 0.0f;
bool tempSensorsValid = false;

// --- Energy Saving Counter ("Today") ---
// "Today" is tracked using REAL elapsed time (not the sped-up simulated days),
// since the simulation compresses a full simulated day into only ~2 real seconds.
unsigned long todayTrackingStartMillis = 0;
const unsigned long DAY_IN_MS = 86400000UL; // 24 real hours
float todayEnergy_Wh = 0.0f;             // actual scaled energy consumed today
float baselineEnergy_Wh = 0.0f;          // hypothetical energy if left running continuously today
float estimatedEnergySaved_Wh = 0.0f;
float lastKnownActivePowerWatts = 0.0f;  // learns the device's "on" wattage from real readings

// Variables to handle the 3-state manual switching logic
enum DeviceMode { AUTOMATION, FORCE_ON, FORCE_OFF };
DeviceMode ledMode = AUTOMATION;
DeviceMode fanMode = AUTOMATION;

unsigned long switch1DownTime = 0;
unsigned long switch2DownTime = 0;
bool lastSwitch1State = HIGH;
bool lastSwitch2State = HIGH;

struct TNBBill {
    float baseCost;      // energy + capacity + network charges
    float eeiRebate;     // Energy Efficiency Incentive (subtracted from the bill)
    float retailCharge;
    float afaCharge;
    float kwtbbCharge;   // renewable energy fund 1.6%
    float serviceTax;
    float totalBill;
};

// RP4 Energy Efficiency Incentive (domestic, effective 1 July 2025):
// a per-kWh rebate applied to the WHOLE month's usage, with the rate picked
// from the bracket the total usage falls in. Zero above 1000 kWh.
float eeiRatePerKWh(float kwh) {
    if (kwh <= 0)     return 0.0f;
    if (kwh <= 200)   return 0.250f;
    if (kwh <= 250)   return 0.245f;
    if (kwh <= 300)   return 0.225f;
    if (kwh <= 350)   return 0.210f;
    if (kwh <= 400)   return 0.170f;
    if (kwh <= 450)   return 0.145f;
    if (kwh <= 500)   return 0.120f;
    if (kwh <= 550)   return 0.105f;
    if (kwh <= 600)   return 0.090f;
    if (kwh <= 650)   return 0.075f;
    if (kwh <= 700)   return 0.055f;
    if (kwh <= 750)   return 0.045f;
    if (kwh <= 800)   return 0.040f;
    if (kwh <= 850)   return 0.025f;
    if (kwh <= 900)   return 0.010f;
    if (kwh <= 1000)  return 0.005f;
    return 0.0f;
}

TNBBill calculateTNBBill(float kwh) {
    TNBBill bill;

    // --- TNB RP4 domestic tariff (effective 1 July 2025), unbundled charges ---
    // Energy charge : 27.03 sen/kWh (usage <= 1500 kWh) or 37.03 sen/kWh (above)
    // Capacity      :  4.55 sen/kWh
    // Network       : 12.85 sen/kWh
    // (27.03 + 4.55 + 12.85 = the familiar 44.43 sen/kWh headline rate)
    float energyRate = (kwh <= 1500.0f) ? 0.2703f : 0.3703f;
    const float capacityRate = 0.0455f;
    const float networkRate  = 0.1285f;
    bill.baseCost = kwh * (energyRate + capacityRate + networkRate);

    // Energy Efficiency Incentive rebate (this is what makes small bills cheap)
    bill.eeiRebate = kwh * eeiRatePerKWh(kwh);

    // Retail charge RM10/month, waived when usage <= 600 kWh
    bill.retailCharge = (kwh > 600.0f) ? 10.00f : 0.00f;

    // AFA (Automatic Fuel Adjustment): varies month to month, domestic customers
    // only pay it above 600 kWh. Currently 0 — update to the published rate.
    const float afaRatePerKWh = 0.0f;
    bill.afaCharge = (kwh > 600.0f) ? (kwh * afaRatePerKWh) : 0.00f;

    float subTotal = bill.baseCost - bill.eeiRebate + bill.retailCharge + bill.afaCharge;

    // KWTBB (Renewable Energy Fund): 1.6%, only when usage > 300 kWh
    bill.kwtbbCharge = (kwh > 300.0f) ? (subTotal * 0.016f) : 0.0f;

    // Service tax 8%: only on the PORTION of usage above 600 kWh, not the whole bill
    bill.serviceTax = 0.0f;
    if (kwh > 600.0f) {
        float taxableFraction = (kwh - 600.0f) / kwh;
        bill.serviceTax = ((bill.baseCost - bill.eeiRebate + bill.afaCharge) * taxableFraction
                           + bill.retailCharge) * 0.08f;
    }

    bill.totalBill = subTotal + bill.kwtbbCharge + bill.serviceTax;

    // TNB minimum monthly charge
    if (kwh > 0.0f && bill.totalBill < 3.00f) {
        bill.totalBill = 3.00f;
    }
    return bill;
}

// RP4 domestic Time-of-Use option: peak energy 28.52 sen/kWh, off-peak 24.43
// (38.52 / 34.43 above 1500 kWh total). Capacity/network/EEI/retail/KWTBB/SST
// are unchanged, so start from the standard bill and swap the energy component.
TNBBill calculateTNBBillToU(float peakKWh, float offKWh) {
    float kwh = peakKWh + offKWh;
    TNBBill bill = calculateTNBBill(kwh);
    float peakRate = (kwh <= 1500.0f) ? 0.2852f : 0.3852f;
    float offRate  = (kwh <= 1500.0f) ? 0.2443f : 0.3443f;
    float stdRate  = (kwh <= 1500.0f) ? 0.2703f : 0.3703f;
    float diff = (peakKWh * peakRate + offKWh * offRate) - (kwh * stdRate);
    bill.baseCost  += diff;
    bill.totalBill += diff; // KWTBB/SST knock-on from the diff is negligible at room scale
    return bill;
}

// --- Tariff-tier-aware automation ---
// As cycle usage climbs toward boundaries where electricity gets more expensive
// (EEI best bracket ends at 200 kWh, KWTBB starts >300, retail+SST+AFA >600),
// the automation gets progressively more aggressive about switching off.
uint8_t conservationLevel(float cycleKWh) {
    if (cycleKWh > 600.0f) return 3;
    if (cycleKWh > 300.0f) return 2;
    if (cycleKWh > 200.0f) return 1;
    return 0;
}

const char* modeName(DeviceMode m) {
  if (m == FORCE_ON) return "ON";
  if (m == FORCE_OFF) return "OFF";
  return "AUTO";
}

void setup(void)
{
  MONITOR_SERIAL.begin(115200); // Feedback over Serial Monitor
  // radar.debug(MONITOR_SERIAL); Uncomment to show debug information from the library on the Serial Monitor. By default this does not show sensor reads as they are very frequent.
  Wire.begin(21,22);
  Wire.setClock(100000);
  delay(100);

  preferences.begin("Energy", false);
  if (!ina219.begin()) {
    Serial.println("INA219 NOT FOUND!");
    while(1);
  }

  // --- OLED init (shares the same I2C bus as the INA219) ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 OLED not found - check wiring/address"));
    oledAvailable = false;
  } else {
    oledAvailable = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Smart Room Booting..."));
    display.display();
  }

  accumulatedKWh = preferences.getFloat("totalEnergyKWh", 0.0f);
  billingCycleStartKWh = preferences.getFloat("cycleStartKWh", 0.0f);
  cyclePeakKWh = preferences.getFloat("cyclePeakKWh", 0.0f);
  cycleOffKWh  = preferences.getFloat("cycleOffKWh", 0.0f);
  simulatedDaysElapsed = preferences.getFloat("simDays", 0.0f);
  lastSavedKWh = accumulatedKWh;
  previousEnergyCalculationTime = millis();
  totalEnergy_Wh = preferences.getFloat("totalEnergy",0.0);
  lastSavedEnergy_Wh = totalEnergy_Wh;
  todayTrackingStartMillis = millis();

  MONITOR_SERIAL.println("\n========================================");
  Serial.println("   TNB ACCELERATED DEMO UTILITY METER   ");
  Serial.println("========================================");
  Serial.print("Restored Lifetime Meter Reading: ");
  Serial.print(accumulatedKWh, 4);
  Serial.println(" kWh");

  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(RELAY_PIN_1, OUTPUT);
  pinMode(RELAY_PIN_2, OUTPUT);
  pinMode(MANUAL_SWITCH_1, INPUT_PULLUP);
  pinMode(MANUAL_SWITCH_2, INPUT_PULLUP);

  digitalWrite(RELAY_PIN_1, LOW);
  digitalWrite(RELAY_PIN_2, LOW); // Relay OFF — board jumpers S1/S2 set to H (active-HIGH trigger)

  // --- Initialize DHT11 sensors ---
  dhtOutdoor.begin();
  dhtIndoor.begin();

  RADAR_SERIAL.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN); // UART for monitoring the radar
  delay(500);
  MONITOR_SERIAL.print(F("\nConnect LD2410 radar TX to GPIO:"));
  MONITOR_SERIAL.println(RADAR_RX_PIN);
  MONITOR_SERIAL.print(F("Connect LD2410 radar RX to GPIO:"));
  MONITOR_SERIAL.println(RADAR_TX_PIN);
  MONITOR_SERIAL.print(F("LD2410 radar sensor initialising: "));

  if (radar.begin(RADAR_SERIAL))
  {
    MONITOR_SERIAL.println(F("OK"));
    MONITOR_SERIAL.print(F("LD2410 firmware version: "));
    MONITOR_SERIAL.print(radar.firmware_major_version);
    MONITOR_SERIAL.print('.');
    MONITOR_SERIAL.print(radar.firmware_minor_version);
    MONITOR_SERIAL.print('.');
    MONITOR_SERIAL.println(radar.firmware_bugfix_version, HEX);

    // Set the radar's own "unattended duration" (inactivity timer) to 10 s.
    // setMaxValues(maxMovingGate, maxStationaryGate, inactivitySeconds) —
    // gates are 0.75 m each; 8 = full range. Lower the first two numbers
    // to shrink detection range if the radar is too sensitive.
    // This is written to the module's flash, so it persists across power cycles.
    MONITOR_SERIAL.print(F("Setting radar unattended duration to 2s: "));
    if (radar.setMaxValues(8, 8, 2)) {
      MONITOR_SERIAL.println(F("OK"));
      radar.requestRestart(); // radar reboots so the new config takes effect
    } else {
      MONITOR_SERIAL.println(F("FAILED"));
    }
  }
  else
  {
    MONITOR_SERIAL.println(F("not connected"));
  }
}

void loop()
{
  // --- Commands arriving from the USB dashboard (non-blocking line reader) ---
  static String cmdBuf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBuf == "OUTAGE")       simulatedOutage = true;
      else if (cmdBuf == "RESTORE") simulatedOutage = false;
      cmdBuf = "";
    } else if (cmdBuf.length() < 32) {
      cmdBuf += c;
    }
  }

  bool pirMotion = digitalRead(PIR_PIN);
  radar.read();
  bool roomOccupied = false;
  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint > 1000) {
    lastDebugPrint = millis();
    Serial.print(F("[DEBUG] PIR raw: "));
    Serial.print(pirMotion);
    Serial.print(F(" | Radar connected: "));
    Serial.print(radar.isConnected());
    Serial.print(F(" | Radar presence: "));
    Serial.println(radar.isConnected() ? radar.presenceDetected() : false);
  }

  // --- READ PHYSICAL SWITCHES & DECODE 3 STATES ---
  bool s1 = digitalRead(MANUAL_SWITCH_1);
  bool s2 = digitalRead(MANUAL_SWITCH_2);

  // Switch 1 (LED) Logic
  if (s1 == LOW && lastSwitch1State == HIGH) { // Just flipped DOWN
    switch1DownTime = millis();
    ledMode = FORCE_OFF; // Default to Force OFF while held down
  }
  else if (s1 == HIGH && lastSwitch1State == LOW) { // Just flipped back UP
    if (millis() - switch1DownTime < 1000) {
      // If it was a quick flick down and up, toggle between FORCE_ON and AUTOMATION
      ledMode = (ledMode == FORCE_ON) ? AUTOMATION : FORCE_ON;
    } else {
      ledMode = AUTOMATION; // Released from long hold, back to automation
    }
  }
  lastSwitch1State = s1;

   // Switch 2 (Fan) Logic — 3-state, same as switch 1:
  // hold down = FORCE_OFF, quick flick = FORCE_ON <-> AUTOMATION
  if (s2 == LOW && lastSwitch2State == HIGH) {
    switch2DownTime = millis();
    fanMode = FORCE_OFF;
  }
  else if (s2 == HIGH && lastSwitch2State == LOW) {
    if (millis() - switch2DownTime < 1000) {
      fanMode = (fanMode == FORCE_ON) ? AUTOMATION : FORCE_ON;
    } else {
      fanMode = AUTOMATION;
    }
  }
  lastSwitch2State = s2;

  // --- Tariff-tier-aware automation: tighten behavior as this cycle's usage
  // approaches expensive tariff boundaries (uses last iteration's meter values) ---
  float cycleKWhSoFar = accumulatedKWh - billingCycleStartKWh;
  if (cycleKWhSoFar < 0.0f) cycleKWhSoFar = 0.0f;
  uint8_t ecoLevel = conservationLevel(cycleKWhSoFar);
  unsigned long effectiveOffDelay = offDelay >> ecoLevel;              // halves per tier
  float effectiveTempThreshold = TEMP_DIFF_THRESHOLD + (float)ecoLevel; // +1 degC per tier


  // --- Periodic DHT11 temperature read (throttled — DHT11 is a slow sensor) ---
  if (millis() - lastTempReadTime > TEMP_READ_INTERVAL) {
    lastTempReadTime = millis();
    float newOutdoorTemp = dhtOutdoor.readTemperature();
    float newIndoorTemp = dhtIndoor.readTemperature();

    // DHT library returns NAN if a read fails (bad wiring, timing glitch, etc.)
    if (!isnan(newOutdoorTemp) && !isnan(newIndoorTemp)) {
      outdoorTempC = newOutdoorTemp;
      indoorTempC = newIndoorTemp;
      tempSensorsValid = true;
    } else {
      tempSensorsValid = false;
      Serial.println(F("⚠️ DHT11 read failed — check wiring"));
    }
  }

  // --- "Today" real-day rollover check (independent of simulated time speed) ---
  if (millis() - todayTrackingStartMillis >= DAY_IN_MS) {
    todayTrackingStartMillis = millis();
    todayEnergy_Wh = 0.0f;
    baselineEnergy_Wh = 0.0f;
  }

  //Process Radar data every 1000ms
  if (radar.isConnected() && millis() - lastReading > 5000) // Report every 1000ms
  {
    lastReading = millis();
    if (radar.presenceDetected())
    {
      if (radar.stationaryTargetDetected())
      {
        Serial.print(F("Stationary target: "));
        Serial.print(radar.stationaryTargetDistance());
        Serial.print(F("cm energy:"));
        Serial.print(radar.stationaryTargetEnergy());
        Serial.print(' ');
      }
      if (radar.movingTargetDetected())
      {
        Serial.print(F("Moving target: "));
        Serial.print(radar.movingTargetDistance());
        Serial.print(F("cm energy:"));
        Serial.print(radar.movingTargetEnergy());
      }
      Serial.println();
    }
    else
    {
      Serial.println(F("No human presence detected"));
    }
  }
  if (pirMotion || (radar.isConnected() && radar.presenceDetected())) // 4. COMBINE SENSORS: Room is occupied if PIR sees motion OR Radar sees presence
  {
    roomOccupied = true;
  }
  if (roomOccupied) // 5. Control the Relay State based on combined occupancy
  {
    lastMotionTime = millis();
    autoloadState = true;
  }
  if (!roomOccupied && autoloadState && (millis() - lastMotionTime > effectiveOffDelay)) // If room is empty, check if the (tier-adjusted) off-delay timeout has expired
  {
    autoloadState = false;
  }

   // --- ASSIGN FINAL OUTPUT STATES ---
  bool physicalLEDOutput = false;
  bool physicalFanOutput = false;

  // LED Output Logic Block
  if (ledMode == FORCE_ON) physicalLEDOutput = true;
  else if (ledMode == FORCE_OFF) physicalLEDOutput = false;
  else physicalLEDOutput = autoloadState; // AUTOMATION MODE

  // Fan Output Logic Block
  if (fanMode == FORCE_ON) physicalFanOutput = true;
  else if (fanMode == FORCE_OFF) physicalFanOutput = false;
  else {
    // AUTOMATION MODE: fan only auto-activates if room is occupied AND
    // outdoor is at least TEMP_DIFF_THRESHOLD °C cooler than indoor
    bool outdoorSignificantlyCooler = tempSensorsValid && ((indoorTempC - outdoorTempC) >= effectiveTempThreshold);
    physicalFanOutput = autoloadState && outdoorSignificantlyCooler;
  }

  // NOTE: the old "radar disconnected -> follow raw switch level" fallback was removed.
  // It overwrote the occupancy + temperature logic every loop whenever the radar was
  // down, and with INPUT_PULLUP switches it forced both loads permanently ON.
  // PIR-based automation still works without the radar, and FORCE_ON/FORCE_OFF via
  // the switches already provides deliberate manual override per device.

  // Grid-loss load shedding (simulated event from the dashboard): the fan
  // (higher-draw channel) is force-cut; the LED stays under normal automation.
  if (simulatedOutage) {
    physicalFanOutput = false;
  }

  // Drive Output Pins (Active-HIGH — board jumpers S1/S2 set to H)
  digitalWrite(RELAY_PIN_1, physicalLEDOutput ? HIGH : LOW);
  digitalWrite(RELAY_PIN_2, physicalFanOutput ? HIGH : LOW);

  float busVoltage = ina219.getBusVoltage_V();

  float shuntVoltage = ina219.getShuntVoltage_mV();

  float loadVoltage = busVoltage + (shuntVoltage/1000);

  float power_mW = ina219.getPower_mW();

  float power_W = power_mW / 1000.0;

  float current_mA = ina219.getCurrent_mA();

  // 12V supply detection: with only USB connected the relays still click, but the
  // loads have no power. Gate the DISPLAYED LED/fan state on real bus voltage so
  // the OLED and dashboard show what the loads are truly doing.
  bool supplyPresent = loadVoltage > 5.0f; // must stay below the ~6.5V the 6xNiMH backup provides through its diode
  bool ledActuallyOn = physicalLEDOutput && supplyPresent;
  bool fanActuallyOn = physicalFanOutput && supplyPresent;

  unsigned long currentTime = millis();
  float elapsedHours = (currentTime - previousEnergyCalculationTime) / 3600000.0;
  previousEnergyCalculationTime = currentTime;

  float simulatedTimeDeltaHours = elapsedHours * TIME_SPEED_MULTIPLIER;
  simulatedDaysElapsed += (simulatedTimeDeltaHours / 24.0f);

  totalEnergy_Wh += power_W * elapsedHours;
  float totalEnergy_kWh = totalEnergy_Wh/1000;

  // Prevent random hardware current noise calculation spikes when off
  float cleanCurrentAmps = (current_mA < 5.0f) ? 0.0f : (current_mA / 1000.0f);
  float instantaneousPowerWatts = loadVoltage * cleanCurrentAmps;

  // Scale up small prototype current draw to mimic massive household main appliances
  float simulatedPowerWatts = instantaneousPowerWatts * SIMULATION_SCALE_FACTOR;
  float energyCalculatedKWh = (simulatedPowerWatts * simulatedTimeDeltaHours) / 1000.0f;
  accumulatedKWh += energyCalculatedKWh; // lifetime meter register - always accumulates, never reset

  // --- ToU split: which simulated tariff window is this energy landing in? ---
  float dayFrac = simulatedDaysElapsed - floorf(simulatedDaysElapsed);
  float simHour = dayFrac * 24.0f;
  bool  isWeekday = (((int)simulatedDaysElapsed) % 7) < 5; // days 0-4 weekdays, 5-6 weekend
  bool  isPeakHour = isWeekday && (simHour >= 14.0f) && (simHour < 22.0f);
  if (isPeakHour) cyclePeakKWh += energyCalculatedKWh;
  else            cycleOffKWh  += energyCalculatedKWh;

  // --- Energy Saving Counter calculations (today's window only) ---
  if ((physicalLEDOutput || physicalFanOutput) && simulatedPowerWatts > 0.0f) {
    lastKnownActivePowerWatts = simulatedPowerWatts;
  }
  todayEnergy_Wh += simulatedPowerWatts * simulatedTimeDeltaHours;
  baselineEnergy_Wh += lastKnownActivePowerWatts * simulatedTimeDeltaHours;
  estimatedEnergySaved_Wh = baselineEnergy_Wh - todayEnergy_Wh;
  if (estimatedEnergySaved_Wh < 0.0f) estimatedEnergySaved_Wh = 0.0f;

  // Compute this billing cycle's usage (meter difference), NOT the lifetime total —
  // mirrors how a real TNB bill is calculated from two meter readings
  float currentCycleKWh = accumulatedKWh - billingCycleStartKWh;
  if (currentCycleKWh < 0.0f) currentCycleKWh = 0.0f;
  TNBBill liveBill = calculateTNBBill(currentCycleKWh);
  TNBBill todayBill = calculateTNBBill(todayEnergy_Wh / 1000.0f);
  TNBBill touBill = calculateTNBBillToU(cyclePeakKWh, cycleOffKWh);

  // --- REFRESH DATA DASHBOARD (SERIAL + OLED + USB JSON TELEMETRY) ONCE PER REAL SECOND ---
  static unsigned long lastDisplayTime = 0;
  if (currentTime - lastDisplayTime >= 1000) {
    lastDisplayTime = currentTime;

    if (oledAvailable) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.print(F("Occ:")); display.print(autoloadState ? F("YES") : F("NO"));
      display.print(F(" LED:")); display.println(ledActuallyOn ? F("ON") : F("OFF"));
      display.print(F("Fan:")); display.print(fanActuallyOn ? F("ON") : F("OFF"));
      display.println(supplyPresent ? F("") : F(" NO12V"));
      if (tempSensorsValid) {
        display.print(F("In:")); display.print(indoorTempC, 1);
        display.print(F("C Out:")); display.print(outdoorTempC, 1); display.println(F("C"));
      } else {
        display.println(F("Temp: sensor err"));
      }
      display.print(F("Today:")); display.print(todayEnergy_Wh, 1); display.println(F("Wh"));
      display.print(F("Saved:")); display.print(estimatedEnergySaved_Wh, 1); display.println(F("Wh"));
      display.print(F("Meter:")); display.print(accumulatedKWh, 2); display.println(F("kWh"));
      display.print(F("RM ")); display.println(liveBill.totalBill, 2);
      display.display();
    }

    // --- JSON TELEMETRY LINE FOR USB WEB DASHBOARD ---
    // One compact machine-readable line per second. The web dashboard parses
    // any serial line that starts with {"tel" and ignores everything else.
    Serial.print(F("{\"tel\":1"));
    Serial.print(F(",\"occ\":"));  Serial.print(autoloadState ? 1 : 0);
    Serial.print(F(",\"pir\":"));  Serial.print(pirMotion ? 1 : 0);
    Serial.print(F(",\"rc\":"));   Serial.print(radar.isConnected() ? 1 : 0);
    Serial.print(F(",\"rp\":"));   Serial.print((radar.isConnected() && radar.presenceDetected()) ? 1 : 0);
    Serial.print(F(",\"led\":"));  Serial.print(ledActuallyOn ? 1 : 0);
    Serial.print(F(",\"fan\":"));  Serial.print(fanActuallyOn ? 1 : 0);
    Serial.print(F(",\"sup\":"));  Serial.print(supplyPresent ? 1 : 0);
    Serial.print(F(",\"lm\":\"")); Serial.print(modeName(ledMode));
    Serial.print(F("\",\"fm\":\"")); Serial.print(modeName(fanMode));
    Serial.print(F("\",\"tok\":")); Serial.print(tempSensorsValid ? 1 : 0);
    Serial.print(F(",\"tin\":"));  Serial.print(indoorTempC, 1);
    Serial.print(F(",\"tout\":")); Serial.print(outdoorTempC, 1);
    Serial.print(F(",\"v\":"));    Serial.print(loadVoltage, 2);
    Serial.print(F(",\"i\":"));    Serial.print(current_mA, 1);
    Serial.print(F(",\"pw\":"));   Serial.print(instantaneousPowerWatts, 3);
    Serial.print(F(",\"hw\":"));   Serial.print(simulatedPowerWatts, 1);
    Serial.print(F(",\"twh\":"));  Serial.print(todayEnergy_Wh, 2);
    Serial.print(F(",\"swh\":"));  Serial.print(estimatedEnergySaved_Wh, 2);
    Serial.print(F(",\"trm\":"));  Serial.print(todayBill.totalBill, 2);
    Serial.print(F(",\"mkwh\":")); Serial.print(accumulatedKWh, 4);
    Serial.print(F(",\"ckwh\":")); Serial.print(currentCycleKWh, 4);
    Serial.print(F(",\"crm\":"));  Serial.print(liveBill.totalBill, 2);
    Serial.print(F(",\"days\":")); Serial.print(simulatedDaysElapsed, 2);
    Serial.print(F(",\"grid\":")); Serial.print(simulatedOutage ? 0 : 1);
    Serial.print(F(",\"eco\":"));  Serial.print(ecoLevel);
    Serial.print(F(",\"pk\":"));   Serial.print(cyclePeakKWh, 3);
    Serial.print(F(",\"ok\":"));   Serial.print(cycleOffKWh, 3);
    Serial.print(F(",\"tou\":"));  Serial.print(touBill.totalBill, 2);
    Serial.print(F(",\"sd\":"));   Serial.print((radar.isConnected() && radar.stationaryTargetDetected()) ? radar.stationaryTargetDistance() : 0);
    Serial.print(F(",\"se\":"));   Serial.print((radar.isConnected() && radar.stationaryTargetDetected()) ? radar.stationaryTargetEnergy() : 0);
    Serial.print(F(",\"md\":"));   Serial.print((radar.isConnected() && radar.movingTargetDetected()) ? radar.movingTargetDistance() : 0);
    Serial.print(F(",\"me\":"));   Serial.print((radar.isConnected() && radar.movingTargetDetected()) ? radar.movingTargetEnergy() : 0);
    Serial.println(F("}"));

    // --- Human-readable serial report (moved inside the 1-second gate so it
    //     no longer floods the monitor on every 200ms loop pass) ---
    Serial.println("\n--- TNB ACCELERATED REAL-TIME BILLING ENGINE ---");
    Serial.print("📅 Simulated Time   : "); Serial.print(simulatedDaysElapsed, 1); Serial.println(" / 30.0 Days (billing cycle)");

    Serial.print("💡 LED Room Light  : ");
    if (ledMode == FORCE_ON) Serial.println("MANUAL FORCED ON");
    else if (ledMode == FORCE_OFF) Serial.println("MANUAL FORCED OFF (COLD/FEVER)");
    else Serial.println(autoloadState ? "AUTOMATION ON" : "AUTOMATION OFF");

    Serial.print("🛸 Cooling Ventilation: ");
    if (fanMode == FORCE_ON) Serial.println("MANUAL FORCED ON");
    else if (fanMode == FORCE_OFF) Serial.println("MANUAL FORCED OFF (COLD/FEVER)");
    else Serial.println(physicalFanOutput ? "AUTOMATION ON" : "AUTOMATION OFF");

    Serial.print("🌡️ Outdoor Temp     : ");
    if (tempSensorsValid) { Serial.print(outdoorTempC, 1); Serial.println(" °C"); }
    else Serial.println("SENSOR ERROR");

    Serial.print("🌡️ Indoor Temp      : ");
    if (tempSensorsValid) { Serial.print(indoorTempC, 1); Serial.println(" °C"); }
    else Serial.println("SENSOR ERROR");

    Serial.print("🔌 Prototype Power  : "); Serial.print(instantaneousPowerWatts); Serial.println(" W");
    Serial.print("🏠 Scaled House Power: "); Serial.print(simulatedPowerWatts); Serial.println(" W");
    Serial.println("-------------------------------------------------");
    Serial.print("🔋 Today's Energy   : "); Serial.print(todayEnergy_Wh, 2); Serial.println(" Wh");
    Serial.print("💵 Today's Cost     : RM "); Serial.println(todayBill.totalBill, 2);
    Serial.print("🌱 Estimated Saved  : "); Serial.print(estimatedEnergySaved_Wh, 2); Serial.println(" Wh");
    Serial.println("-------------------------------------------------");
    Serial.print("⚡ Lifetime Meter Reading : "); Serial.print(accumulatedKWh, 3); Serial.println(" kWh (never resets)");
    Serial.print("📊 This Billing Cycle     : "); Serial.print(currentCycleKWh, 3); Serial.println(" kWh");
    Serial.print("💵 Projected TNB Invoice  : RM "); Serial.println(liveBill.totalBill, 2);
    Serial.print("⏰ Peak / Off-peak kWh    : "); Serial.print(cyclePeakKWh, 3); Serial.print(" / "); Serial.println(cycleOffKWh, 3);
    Serial.print("💵 If on ToU tariff       : RM "); Serial.println(touBill.totalBill, 2);
    Serial.print("🌿 Eco tier (auto-saver)  : "); Serial.println(ecoLevel);
    if (simulatedOutage) Serial.println("⚡ GRID: SIMULATED OUTAGE — fan shed, LED on essential automation");
    Serial.println("-------------------------------------------------");

    Serial.print("Voltage : ");
    Serial.print(loadVoltage);
    Serial.println(" V");

    Serial.print("Current : ");
    Serial.print(current_mA);
    Serial.println(" mA");

    Serial.print("Power  : ");
    Serial.print(power_W,3);
    Serial.println(" W");

    Serial.print("Energy : ");
    Serial.print(totalEnergy_kWh,6);
    Serial.println(" Wh");
  }


  if(millis()-previousSaveMillis>=SAVE_INTERVAL)
  {
    previousSaveMillis = millis();
    if (accumulatedKWh != lastSavedKWh) {
      preferences.putFloat("totalEnergyKWh", accumulatedKWh);
      preferences.putFloat("simDays", simulatedDaysElapsed);
      preferences.putFloat("cycleStartKWh", billingCycleStartKWh);
      preferences.putFloat("cyclePeakKWh", cyclePeakKWh);
      preferences.putFloat("cycleOffKWh", cycleOffKWh);
      lastSavedKWh = accumulatedKWh;
    }
  }

   // --- END OF BILLING CYCLE: start a new cycle, but the lifetime meter is NEVER reset ---
  if (simulatedDaysElapsed >= 30.0f) {
      Serial.println("\n===========================================");
      Serial.println("  ⚠️ TNB MONTHLY BILLING STATEMENT   ");
      Serial.println("===========================================");
      Serial.print("Final Cycle Invoice Total: RM "); Serial.println(liveBill.totalBill, 2);
      Serial.print("Base Charges (E+C+N)     : RM "); Serial.println(liveBill.baseCost, 2);
      Serial.print("EEI Rebate               : -RM "); Serial.println(liveBill.eeiRebate, 2);
      Serial.print("SST Service Tax (8%)     : RM "); Serial.println(liveBill.serviceTax, 2);
      Serial.print("Lifetime Meter Reading   : "); Serial.print(accumulatedKWh, 3); Serial.println(" kWh (carried forward, not reset)");
      Serial.println("Starting new billing cycle...\n");
      billingCycleStartKWh = accumulatedKWh; // new cycle starts from the current meter reading
      cyclePeakKWh = 0.0f;
      cycleOffKWh  = 0.0f;
      preferences.putFloat("cycleStartKWh", billingCycleStartKWh);
      preferences.putFloat("cyclePeakKWh", 0.0f);
      preferences.putFloat("cycleOffKWh", 0.0f);
      simulatedDaysElapsed = 0.0f;
      // NOTE: accumulatedKWh (lifetime meter) is intentionally NEVER reset here —
      // a real TNB meter's cumulative register never zeroes out either.
  }

  delay(200); // Small stability delay for the loop
}
