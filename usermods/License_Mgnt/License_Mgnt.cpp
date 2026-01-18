#include "License_Mgnt.h"

void License_Mgnt::setup()
{

  isSetupDone = true;

  // Initialize last time
  lastTime = millis();
}

void License_Mgnt::loop()
{
  unsigned long now = millis();
  // compute elapsed ms as unsigned long to avoid ambiguous abs() overload on unsigned types
  unsigned long elapsedMs = now - lastTime;
  if ((elapsedMs / 1000UL) > (unsigned long)TIMEOUT_60_SECONDS)
  {
    // Reset last time
    lastTime = now;
    // Implement 1-min task logic
    LM_UM_DEBUGLN("[LM-UM] 1-min task triggered\n");

    if (true == rebootRequested)
    {
      LM_UM_DEBUGLN("[LM-UM] Reboot requested, performing reboot now...\n");
      delay(100); // Allow debug messages to flush
      WLED::instance().reset();
    }

    int8_t devKeyStatus = validateDeviceKey();
    if (0 != devKeyStatus)
    {
      LM_UM_DEBUGF("[LM-UM] Fail to validate device key (status=%d)\n", devKeyStatus);

      LM_UM_DEBUGF("[LM-UM] Have %d minutes to run KNX_IP user mode if no valid key is provided\n", TIMEOUT_60_MINUTES);
      LM_UM_DEBUGF("[LM-UM] Will force reboot once %d-min timeout reached\n", TIMEOUT_60_MINUTES);

      counter++;
      setTrialMinsLeft(TIMEOUT_60_MINUTES - counter);
      LM_UM_DEBUGF("[LM-UM] [%d] minutes remaining for shutting down KNX_IP user mode\n", getTrialMinsLeft());

      if (counter >= TIMEOUT_60_MINUTES)
      {
        // Implement 1-hour task logic
        LM_UM_DEBUGLN("[LM-UM] 1-hour task triggered\n");
        LM_UM_DEBUGLN("[LM-UM] Rebooting device due to invalid device key\n");

        LM_UM_DEBUGLN("[LM-UM] Set effect to \"Red Blink\" as warn before rebooting\n");
        // Set effect to "Red Blink" before rebooting to avoid FX-related issues on restart
        warningEffectBeforeReboot();

        // Should never reach here
        // counter = 0; just for logic completeness
      }
    }
    else
    {
      LM_UM_DEBUGLN("[LM-UM] Device key validated successfully\n");
    }
  }

#ifndef WLED_DISABLE_MQTT
  if (WLED_MQTT_CONNECTED)
  {
    char array[10];
    snprintf(array, sizeof(array), "%s", deviceId.c_str());
    publishMqtt(array);
  }
#endif
}

void License_Mgnt::addToJsonInfo(JsonObject &root)
{
  LM_UM_DEBUGF("[LM-UM] addToJsonInfo called - adding usermod info to JSON\n");

  // if "u" object does not exist yet we need to create it
  JsonObject user = root["u"];
  if (user.isNull())
  {
    user = root.createNestedObject("u");
  }

  // Check license status
  JsonArray licenseInfo = user.createNestedArray("License");
  int8_t devKeyStatus = validateDeviceKey();
  if (0 == devKeyStatus)
  {
    licenseInfo.add("Forever");
  }
  else if (-1 == devKeyStatus)
  {
    licenseInfo.add("Not Imported");
    LM_UM_DEBUGF("[LM-UM] Device key is not imported\n");
  }
  else if (-2 == devKeyStatus)
  {
    licenseInfo.add("Invalid");
    LM_UM_DEBUGF("[LM-UM] Device key is invalid\n");
  }
  else
  {
    licenseInfo.add("Error");
    LM_UM_DEBUGF("[LM-UM] Device key validation error\n");
  }

  if (0 != devKeyStatus)
  {
    JsonArray trialInfo = user.createNestedArray("Free Trial");
    uint8_t trialMinsLeft = getTrialMinsLeft();
    if (trialMinsLeft > 0)
    {
      trialInfo.add(String(trialMinsLeft) + " mins left");
      LM_UM_DEBUGF("[LM-UM] Free trial active: %d mins left\n", trialMinsLeft);
    }
    else
    {
      trialInfo.add("Expired");
      LM_UM_DEBUGF("[LM-UM] Free trial expired\n");
    }
  }
}

void License_Mgnt::addToConfig(JsonObject &root)
{
  JsonObject top = root.createNestedObject(FPSTR(_name));
  top[FPSTR(_licenseStatus)] = licenseStatus;
  top[FPSTR(_deviceId)] = deviceId;
  top[FPSTR(_deviceKey)] = deviceKey;
}

// Append useful info to the usermod settings gui
void License_Mgnt::appendConfigData()
{
  // No additional info needed currently
}

bool License_Mgnt::readFromConfig(JsonObject &root)
{
  JsonObject top = root[FPSTR(_name)];
  bool configComplete = !top.isNull();
  configComplete &= getJsonValue(top[FPSTR(_licenseStatus)], licenseStatus);
  configComplete &= getJsonValue(top[FPSTR(_deviceId)], deviceId);
  configComplete &= getJsonValue(top[FPSTR(_deviceKey)], deviceKey);
  return configComplete;
}

void License_Mgnt::publishMqtt(const char *state, bool retain)
{
#ifndef WLED_DISABLE_MQTT
  // Check if MQTT Connected, otherwise it will crash the 8266
  if (WLED_MQTT_CONNECTED)
  {
    // Todo: adjust topic as needed
  }
#endif
}

// Quick blink red effect to indicate impending reboot
void License_Mgnt::warningEffectBeforeReboot()
{
  strip.getMainSegment().setColor(0, RGBW32(255, 0, 0, 100));
  strip.getMainSegment().setMode(FX_MODE_BLINK);
  colorUpdated(CALL_MODE_DIRECT_CHANGE);

  LM_UM_DEBUGLN("[LM-UM] Warning effect before reboot triggered\n");
  rebootRequested = true;
}

const char License_Mgnt::_name[] PROGMEM = "License_Mgnt";
const char License_Mgnt::_licenseStatus[] PROGMEM = "License Status";
const char License_Mgnt::_deviceId[] PROGMEM = "Device ID";
const char License_Mgnt::_deviceKey[] PROGMEM = "Device Key";

static License_Mgnt license_mgnt;
REGISTER_USERMOD(license_mgnt);