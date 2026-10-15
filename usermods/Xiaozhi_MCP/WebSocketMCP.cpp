#include "WebSocketMCP.h"

#include <esp_random.h>
#include <esp_transport_ws.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>
#include <NetworkClient.h>
#include <lwip/sockets.h>
// Enable the library's certificate-store adapter API and full RSA/ECDSA support.
#define ENABLE_FS
#undef BEARSSL_SSL_BASIC
#include <ESP_SSLClient.h>
#include "McpTrustStore.h"
#include <new>

#ifdef SSLCLIENT_INSECURE_ONLY
static_assert(false, "Xiaozhi MCP requires certificate verification support");
#endif

// Uses ESP_SSLClient for TLS and ESP-IDF for RFC 6455 framing:
// https://github.com/espressif/esp-idf/tree/v5.5.4/components/tcp_transport
// Opening handshake requirements: https://www.rfc-editor.org/rfc/rfc6455#section-4.1
// Read the HTTP upgrade one byte at a time so an immediately following MCP
// request stays in the socket. Do not log the request: its query can contain a token.
struct WebSocketMCP::Transport {
  static constexpr size_t MAX_MESSAGE = 15 * 1024; // Matches the previous client's frame limit.
  static constexpr size_t QUEUE_LENGTH = 4;
  static constexpr size_t MAX_RPC_PER_LOOP = 2; // initialize produces two outbound messages.
  static constexpr uint32_t IO_TIMEOUT = 5000;
  static constexpr uint32_t RECONNECT_INTERVAL = 60000;
  static constexpr uint32_t PING_INTERVAL = 10000;
  static constexpr uint32_t PONG_TIMEOUT = 10000;
  static constexpr uint8_t MAX_MISSED_PONGS = 6;
  struct Message {
    char *data;
    size_t length;
    uint32_t session;
  };

  String host, path;
  uint16_t port = 0;
  bool secure = false;
  QueueHandle_t incoming = nullptr, outgoing = nullptr;
  std::atomic<unsigned> references{1};
  std::atomic<bool> stop{false}, online{false}, reset{false};
  std::atomic<uint32_t> session{0};
  std::atomic<bool> finished{false};
  // Toki is mutable and belongs to WLED's main loop. Publish UTC seconds here;
  // the worker must neither call toki.second() nor use the unrelated POSIX clock.
  std::atomic<uint32_t> clockSeconds{0};
  std::atomic<const char *> detail{"Starting"};
  bool started = false; // Main-loop-only task admission state.
  uint32_t lastStartAttempt = 0;
  esp_transport_handle_t socket = nullptr, websocket = nullptr; // Worker-owned.
  // Worker-owned TCP adapter: cancel TLS I/O cooperatively and feed the IDLE task.
  // NetworkClient::write can restart its retry count on each partial send, so use
  // nonblocking lwIP sends with one deadline shared by all writes in a TLS operation.
  class TlsTcpClient : public NetworkClient {
    Transport &_owner;
    uint32_t _ioStarted = 0, _ioTimeout = IO_TIMEOUT;

    bool cancelled() {
      if (!_owner.stop && !_owner.reset && millis() - _ioStarted < _ioTimeout) return false;
      NetworkClient::stop();
      return true;
    }

  public:
    explicit TlsTcpClient(Transport &owner) : _owner(owner) {}
    void arm(uint32_t timeout = IO_TIMEOUT) { _ioStarted = millis(); _ioTimeout = timeout; }
    using NetworkClient::connect;
    int connect(const char *host, uint16_t port) override {
      int result = NetworkClient::connect(host, port, IO_TIMEOUT);
      arm(); // DNS/TCP setup must not consume the subsequent TLS I/O deadline.
      return result;
    }
    // Arduino flush discards received bytes. TCP writes need no such operation.
    void flush() override {}
    int available() override {
      delay(1); // ESP_SSLClient's handshake loop otherwise need not yield to IDLE.
      return cancelled() ? 0 : NetworkClient::available();
    }
    size_t write(const uint8_t *buffer, size_t length) override {
      if (!length) return 0;
      do {
        if (cancelled() || fd() < 0) break;
        int count = ::send(fd(), buffer, length, MSG_DONTWAIT);
        if (count > 0) return count;
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        delay(1);
      } while (true);
      // A zero write must carry an error: ESP_SSLClient may otherwise continue
      // its engine loop after stop() has freed the engine on a transport failure.
      setWriteError(1);
      NetworkClient::stop();
      return 0;
    }
  } tcp{*this};

  McpTrustStore trustStore;
  BSSL_SSLClient tls; // Direct TLS API avoids the wrapper's port-based auto-detection.
  Client *client = nullptr;

  // Discard queued buffers when their connection is no longer usable.
  static void clearQueue(QueueHandle_t queue) {
    if (!queue) return;
    Message message;
    while (xQueueReceive(queue, &message, 0) == pdTRUE) free(message.data);
  }

  // Queue entries own their buffers. Called only after both owners release the state.
  ~Transport() {
    for (QueueHandle_t queue : {incoming, outgoing}) {
      if (!queue) continue;
      clearQueue(queue);
      vQueueDelete(queue);
    }
  }
  void release() { if (references.fetch_sub(1) == 1) delete this; }

  // Reject malformed URLs and header injection without exposing endpoint credentials.
  bool parseEndpoint(const char *endpoint) {
    if (!endpoint || strnlen(endpoint, 2049) > 2048) return false;
    String url(endpoint);
    if (url.startsWith("wss://")) secure = true;
    else if (!url.startsWith("ws://")) return false;
    for (size_t i = 0; i < url.length(); i++) {
      if ((uint8_t)url[i] <= 32 || (uint8_t)url[i] == 127 || url[i] == '#') return false;
    }
    size_t start = secure ? 6 : 5;
    size_t end = start;
    while (end < url.length() && url[end] != '/' && url[end] != '?') end++;
    String authority = url.substring(start, end);
    if (!authority.length() || authority.indexOf('@') >= 0) return false;
    path = end == url.length() ? "/" : url.substring(end);
    if (path[0] == '?') path = "/" + path;
    int portPos = authority.indexOf(':');
    // Preserve DNS/IPv4 endpoints and also accept bracketed IPv6 literals.
    if (authority[0] == '[') {
      int bracket = authority.indexOf(']');
      if (bracket < 2) return false;
      host = authority.substring(1, bracket);
      if (bracket + 1 < (int)authority.length() && authority[bracket + 1] != ':') return false;
      portPos = bracket + 1 < (int)authority.length() ? bracket + 1 : -1;
    } else {
      host = portPos < 0 ? authority : authority.substring(0, portPos);
      if (host.indexOf('[') >= 0 || host.indexOf(']') >= 0) return false;
    }
    // ESP_SSLClient 3.1.3 stores hostnames in a 64-byte array.
    if (!host.length() || (secure && host.length() >= 64)) return false;
    port = secure ? 443 : 80;
    if (portPos >= 0) {
      String value = authority.substring(portPos + 1);
      if (!value.length() || value.length() > 5) return false;
      unsigned parsed = 0;
      for (size_t i = 0; i < value.length(); i++) {
        if (value[i] < '0' || value[i] > '9') return false;
        parsed = parsed * 10 + value[i] - '0';
      }
      if (!parsed || parsed > UINT16_MAX) return false;
      port = parsed;
    }
    return true;
  }

  // Finish partial writes within a fixed deadline; never wait indefinitely.
  bool writeAll(const char *data, size_t length) {
    uint32_t started = millis();
    while (length && !stop && !reset) {
      uint32_t elapsed = millis() - started;
      if (elapsed >= IO_TIMEOUT) return false;
      int count = esp_transport_write(socket, data, length, IO_TIMEOUT - elapsed);
      if (count <= 0) return false;
      data += count;
      length -= count;
    }
    return length == 0;
  }

  // Match comma-separated HTTP tokens without accepting substrings like "notupgrade".
  static bool hasToken(String value, const char *token) {
    value.toLowerCase();
    int start = 0;
    while (start < (int)value.length()) {
      int end = value.indexOf(',', start);
      if (end < 0) end = value.length();
      String item = value.substring(start, end);
      item.trim();
      if (item == token) return true;
      start = end + 1;
    }
    return false;
  }

  // Validate the upgrade and nonce; bounded reads leave subsequent frames untouched.
  bool handshake() {
    uint8_t nonce[16], digest[20];
    unsigned char key[25], accept[29];
    size_t encoded = 0;
    esp_fill_random(nonce, sizeof(nonce));
    if (mbedtls_base64_encode(key, sizeof(key), &encoded, nonce, sizeof(nonce))) return false;
    String challenge = String((char *)key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    br_sha1_context sha;
    br_sha1_init(&sha);
    br_sha1_update(&sha, challenge.c_str(), challenge.length());
    br_sha1_out(&sha, digest);
    if (mbedtls_base64_encode(accept, sizeof(accept), &encoded, digest, sizeof(digest))) return false;
    String request;
    if (!request.reserve(path.length() + host.length() + 256)) return false;
    String hostHeader = host.indexOf(':') >= 0 ? "[" + host + "]" : host;
    request = "GET " + path + " HTTP/1.1\r\nHost: " + hostHeader + ":" + String(port) +
              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
              (char *)key + "\r\nSec-WebSocket-Protocol: arduino\r\n\r\n";
    if (!writeAll(request.c_str(), request.length())) return false;

    String line;
    if (!line.reserve(512)) return false;
    bool status = false, upgrade = false, connection = false, accepted = false;
    bool firstLine = true;
    uint32_t started = millis();
    for (size_t total = 0; total < 4096 && !stop && !reset;) {
      if (millis() - started >= IO_TIMEOUT) return false;
      char ch;
      int count = esp_transport_read(socket, &ch, 1, 100);
      if (count < 0) return false;
      if (!count) continue;
      total++;
      if (ch != '\n') {
        if (line.length() >= 511 || !line.concat(ch)) return false;
        continue;
      }
      if (!line.endsWith("\r")) return false;
      line.remove(line.length() - 1);
      if (firstLine) {
        status = line == "HTTP/1.1 101" || line.startsWith("HTTP/1.1 101 ");
        firstLine = false;
      } else if (!line.length()) {
        return status && upgrade && connection && accepted;
      } else {
        int colon = line.indexOf(':');
        if (colon <= 0) return false;
        String name = line.substring(0, colon), value = line.substring(colon + 1);
        name.toLowerCase();
        value.trim();
        if (name == "upgrade") upgrade = hasToken(value, "websocket");
        else if (name == "connection") connection = hasToken(value, "upgrade");
        else if (name == "sec-websocket-accept") {
          if (accepted || value != (char *)accept) return false;
          accepted = true;
        } else if (name == "sec-websocket-extensions" && value.length()) return false;
        else if (name == "sec-websocket-protocol" && value != "arduino") return false;
      }
      line = "";
    }
    return false;
  }

  static Transport *fromHandle(esp_transport_handle_t handle) {
    return static_cast<Transport *>(esp_transport_get_context_data(handle));
  }

  // Arduino clients are polled in the worker; the render loop never waits for TLS.
  static int pollRead(esp_transport_handle_t handle, int timeout) {
    auto *state = fromHandle(handle);
    uint32_t started = millis();
    state->tcp.arm();
    do {
      if (state->stop || state->reset) return -1;
      if (state->client->available() > 0) return 1;
      if (!state->client->connected()) return -1;
      if (!timeout) break;
      delay(1);
    } while (millis() - started < (uint32_t)timeout);
    return 0;
  }

  // IDF's frame parser expects a complete header read, while payload reads may be short.
  static int readSocket(esp_transport_handle_t handle, char *buffer, int length, int timeout) {
    auto *state = fromHandle(handle);
    uint32_t started = millis();
    int received = 0;
    while (received < length) {
      uint32_t elapsed = millis() - started;
      int ready = pollRead(handle, elapsed >= (uint32_t)timeout ? 0 : timeout - elapsed);
      if (ready <= 0) return received ? received : ready;
      int count = state->client->read((uint8_t *)buffer + received, length - received);
      if (count <= 0) return received ? received : -1;
      received += count;
      // Header reads are at most 8 bytes. Larger reads may return available payload.
      if (length > 8 || millis() - started >= (uint32_t)timeout) break;
    }
    return received;
  }

  // Bound the entire write, including TLS records, rather than restarting a timeout
  // for every chunk of a large MCP reply. Short writes make the frame fail closed.
  static int writeSocket(esp_transport_handle_t handle, const char *buffer, int length, int timeout) {
    auto *state = fromHandle(handle);
    uint32_t started = millis();
    state->tcp.arm(timeout > 0 ? timeout : 0);
    int written = 0;
    while (written < length && !state->stop && !state->reset) {
      uint32_t elapsed = millis() - started;
      if (timeout <= 0 || elapsed >= (uint32_t)timeout) break;
      if (state->secure) state->tls.setTimeout(timeout - elapsed);
      size_t count = state->client->write((const uint8_t *)buffer + written, min(length - written, 512));
      if (!count) return -1;
      written += count;
    }
    return written;
  }

  static int pollWrite(esp_transport_handle_t handle, int) {
    auto *state = fromHandle(handle);
    return state->client->connected() ? 1 : -1;
  }

  static int closeSocket(esp_transport_handle_t handle) {
    auto *state = fromHandle(handle);
    state->tcp.stop();
    if (state->client) state->client->stop();
    return 0;
  }

  // Only this worker touches clients, including during reconnect and shutdown.
  bool connect() {
    detail = "Connecting";
    tcp.setConnectionTimeout(IO_TIMEOUT);
    tcp.setTimeout(IO_TIMEOUT);
    tcp.clearWriteError();
    tcp.arm();
    client = &tcp;
    if (secure) {
      // WLED updates Toki via NTP/API, not time(nullptr). Wait without allocating
      // TLS buffers, and resume as soon as the main loop publishes valid time.
      detail = "Waiting for WLED time; enable NTP in Time & Macros";
      uint32_t now;
      while ((now = clockSeconds.load()) < ESP_SSLCLIENT_VALID_TIMESTAMP) {
        if (stop || reset) return false;
        delay(20);
      }
      if (stop || reset) return false;
      if (!trustStore.load()) {
        detail = "CA bundle initialization failed; retrying in 60 seconds";
        return false;
      }
      detail = "Connecting TLS";
      tls.setClient(&tcp, true);
      tls.setCertStore(&trustStore);
      tls.setX509Time(now);
      tls.setSSLVersion(BR_TLS12, BR_TLS12);
      tls.setTimeout(IO_TIMEOUT);
      tls.setHandshakeTimeout(IO_TIMEOUT);
      tls.setBufferSizes(16384, 512); // Full incoming TLS records, bounded transmit buffer.
      client = &tls;

      if (!tls.connectSSL(host.c_str(), port) || !tls.isSecure()) {
        detail = "TLS connection failed; retrying in 60 seconds";
        // ESP_SSLClient frees its engine on failure; getLastSSLError() would
        // dereference that engine. Keep this diagnostic independent of its state.
        XIAOZHI_MCP_WARNLN("[xiaozhi-mcp] TLS connection failed");
        return false;
      }

      tls.setTimeout(IO_TIMEOUT); // The library resets its I/O timeout during connectSSL().
    } else if (!tcp.connect(host.c_str(), port, IO_TIMEOUT)) {
      detail = "TCP connection failed; retrying in 60 seconds";
      return false;
    }
    socket = esp_transport_init();
    if (!socket) return false;
    esp_transport_set_context_data(socket, this);
    esp_transport_set_func(socket, nullptr, readSocket, writeSocket, closeSocket, pollRead, pollWrite, nullptr);
    detail = "WebSocket upgrade";
    if (!handshake()) {
      detail = "WebSocket upgrade failed; retrying in 60 seconds";
      XIAOZHI_MCP_WARNLN("[xiaozhi-mcp] WebSocket upgrade failed");
      return false;
    }
    websocket = esp_transport_ws_init(socket);
    if (!websocket) return false;
    esp_transport_ws_config_t config = {};
    config.propagate_control_frames = true;
    return esp_transport_ws_set_config(websocket, &config) == ESP_OK;
  }

  void close() {
    online = false;
    // Abort TCP before freeing TLS: shutdown must not flush pending application
    // data or wait for the peer. Also covers failure before TLS becomes secure.
    tcp.stop();
    if (client) client->stop();
    trustStore.clear(); // TLS no longer holds anchor descriptors.
    clearQueue(incoming);
    clearQueue(outgoing);
    if (websocket) esp_transport_destroy(websocket);
    if (socket) esp_transport_destroy(socket);
    websocket = socket = nullptr;
  }

  // ESP-IDF masks client frames in place, so all nonempty payloads must be writable.
  bool sendFrame(uint8_t opcode, char *data, size_t length) {
    auto op = static_cast<ws_transport_opcodes_t>(0x80 | opcode);
    return esp_transport_ws_send_raw(websocket, op, data, length, IO_TIMEOUT) == (int)length;
  }

  // Assemble text messages across TCP reads and continuation frames. Control frames
  // can interleave; binary messages are consumed but never passed to the MCP layer.
  bool receiveFrame(String &message, uint8_t &messageOpcode, bool &waitingPong, uint8_t &missedPongs) {
    char buffer[512];
    // Poll briefly when idle, but allow a full I/O timeout once a frame starts.
    // Using the short poll timeout for IDF's header parser would reject slow TCP splits.
    int ready = esp_transport_poll_read(socket, 100);
    if (ready <= 0) return ready == 0;
    uint32_t started = millis();
    int count = esp_transport_read(websocket, buffer, sizeof(buffer), IO_TIMEOUT);
    if (count < 0) return false;
    auto opcode = esp_transport_ws_get_read_opcode(websocket);
    if (opcode == WS_TRANSPORT_OPCODES_NONE) return true; // Idle read timeout.
    int length = esp_transport_ws_get_read_payload_len(websocket);
    bool final = esp_transport_ws_get_fin_flag(websocket);
    bool control = (opcode & 8) != 0;
    if (length < 0 || length > (int)MAX_MESSAGE || count > length) return false;
    if (control) {
      if (!final || length > 125 || (opcode != 8 && opcode != 9 && opcode != 10)) return false;
    } else if (opcode == 1 || opcode == 2) {
      if (messageOpcode) return false;
      messageOpcode = opcode;
    } else if (opcode != 0 || !messageOpcode) return false;
    if (!control && (size_t)length > MAX_MESSAGE - message.length()) return false;

    char controlData[125];
    size_t received = 0;
    do {
      if (!count && length) return false; // A mid-frame timeout invalidates IDF's parser state.
      if (control && count) memcpy(controlData + received, buffer, count);
      else if (messageOpcode == 1 && count && !message.concat(buffer, count)) return false;
      received += count;
      if (received == (size_t)length) break;
      uint32_t elapsed = millis() - started;
      if (stop || reset || elapsed >= IO_TIMEOUT) return false;
      count = esp_transport_read(websocket, buffer, min(sizeof(buffer), (size_t)length - received), IO_TIMEOUT - elapsed);
      if (count <= 0 || (size_t)count > (size_t)length - received) return false;
    } while (true);

    if (control) {
      if (opcode == 8) {
        if (length == 1) return false;
        sendFrame(8, controlData, length);
        return false;
      }
      if (opcode == 9) return sendFrame(10, controlData, length);
      waitingPong = false;
      missedPongs = 0;
    } else if (final) {
      if (messageOpcode == 1) {
        Message packet{nullptr, message.length(), session.load()};
        packet.data = static_cast<char *>(p_malloc(packet.length + 1));
        if (!packet.data) return false;
        memcpy(packet.data, message.c_str(), packet.length + 1);
        if (xQueueSend(incoming, &packet, 0) != pdTRUE) {
          free(packet.data);
          return false; // Bounded backpressure: reconnect rather than lose an RPC silently.
        }
      }
      message = "";
      messageOpcode = 0;
    }
    return true;
  }

  // Only the main loop creates tasks; a retired worker must finish I/O first.
  bool start() {
    lastStartAttempt = millis();
    references.fetch_add(1);
    if (xTaskCreate(run, "xiaozhi-mcp", 8192, this, 1, nullptr) != pdPASS) {
      release();
      return false;
    }
    started = true;
    return true;
  }

  // Keep a loop's snapshot alive when a user callback calls end() or begin().
  struct Reference {
    Transport *state;
    explicit Reference(Transport *value) : state(value) { state->references.fetch_add(1); }
    ~Reference() { state->release(); }
    Reference(const Reference &) = delete;
    Reference &operator=(const Reference &) = delete;
  };

  // Own all I/O and buffers; communicate with the main loop only through queues/atomics.
  static void run(void *argument) {
    auto *state = static_cast<Transport *>(argument);
    while (!state->stop) {
      state->reset = false;
      if (state->connect() && !state->stop && !state->reset) {
        clearQueue(state->incoming);
        clearQueue(state->outgoing);
        state->session.fetch_add(1);
        state->online = true;
        state->detail = "Connected";
        uint32_t lastPing = millis();
        bool waitingPong = false;
        uint8_t missedPongs = 0, messageOpcode = 0;
        String message;
        while (!state->stop && !state->reset) {
          Message packet;
          bool sent = true;
          // Bound each batch so queued output cannot starve reads and heartbeats.
          for (size_t i = 0; i < QUEUE_LENGTH && xQueueReceive(state->outgoing, &packet, 0) == pdTRUE; i++) {
            if (packet.session == state->session.load()) sent = state->sendFrame(1, packet.data, packet.length);
            free(packet.data);
            if (!sent) break;
          }
          if (!sent || !state->receiveFrame(message, messageOpcode, waitingPong, missedPongs)) break;
          uint32_t now = millis();
          if (waitingPong && now - lastPing >= PONG_TIMEOUT) {
            waitingPong = false;
            if (++missedPongs >= MAX_MISSED_PONGS) break;
          }
          if (!waitingPong && now - lastPing >= PING_INTERVAL) {
            if (!state->sendFrame(9, nullptr, 0)) break;
            lastPing = now;
            waitingPong = true;
          }
          delay(1); // Feed the IDLE watchdog even during continuous traffic.
        }
      }
      if (state->online) state->detail = "Connection closed; retrying in 60 seconds";
      state->close();
      uint32_t disconnectedAt = millis();
      while (!state->stop && millis() - disconnectedAt < RECONNECT_INTERVAL) {
        // A producer can have observed online just before close(). Release its
        // late packet as well, rather than retaining it throughout the retry wait.
        clearQueue(state->outgoing);
        delay(20);
      }
    }
    state->close();
    state->finished = true; // No more socket or queue work after this publication.
    state->release(); // The worker never references WebSocketMCP itself.
    vTaskDelete(nullptr);
  }
};

WebSocketMCP::WebSocketMCP() = default;

WebSocketMCP::~WebSocketMCP() {
  stopTransport();
  if (_retiring) _retiring->release(); // The worker retains its own reference.
}

// Reap only after the worker has closed its sockets and released queued buffers.
void WebSocketMCP::reapTransport() {
  if (_retiring && _retiring->finished) {
    _retiring->release();
    _retiring = nullptr;
  }
}

// Do not delete a task inside TLS. Retain the retiring worker until its I/O stops;
// subsequent begin() calls can replace a pending endpoint without spawning tasks.
void WebSocketMCP::stopTransport() {
  reapTransport();
  if (_transport) {
    _transport->stop = true;
    if (_transport->started) _retiring = _transport;
    else _transport->release();
    _transport = nullptr;
  }
  connected = false;
  _connectionDetail = "Stopped";
  lastPingTime = 0;
}

// Validate the replacement first. A replacement worker starts from loop() after
// the previous worker exits, preventing overlapping TLS allocations on reconfigure.
bool WebSocketMCP::begin(const char *endpoint, ConnectionCallback callback) {
  auto *state = new (std::nothrow) Transport;
  if (!state) return false;
  if (!state->parseEndpoint(endpoint)) { state->release(); return false; }
  state->incoming = xQueueCreate(Transport::QUEUE_LENGTH, sizeof(Transport::Message));
  state->outgoing = xQueueCreate(Transport::QUEUE_LENGTH, sizeof(Transport::Message));
  if (!state->incoming || !state->outgoing) { state->release(); return false; }
  bool wasConnected = connected;
  auto previousCallback = connectionCallback;
  stopTransport();
  _transport = state;
  state->clockSeconds = toki.second();
  _connectionDetail = "Starting";
  _session = 0;
  connectionCallback = callback;
  bool accepted = _retiring || state->start();
  if (!accepted) stopTransport();
  // Publish the new state before notifying; the callback may itself replace it.
  if (wasConnected && previousCallback) previousCallback(false);
  return accepted;
}

// Success means accepted into the bounded send queue, not acknowledged by the peer.
bool WebSocketMCP::sendMessage(const String &message) {
  if (!_transport || !connected || !_transport->online || _transport->reset || _transport->session != _session) return false;

  if (message.length() > Transport::MAX_MESSAGE) { _transport->reset = true; return false; }

  Transport::Message packet{nullptr, message.length(), _session};
  packet.data = static_cast<char *>(p_malloc(packet.length + 1));
  if (!packet.data) { _transport->reset = true; return false; }
  memcpy(packet.data, message.c_str(), packet.length + 1);
  if (xQueueSend(_transport->outgoing, &packet, 0) == pdTRUE) return true;
  free(packet.data);
  _transport->reset = true;
  return false;
}

// All tool callbacks stay on WLED's main loop. Session tags discard stale commands
// and responses after reconnects, and connection notification precedes the first RPC.
void WebSocketMCP::loop() {
  reapTransport();
  auto *state = _transport;
  if (!state) return;

  state->clockSeconds = toki.second();
  _connectionDetail = state->detail.load();

  if (!state->started) {
    if (_retiring) return;
    if (state->lastStartAttempt && millis() - state->lastStartAttempt < 1000) return;
    if (!state->start()) return; // Retry after temporary task/stack allocation failure.
  }
  Transport::Reference reference(state);
  bool online = state->online && !state->reset;
  uint32_t session = state->session;
  if (connected && (!online || _session != session)) {
    connected = false;
    lastPingTime = 0;
    if (connectionCallback) connectionCallback(false);
    if (_transport != state || state->stop || state->reset) return;
  }
  if (online && !connected) {
    _session = session;
    connected = true;
    lastPingTime = 0;
    if (connectionCallback) connectionCallback(true);
    if (_transport != state || state->stop || state->reset) return;
  }
  if (!online) return;
  Transport::Message packet;
  for (size_t i = 0; i < Transport::MAX_RPC_PER_LOOP; i++) {
    // Reserve room for both initialize replies before invoking another RPC.
    if (uxQueueSpacesAvailable(state->outgoing) < 2 || xQueueReceive(state->incoming, &packet, 0) != pdTRUE) break;
    if (connected && state->online && !state->reset && packet.session == _session && state->session == _session) {
      String message;
      if (message.concat(packet.data, packet.length)) handleJsonRpcMessage(message);
      else state->reset = true;
    }
    free(packet.data);
    if (_transport != state || state->stop || state->reset) return;
  }
  if (connected && lastPingTime && millis() - lastPingTime > 120000) disconnect();
}

bool WebSocketMCP::isConnected() { return connected; }

// Disabling the usermod must also release its asynchronous network resources.
void WebSocketMCP::end() {
  bool wasConnected = connected;
  stopTransport();
  if (wasConnected && connectionCallback) connectionCallback(false);
}

// As before, disconnect resets the connection and automatic reconnection remains enabled.
void WebSocketMCP::disconnect() {
  if (_transport) _transport->reset = true;
  bool wasConnected = connected.exchange(false);
  lastPingTime = 0;
  if (wasConnected && connectionCallback) connectionCallback(false);
}

void WebSocketMCP::handleJsonRpcMessage(const String &message) {
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    XIAOZHI_MCP_WARNLN("[xiaozhi-mcp] Failed to parse JSON: " + String(error.c_str()));
    return;
  }
  
  // Check whether this is a ping request
  if (doc.containsKey("method") && doc["method"] == "ping") {
    // Record the last ping time
    lastPingTime = millis();
    
    // Build the pong response - echo the original id without modification
    String id = doc["id"].as<String>();
    XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Received ping request: " + id);

    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}";
    sendMessage(response);

    XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Responded to ping request: " + id);
  }
  // Handle initialize requests
  else if (doc.containsKey("method") && doc["method"] == "initialize") {
    String id = doc["id"].as<String>();
    
    String serverName = "ESP-HA"; 

    // Send the initialize response
    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + 
      ",\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"experimental\":{},\"prompts\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,\"listChanged\":false},\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"" + serverName + "\",\"version\":\"1.0.0\"}}}";
    
    sendMessage(response);
    XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Responded to initialize request");
    
    // Send the initialized notification
    sendMessage("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
  }
  // Handle tools/list requests
  else if (doc.containsKey("method") && doc["method"] == "tools/list") {
    String id = doc["id"].as<String>();
    
    // This can be customized to match specific requirements
    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + 
      ",\"result\":{\"tools\":[";
    
    // Build tool metadata from the registered tool list
    if (_tools.size() > 0) {
      for (size_t i = 0; i < _tools.size(); i++) {
        if (i > 0) {
          response += ",";
        }
        response += "{\"name\":\"" + _tools[i].name + "\",";
        response += "\"description\":\"" + _tools[i].description + "\",";
        response += "\"inputSchema\":" + _tools[i].inputSchema + "}";
      }
    }
    
    response += "]}}";
    
    sendMessage(response);
    XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Responded to tools/list request, total tools: " + String(_tools.size()));
  }
  // Handle tools/call requests
  else if (doc.containsKey("method") && doc["method"] == "tools/call") {
    int id = doc["id"].as<int>();
    String toolName = doc["params"]["name"].as<String>();
    JsonObject arguments = doc["params"]["arguments"].as<JsonObject>();
    
    XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Received tool call request: " + toolName);
    
    // Find the tool
    bool toolFound = false;
    ToolResponse toolResponse;
    
    for (size_t i = 0; i < _tools.size(); i++) {
      if (_tools[i].name == toolName) {
        toolFound = true;
        // Invoke the tool callback with the arguments and get the result
        if (_tools[i].callback) {
          String argumentsJson;
          serializeJson(arguments, argumentsJson);
          
          // Invoke the callback and retrieve the structured result
          toolResponse = _tools[i].callback(argumentsJson);
        } else {
          toolResponse = ToolResponse("{\"error\":\"Tool callback not registered\"}", true);
        }
        break;
      }
    }
    
    if (!toolFound) {
      toolResponse = ToolResponse("{\"error\":\"Tool not found: " + toolName + "\"}", true);
    }
    
    // Build the response
    DynamicJsonDocument responseDoc(2048);
    responseDoc["jsonrpc"] = "2.0";
    responseDoc["id"] = id;
    
    JsonObject result = responseDoc.createNestedObject("result");
    
    JsonArray content = result.createNestedArray("content");
    for (const auto& item : toolResponse.content) {
      JsonObject contentItem = content.createNestedObject();
      contentItem["type"] = item.type;
      contentItem["text"] = item.text;
    }

    result["isError"] = toolResponse.isError;
    
    String response;
    serializeJson(responseDoc, response);
    
    sendMessage(response);
    XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Tool call completed: " + toolName + (toolResponse.isError ? " (error)" : ""));
  }
}

// Escape special characters in JSON strings
String WebSocketMCP::escapeJsonString(const String &input) {
  String result = "";
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\"' || c == '\\' || c == '/' || 
        c == '\b' || c == '\f' || c == '\n' || 
        c == '\r' || c == '\t') {
      if (c == '\"') result += "\\\"";
      else if (c == '\\') result += "\\\\";
      else if (c == '/') result += "\\/";
      else if (c == '\b') result += "\\b";
      else if (c == '\f') result += "\\f";
      else if (c == '\n') result += "\\n";
      else if (c == '\r') result += "\\r";
      else if (c == '\t') result += "\\t";
    } else {
      result += c;
    }
  }
  return result;
}

// Register a tool with a callback function
bool WebSocketMCP::registerTool(const String &name, const String &description, 
                              const String &inputSchema, ToolCallback callback) {
  // Check whether the tool already exists
  for (size_t i = 0; i < _tools.size(); i++) {
    if (_tools[i].name == name) {
      // If it already exists, you may choose to replace the callback
      _tools[i].callback = callback;
      XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Updated tool callback: " + name);
      return true;
    }
  }
  
  // Create a new tool and add it to the list
  Tool newTool;
  newTool.name = name;
  newTool.description = description;
  newTool.inputSchema = inputSchema;
  newTool.callback = callback;
  
  _tools.push_back(newTool);
  XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Successfully registered tool: " + name);
  return true;
}

// Add a simplified tool registration method
bool WebSocketMCP::registerSimpleTool(const String &name, const String &description, 
                                    const String &paramName, const String &paramDesc, 
                                    const String &paramType, ToolCallback callback) {
  // Build a simple inputSchema
  String inputSchema = "{\"type\":\"object\",\"properties\":{\"" + 
                      paramName + "\":{\"type\":\"" + paramType + 
                      "\",\"description\":\"" + paramDesc + 
                      "\"}},\"required\":[\"" + paramName + "\"]}";
                      
  return registerTool(name, description, inputSchema, callback);
}

// Unregister a tool
bool WebSocketMCP::unregisterTool(const String &name) {
  for (size_t i = 0; i < _tools.size(); i++) {
    if (_tools[i].name == name) {
      _tools.erase(_tools.begin() + i);
      XIAOZHI_MCP_DEBUGLN("[xiaozhi-mcp] Unregistered tool: " + name);
      return true;
    }
  }
  XIAOZHI_MCP_WARNLN("[xiaozhi-mcp] Tool " + name + " does not exist, cannot unregister");
  return false;
}

// Get the tool count
size_t WebSocketMCP::getToolCount() {
  return _tools.size();
}

// Clear all tools
void WebSocketMCP::clearTools() {
  _tools.clear();
  XIAOZHI_MCP_DEBUGLN("[WebSocketMCP] Cleared all tools");
}

// Format a JSON string so each key-value pair appears on its own line
String WebSocketMCP::formatJsonString(const String &jsonStr) {
  // 1. Handle empty strings or invalid JSON
  if (jsonStr.length() == 0) {
    return "{}";
  }

  // 2. Attempt to parse JSON to verify validity
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    // If parsing fails, return the original string
    return jsonStr;
  }
  
  // 3. Initialize the result string
  String result = "{\n";
  
  // 4. Get all keys in the JSON object
  JsonObject obj = doc.as<JsonObject>();
  bool firstItem = true;
  
  // 5. Iterate through each key-value pair in the JSON object
  for (JsonPair p : obj) {
    if (!firstItem) {
      result += ",\n";
    }
    firstItem = false;
    
    // Add two spaces of indentation
    result += "  \"" + String(p.key().c_str()) + "\": ";
    
    // Format the value according to its type
    if (p.value().is<JsonObject>() || p.value().is<JsonArray>()) {
      // For objects or arrays, use serializeJson
      String nestedJson;
      serializeJson(p.value(), nestedJson);
      result += nestedJson;
    } else if (p.value().is<const char*>() || p.value().is<String>()) {
      // For strings, add quotes
      result += "\"" + String(p.value().as<const char*>()) + "\"";
    } else {
      // For other types (numbers, booleans, etc.)
      String valueStr;
      serializeJson(p.value(), valueStr);
      result += valueStr;
    }
  }
  
  // 6. Add the closing brace on a new line
  result += "\n}";
  
  return result;
}

