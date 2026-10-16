#pragma once

#if defined(ESP8266)
#error "License_Mgnt is currently not supported on ESP8266 platform, please use ESP32"
#endif

#include "wled.h"

// Keep the existing per-usermod Serial gates: routing these through WLED_DEBUG
// would change diagnostic output for builds that enable only the module flag.
// Debug logging control: define LM_UM_DEBUG at build time to enable verbose logs
#ifdef LM_UM_DEBUG
#define LM_UM_DEBUGF(...) Serial.printf(__VA_ARGS__)
#define LM_UM_DEBUGLN(msg) Serial.println(msg)
#else
#define LM_UM_DEBUGF(...)
#define LM_UM_DEBUGLN(msg)
#endif

// Warning logging (always on unless explicitly suppressed)
#ifndef LM_UM_SUPPRESS_WARN
#define LM_UM_WARNF(...) Serial.printf(__VA_ARGS__)
#define LM_UM_WARNLN(msg) Serial.println(msg)
#else
#define LM_UM_WARNF(...)
#define LM_UM_WARNLN(msg)
#endif

#ifndef USERMOD_ID_LICENSE_MGNT
#define USERMOD_ID_LICENSE_MGNT 0xA901
#endif

class License_Mgnt : public Usermod {
public:
  // --- Usermod API ---
  void setup() override;
  void loop() override;
  void addToJsonInfo(JsonObject& root) override;
  void addToConfig(JsonObject& root) override;
  void appendConfigData() override;
  bool readFromConfig(JsonObject& root) override;
  uint16_t getId() override { return USERMOD_ID_LICENSE_MGNT; }
  const char* getName() { return "License_Mgnt"; }

private:
  // Read-only values exposed in the usermod settings.
  String licenseStatus = "";
  String deviceId = "";

  int8_t devKeyStatus = -1; // 0=valid, -1=not imported, -2=open error, -3=invalid
  unsigned long lastTime = 0;
  uint8_t counter = 0;
  uint8_t trialMinsLeft = 0;
  bool rebootRequested = false;

  static const char _name[];
  static const char _licenseStatus[];
  static const char _deviceId[];

  static constexpr int TIMEOUT_60_SECONDS = 60;
  static constexpr int TIMEOUT_60_MINUTES = 60;

  void runPeriodicLicenseCheck();
  void warningEffectBeforeReboot();
  static void getDevKeyStatusString(const int8_t devKeyStatus, String& statusStr);
  static String getStoredDeviceKey();

  inline uint8_t getTrialMinsLeft() const { return trialMinsLeft; }
  inline void setTrialMinsLeft(uint8_t mins) { trialMinsLeft = mins; }
};
