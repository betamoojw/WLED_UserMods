#ifndef WEBSOCKET_MCP_H
#define WEBSOCKET_MCP_H

#include "wled.h"
#include <atomic>
#include <functional>

// Debug logging control: define XIAOZHI_MCP_DEBUG at build time to enable verbose logs
#ifdef XIAOZHI_MCP_DEBUG
#define XIAOZHI_MCP_DEBUGF(...) DEBUG_PRINTF(__VA_ARGS__)
#define XIAOZHI_MCP_DEBUGLN(msg) DEBUG_PRINTLN(msg)
#else
#define XIAOZHI_MCP_DEBUGF(...)
#define XIAOZHI_MCP_DEBUGLN(msg)
#endif

// Diagnostics follow WLED_DEBUG and can additionally be suppressed for this usermod.
#ifndef XIAOZHI_MCP_SUPPRESS_WARN
#define XIAOZHI_MCP_WARNF(...) DEBUG_PRINTF(__VA_ARGS__)
#define XIAOZHI_MCP_WARNLN(msg) DEBUG_PRINTLN(msg)
#else
#define XIAOZHI_MCP_WARNF(...)
#define XIAOZHI_MCP_WARNLN(msg)
#endif

/**
 * WebSocketMCP class
 * Encapsulates the WebSocket connection to the MCP server and its communication.
 */
class WebSocketMCP {
public:
  // Define the tool response content structure
  // Sending to WebSocket: {"jsonrpc":"2.0","id":48,"result":{"content":[{"type":"text","text":"{\n  \"success\": true,\n  \"result\": 2\n}"}],"isError":false}}
  struct ToolContentItem {
    String type; // Content type, such as "text"
    String text; // Text content
  };

  // Define the tool response structure
  struct ToolResponse {
    std::vector<ToolContentItem> content; // Response content array
    bool isError;                         // Whether this is an error response

    // Constructor: create a response from a single text content item
    ToolResponse(const String& textContent, bool error = false) {
      ToolContentItem item;
      item.type = "text";

      // Try to detect JSON and format it (by checking for a leading { and trailing })
      String trimmedText = textContent;
      trimmedText.trim();

      if (trimmedText.startsWith("{") && trimmedText.endsWith("}")) {
        item.text = WebSocketMCP::formatJsonString(textContent);
      } else {
        item.text = textContent;
      }

      content.push_back(item);
      isError = error;
    }

    // Additional constructor for bool and String parameter handling
    ToolResponse(bool error, const String& message) {
      ToolContentItem item;
      item.type = "text";
      item.text = message;
      content.push_back(item);
      isError = error;
    }

    // Default constructor
    ToolResponse() : isError(false) {}

    // Create a response from a JSON object (convenience method)
    static ToolResponse fromJson(const JsonObject& json, bool error = false) {
      String jsonStr;
      serializeJson(json, jsonStr);
      return ToolResponse(jsonStr, error);
    }
  };

  // Helper class for parameter handling
  class ToolParams {
  public:
    ToolParams(const String& json) {
      DeserializationError error = deserializeJson(doc, json);
      valid = !error;
    }

    // Static factory method from JsonVariantConst
    // This creates a new ToolParams instance by serializing the variant and re-parsing it.
    // Useful for handling items from JsonArray or nested JsonObjects reliably.
    static ToolParams fromVariant(const JsonVariantConst& variant) {
      String variantJson;
      serializeJson(variant, variantJson);
      return ToolParams(variantJson);
    }

    template <typename T>
    T get(const String& key, T defaultValue) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return defaultValue;
      }
      return doc.as<JsonObjectConst>()[key].as<T>();
    }

    JsonVariantConst getJsonValue(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return JsonVariantConst();
      }
      return doc.as<JsonObjectConst>()[key];
    }

    JsonArrayConst getJsonArray(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key) || !doc.as<JsonObjectConst>()[key].is<JsonArray>()) {
        return JsonArrayConst();
      }
      return doc.as<JsonObjectConst>()[key].as<JsonArrayConst>();
    }

    bool isArray(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return false;
      }
      return doc.as<JsonObjectConst>()[key].is<JsonArray>();
    }

    size_t getArraySize(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key) || !doc.as<JsonObjectConst>()[key].is<JsonArray>()) {
        return 0;
      }
      return doc.as<JsonObjectConst>()[key].as<JsonArrayConst>().size();
    }

    // Method to check if the root of this ToolParams' document is a JSON object
    bool isJsonObject() const {
      return valid && doc.is<JsonObject>();
    }

    // Method to check if the root of this ToolParams' document is a JSON array
    bool isJsonArray() const {
      return valid && doc.is<JsonArray>();
    }

    // Method to get the root as a JsonObjectConst if it is one
    JsonObjectConst getAsJsonObject() const {
      if (isJsonObject()) {
        return doc.as<JsonObjectConst>();
      }
      return JsonObjectConst(); // Returns a null JsonObjectConst
    }

    // Method to get the root as a JsonArrayConst if it is one
    JsonArrayConst getAsJsonArray() const {
      if (isJsonArray()) {
        return doc.as<JsonArrayConst>();
      }
      return JsonArrayConst(); // Returns a null JsonArrayConst
    }

    bool contains(const String& key) const {
      return valid && doc.is<JsonObject>() && doc.as<JsonObjectConst>().containsKey(key);
    }

    String getDebugJson() const {
      String result;
      if (valid) {
        serializeJson(doc, result);
      } else {
        result = "{\"error\":\"Invalid JSON document in ToolParams\"}";
      }
      return result;
    }

    bool isValid() const {
      return valid;
    }

  private:
    DynamicJsonDocument doc{2048}; // Adjust size as needed
    bool valid = false;
  };

  // Redefine the tool callback type - accepts a JSON string and returns a ToolResponse
  typedef std::function<ToolResponse(const String&)> ToolCallback;

  // Callback type definitions
  // Output callback: void(const String&)
  typedef void (*OutputCallback)(const String&);
  // Error callback: void(const String&)
  typedef void (*ErrorCallback)(const String&);
  // Connection status callback: void(bool)
  typedef void (*ConnectionCallback)(bool);

  WebSocketMCP();
  ~WebSocketMCP();
  WebSocketMCP(const WebSocketMCP&) = delete;
  WebSocketMCP& operator=(const WebSocketMCP&) = delete;

  // Call lifecycle, send, and tool APIs only from WLED's main loop task.
  // Callbacks may call begin(), end(), or disconnect(); replacement starts after
  // the retiring worker exits and loop() runs again.
  // Start an outbound ws/wss client. WSS verifies the server using the IDF CA
  // bundle through BearSSL and requires synchronized WLED time. Callbacks run from loop().
  // Heartbeats: 10-second ping/pong intervals, six missed pongs before reconnect.
  // Reconnect attempts are spaced 60 seconds apart, as in the previous transport.
  bool begin(const char* mcpEndpoint, ConnectionCallback connCb = nullptr);

  /**
   * Send data to the WebSocket server (equivalent to stdin)
   * @param message The message to send
   * @return Whether the bounded queue accepted the message, not peer delivery
   */
  bool sendMessage(const String& message);

  /**
   * Process WebSocket events and maintain the connection
   * This should be called frequently in the main loop
   */
  void loop();

  /**
   * Check whether the client is connected to the server
   * @return Connection status
   */
  bool isConnected();

  // Static diagnostic text, safe to read from the HTTP task without touching transport state.
  const char* getConnectionDetail() const { return _connectionDetail.load(); }

  /**
   * Disconnect from the server
   */
  void disconnect();

  // Stop the worker and automatic reconnection until begin() is called again.
  void end();

  // Tool registration and management methods
  bool registerTool(const String& name, const String& description, const String& inputSchema, ToolCallback callback);
  // Simplified tool registration API
  bool registerSimpleTool(const String& name, const String& description,
                          const String& paramName, const String& paramDesc,
                          const String& paramType, ToolCallback callback);

  bool unregisterTool(const String& name);
  size_t getToolCount();
  void clearTools();

private:
  std::atomic<const char*> _connectionDetail{"Stopped"};
  // The worker owns networking; loop() alone invokes callbacks and MCP tools.
  struct Transport;
  Transport* _transport = nullptr;
  Transport* _retiring = nullptr;
  void reapTransport();
  ConnectionCallback connectionCallback = nullptr;
  std::atomic<bool> connected{false};
  uint32_t _session = 0;
  unsigned long lastPingTime = 0;
  void stopTransport();
  void handleJsonRpcMessage(const String& message);

  // Tool structure definition
  struct Tool {
    String name;           // Tool name
    String description;    // Tool description
    String inputSchema;    // Tool input schema (JSON format)
    ToolCallback callback; // Tool callback function
  };

  // Tool list
  std::vector<Tool> _tools;

  // Helper methods
  String escapeJsonString(const String &input);
  
  // Format a JSON string so each key-value pair appears on its own line
  static String formatJsonString(const String& jsonStr);
};

#endif // WEBSOCKET_MCP_H
