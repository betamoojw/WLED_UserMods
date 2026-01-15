#include "wled.h"
#include <WebSocketMCP.h>
#include "Xiaozhi_MCP.h"
#include "src/dependencies/network/Network.h"


void Xiaozhi_MCP::setup()
{
  if (!isEnabled) return;

  if (((mcpEndpoint == "") || mcpEndpoint.length() == 0))
  {
    MCP_UM_WARNLN("[MCP-UM] MCP Endpoint is empty, disabling usermod.");
    return;
  }

  // Check network connectivity
  if (!Network.isConnected())
  {
    MCP_UM_DEBUGLN("[MCP-UM] Network not connected, deferring MCP.begin().");
    return;
  }
  
  IPAddress ip = Network.localIP();
  if (!ip || ip.toString() == String("0.0.0.0"))
  {
    MCP_UM_DEBUGLN("[MCP-UM] Network connected but no IP yet, deferring MCP.begin().");
    return;
  }

#ifdef ARDUINO_ARCH_ESP32
  if (Network.isEthernet())
  {
    // For Ethernet, we don't need to disable WiFi sleep
    MCP_UM_DEBUGLN("[MCP-UM] Using Ethernet connection");
  }
  else
  {
    WiFi.setSleep(false); // modem-sleep off helps WiFi multicast reliability
    MCP_UM_DEBUGLN("[MCP-UM] Using WiFi connection, sleep disabled");

    delay(100); // Give lwIP time to complete initialization

    // Verify WiFi is still connected after delay
    if (WiFi.status() != WL_CONNECTED)
    {
      MCP_UM_DEBUGLN("[MCP-UM] WiFi disconnected during lwIP wait, deferring MCP.begin().");
      return;
    }
  }
#endif

  // Additional safety: ensure we're not in a critical network transition
  yield();


  MCP_UM_DEBUGF("[MCP-UM] Mode Name: '%s'\n", getName());
  MCP_UM_DEBUGF("[MCP-UM] MCP Endpoint: '%s'\n", mcpEndpoint.c_str());

  // Start MCP service
  mcpClient.begin(mcpEndpoint.c_str(), onConnectionStatus);

  // Initialize last time
  lastTime = millis();

  isSetupDone = true;
}

void Xiaozhi_MCP::loop()
{
  if (!isEnabled) return;

  unsigned long now = millis();
  if (int((now - lastTime) / 1000) > TIMEOUT_60_SECONDS) {
    // Reset last time
    lastTime = now;
    // Implement 1-min task logic
    MCP_UM_DEBUGLN("[MCP-UM] 1-min task triggered\n");

    // Check network connectivity
    if (Network.isConnected() && !isSetupDone) {
      setup();
    }
  }

// Measure the temperature
#ifdef defined(CONFIG_IDF_TARGET_ESP32S2) // ESP32S2
  temperature = -1;
#else // ESP32 ESP32S3 and ESP32C3
  temperature = roundf(temperatureRead() * 10) / 10;
#endif

#ifndef WLED_DISABLE_MQTT
  if (WLED_MQTT_CONNECTED)
  {
    char array[10];
    snprintf(array, sizeof(array), "%f", temperature);
    publishMqtt(array);
  }
#endif
// Handle MCP client events
  mcpClient.loop();
}

void Xiaozhi_MCP::addToJsonInfo(JsonObject &root)
{
  if (!isEnabled)
  {
    return;
  }

  // if "u" object does not exist yet we need to create it
  JsonObject user = root["u"];
  if (user.isNull())
    user = root.createNestedObject("u");

  JsonArray userTempArr = user.createNestedArray(FPSTR(_name));
  userTempArr.add(temperature);
  userTempArr.add(F(" °C"));

  // if "sensor" object does not exist yet wee need to create it
  JsonObject sensor = root[F("sensor")];
  if (sensor.isNull())
    sensor = root.createNestedObject(F("sensor"));

  JsonArray sensorTempArr = sensor.createNestedArray(FPSTR(_name));
  sensorTempArr.add(temperature);
  sensorTempArr.add(F("°C"));
}

void Xiaozhi_MCP::addToConfig(JsonObject &root)
{
  JsonObject top = root.createNestedObject(FPSTR(_name));
  top[FPSTR(_enabled)] = isEnabled;
  top[FPSTR(_connectionRetryInterval)] = connRetryInterval;
  top[FPSTR(_mcpEndpoint)] = mcpEndpoint;
}

// Append useful info to the usermod settings gui
void Xiaozhi_MCP::appendConfigData()
{
  // Display 'ms' next to the 'Loop Interval' setting
  oappend(F("addInfo('Temperature:Loop Interval', 1, 'ms');"));
  // Display '°C' next to the 'Activation Threshold' setting
  oappend(F("addInfo('Temperature:Activation Threshold', 1, '°C');"));
  // Display '0 = Disabled' next to the 'Preset To Activate' setting
  oappend(F("addInfo('Temperature:Preset To Activate', 1, '0 = unused');"));
}

bool Xiaozhi_MCP::readFromConfig(JsonObject &root)
{
  JsonObject top = root[FPSTR(_name)];
  bool configComplete = !top.isNull();
  configComplete &= getJsonValue(top[FPSTR(_enabled)], isEnabled);
  configComplete &= getJsonValue(top[FPSTR(_connectionRetryInterval)], connRetryInterval);
  connRetryInterval = max(connRetryInterval, minLoopInterval); // Makes sure the loop interval isn't too small.
  configComplete &= getJsonValue(top[FPSTR(_mcpEndpoint)], mcpEndpoint);
  return configComplete;
}

void Xiaozhi_MCP::publishMqtt(const char *state, bool retain)
{
#ifndef WLED_DISABLE_MQTT
  // Check if MQTT Connected, otherwise it will crash the 8266
  if (WLED_MQTT_CONNECTED)
  {
    // Todo: adjust topic as needed
  }
#endif
}



void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] 已连接到服务器");
    // 连接成功后注册工具
    registerMcpTools();
  } else {
    Serial.println("[MCP] 与服务器断开连接");
  }
}

void registerMcpTools()
{
  // 注册一个简单的LED控制工具
  mcpClient.registerTool(
      "led_blink",
      "控制ESP32板载LED",
      "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"]}},\"required\":[\"state\"]}",
      [](const String &args)
      {
        DynamicJsonDocument doc(256);
        deserializeJson(doc, args);
        String state = doc["state"].as<String>();

        if (state == "on")
        {
          if (bri == 0)
          {
            bri = (briLast > 0) ? briLast : 128;
          }
        }
        else if (state == "off")
        {
          briLast = bri;
          bri = 0;
        }
        
        stateUpdated(CALL_MODE_DIRECT_CHANGE);

        return WebSocketMCP::ToolResponse("{\"success\":true,\"state\":\"" + state + "\"}");
      });
  Serial.println("[MCP] LED控制工具已注册");
}

const char Xiaozhi_MCP::_name[] PROGMEM = "Xiaozhi_MCP";
const char Xiaozhi_MCP::_enabled[] PROGMEM = "Enabled";
const char Xiaozhi_MCP::_connectionRetryInterval[] PROGMEM = "Connection Retry Interval";
const char Xiaozhi_MCP::_mcpEndpoint[] PROGMEM = "MCP Endpoint";

static Xiaozhi_MCP xiaozhi_mcp;
REGISTER_USERMOD(xiaozhi_mcp);