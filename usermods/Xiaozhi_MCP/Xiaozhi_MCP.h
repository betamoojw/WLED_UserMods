#pragma once

#if defined(ESP8266)
#error "Xiaozhi MCP is currently not supported on ESP8266 platform, please use ESP32"
#endif

#include "wled.h"
#include <WebSocketMCP.h>

// Keep the existing per-usermod Serial gates: routing these through WLED_DEBUG
// would change diagnostic output for builds that enable only the module flag.
// Debug logging control: define MCP_UM_DEBUG at build time to enable verbose logs
#ifdef MCP_UM_DEBUG
#define MCP_UM_DEBUGF(...) Serial.printf(__VA_ARGS__)
#define MCP_UM_DEBUGLN(msg) Serial.println(msg)
#else
#define MCP_UM_DEBUGF(...)
#define MCP_UM_DEBUGLN(msg)
#endif

// Warning logging (always on unless explicitly suppressed)
#ifndef MCP_UM_SUPPRESS_WARN
#define MCP_UM_WARNF(...) Serial.printf(__VA_ARGS__)
#define MCP_UM_WARNLN(msg) Serial.println(msg)
#else
#define MCP_UM_WARNF(...)
#define MCP_UM_WARNLN(msg)
#endif

#ifndef USERMOD_ID_XIAOZHI_MCP
#define USERMOD_ID_XIAOZHI_MCP 0xA900
#endif

class Xiaozhi_MCP : public Usermod {
public:
  // --- Usermod API ---
  void setup() override;
  void loop() override;
  void addToJsonInfo(JsonObject& root) override;
  void addToConfig(JsonObject& root) override;
  void appendConfigData() override;
  bool readFromConfig(JsonObject& root) override;
  uint16_t getId() override { return USERMOD_ID_XIAOZHI_MCP; }
  const char* getName() { return "Xiaozhi_MCP"; }

  // Accessors for MCP terminal alias
  const String& getMcpTerminalAlias() const { return mcpTerminalAlias; }
  void setMcpTerminalAlias(const String& alias) { mcpTerminalAlias = alias; }

private:
  // --- Config values (editable via JSON/UI) ---
  bool isEnabled = false;
  String mcpTerminalAlias = "LED";
  String mcpEndpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjExODk4NCwiYWdlbnRJZCI6MTA5MDAsImVuZHBvaW50SWQiOiJhZ2VudF8xMDkwMCIsInB1cnBvc2UiOiJtY3AtZW5kcG9pbnQiLCJpYXQiOjE3NjgzMTM2NDMsImV4cCI6MTc5OTg3MTI0M30.Rf1Wg030527sX6DGzDzVdP52cOjYOMN67QN1iKU5fAaT3DTbAv3idzxDhn1IV5ecN45HxaXM1JdMDxZc4elkKA"; // Adjust as needed

  bool isSetupDone = false;
  unsigned long lastTime = 0;

  static const char _name[];
  static const char _enabled[];
  static const char _mcpTerminalAlias[];
  static const char _mcpEndpoint[];

  static constexpr int TIMEOUT_60_SECONDS = 60;

  // any private methods should go here (non-inline method should be defined out of class)
  bool checkMcpConfig();
};
