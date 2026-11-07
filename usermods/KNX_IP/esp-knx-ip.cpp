#include "esp-knx-ip.h"
#include "knx_ip_protocol.h"
#ifndef UNIT_TEST
#include "src/dependencies/network/Network.h"
#endif

// ======== UDP Debug Support ========
// #ifdef KNX_UDP_DEBUG && !defined(UNIT_TEST)
#ifdef KNX_UDP_DEBUG
#include <WiFiUdp.h>
static WiFiUDP debugUdp;
static IPAddress debugTarget;
static bool debugUdpInit = false;

static void initUdpDebug() {
  if (debugUdpInit) return;
  debugTarget = IPAddress(255, 255, 255, 255); // Broadcast initially
  debugUdp.begin(0);                           // Any available port
  debugUdpInit = true;
}

void sendUdpDebug(const char* fmt, ...) {
  if (!debugUdpInit) initUdpDebug();
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  debugUdp.beginPacket(debugTarget, 5140); // Port 5140 for debug
  debugUdp.print(buf);
  debugUdp.endPacket();
}
#else
void sendUdpDebug(const char* fmt, ...) {
  (void)fmt;
}
#endif

// ======== Network abstraction for testing ========
class KnxNetworkInterface {
public:
#ifndef UNIT_TEST
  static IPAddress localIP() { return WLEDNetwork.localIP(); }
  static void localMAC(uint8_t* mac) { WLEDNetwork.localMAC(mac); }
  static bool isConnected() { return WLEDNetwork.isConnected(); }
#else
  static IPAddress localIP() { return WiFi.localIP(); }
  static void localMAC(uint8_t* mac) { WiFi.macAddress(mac); }
  static bool isConnected() { return WiFi.status() == WL_CONNECTED; }
#endif
};

// KNX UDP RX buffer (routing frames are small; 512 is plenty)
static constexpr size_t UDP_BUF_SIZE = 512;

// ======== Global instance as declared in header ========
KnxIpCore KNX;

// ======== Begin / End / Loop ========
bool KnxIpCore::begin() {
  KNX_UDP_LOG("KnxIpCore::begin() called");
  if (_running) {
    KNX_UDP_LOG("Already running, returning true");
    return true;
  }
  if (!KnxNetworkInterface::isConnected()) {
    KNX_UDP_LOG("ERROR: Network not connected");
    KNX_LOG("begin(): Network not connected.");
    return false;
  }

  IPAddress localIP = KnxNetworkInterface::localIP();
  KNX_UDP_LOG("Local IP: %s", localIP.toString().c_str());

  // Create UDP socket
  _sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (_sock < 0) {
    _rxErrors++;
    KNX_UDP_LOG("ERROR: socket() failed errno=%d", errno);
    KNX_LOG("begin(): socket() failed errno=%d", errno);
    return false;
  }
  KNX_UDP_LOG("Socket created: %d", _sock);

  // Allow address reuse
  int yes = 1;
  (void)::setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  // Bind to INADDR_ANY:3671
  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = htons(KNX_IP_UDP_PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(_sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
    _rxErrors++;
    KNX_UDP_LOG("ERROR: bind() failed errno=%d", errno);
    KNX_LOG("begin(): bind() failed errno=%d", errno);
    ::close(_sock);
    _sock = -1;
    return false;
  }
  KNX_UDP_LOG("Socket bound to port %d", KNX_IP_UDP_PORT);

  in_addr maddr{};
  maddr.s_addr = inet_addr("224.0.23.12"); // KNX group
  in_addr ifaddr{};
  ifaddr.s_addr = inet_addr(KnxNetworkInterface::localIP().toString().c_str());
  _lastIfAddr = ifaddr;

  KNX_UDP_LOG("Joining multicast 224.0.23.12 on interface %s", KnxNetworkInterface::localIP().toString().c_str());

  // Join multicast group on all interfaces (or use ifaddr here if you prefer)
  ip_mreq mreq{};
  mreq.imr_multiaddr = maddr;
  mreq.imr_interface = ifaddr;
  if (::setsockopt(_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    _rxErrors++;
    KNX_UDP_LOG("ERROR: IP_ADD_MEMBERSHIP failed errno=%d", errno);
    KNX_LOG("begin(): IP_ADD_MEMBERSHIP failed errno=%d", errno);
  } else {
    KNX_UDP_LOG("Successfully joined multicast group");
  }

  // TTL=1 and LOOP=1 are fine…
  uint8_t ttl = 1;
  (void)::setsockopt(_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  uint8_t loop = 1;
  (void)::setsockopt(_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  // Pin outgoing multicast to the STA interface
  if (::setsockopt(_sock, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
    KNX_UDP_LOG("ERROR: IP_MULTICAST_IF failed errno=%d", errno);
    KNX_LOG("begin(): IP_MULTICAST_IF failed errno=%d", errno);
  } else {
    KNX_UDP_LOG("Multicast interface set successfully");
  }
  // Prepare sockaddr for send()
  memset(&_mcastAddr, 0, sizeof(_mcastAddr));
  _mcastAddr.sin_family = AF_INET;
  _mcastAddr.sin_port = htons(KNX_IP_UDP_PORT);
  _mcastAddr.sin_addr = maddr;

  // Non-blocking
  fcntl(_sock, F_SETFL, O_NONBLOCK);

  KNX_LOG("begin(): joined %u.%u.%u.%u:%u (sock=%d)",
          _maddr[0], _maddr[1], _maddr[2], _maddr[3],
          (unsigned)KNX_IP_UDP_PORT, _sock);

  _running = true;
  KNX_UDP_LOG("KnxIpCore::begin() completed successfully, running=%d", _running);
  return true;
}

void KnxIpCore::end() {
  if (!_running) return;
  if (_sock >= 0) {
    ::close(_sock);
    _sock = -1;
  }
  _running = false;
}

bool KnxIpCore::rejoinMulticast() {
  if (!_running || _sock < 0) {
    KNX_LOG("rejoinMulticast(): core not running.");
    return false;
  }

  in_addr maddr{};
  inet_aton("224.0.23.12", &maddr);
  in_addr ifaddr{};
  inet_aton(KnxNetworkInterface::localIP().toString().c_str(), &ifaddr);

  // If interface changed, drop old membership first to avoid stale IGMP state
  if (_lastIfAddr.s_addr != 0 && _lastIfAddr.s_addr != ifaddr.s_addr) {
    ip_mreq drop{};
    drop.imr_multiaddr = maddr;
    drop.imr_interface = _lastIfAddr;
    (void)::setsockopt(_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &drop, sizeof(drop));
  }

  // (Re)join multicast group on current interface
  ip_mreq mreq{};
  mreq.imr_multiaddr = maddr;
  mreq.imr_interface = ifaddr;
  if (::setsockopt(_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    KNX_LOG("rejoinMulticast(): IP_ADD_MEMBERSHIP failed errno=%d", errno);
    return false;
  }

  // Re-apply multicast socket options that some stacks reset
  uint8_t ttl = 1;
  (void)::setsockopt(_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  uint8_t loop = 1;
  (void)::setsockopt(_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  // Pin outgoing multicast to Wi-Fi STA again (some drivers reset on config changes)
  if (::setsockopt(_sock, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
    KNX_LOG("rejoinMulticast(): IP_MULTICAST_IF failed errno=%d", errno);
    // not fatal – continue
  }
  _lastIfAddr = ifaddr;

  KNX_LOG("rejoinMulticast(): refreshed membership for 224.0.23.12 on %s",
          KnxNetworkInterface::localIP().toString().c_str());
  return true;
}

void KnxIpCore::loop() {
  if (!_running) return;

  uint8_t buf[UDP_BUF_SIZE];
  struct sockaddr_in from;
  socklen_t flen = sizeof(from);
  int len = ::recvfrom(_sock, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
  if (len <= 0) return; // EWOULDBLOCK

  KNX_UDP_LOG("Received packet: %d bytes from %d.%d.%d.%d",
              len,
              (int)((ntohl(from.sin_addr.s_addr) >> 24) & 0xFF),
              (int)((ntohl(from.sin_addr.s_addr) >> 16) & 0xFF),
              (int)((ntohl(from.sin_addr.s_addr) >> 8) & 0xFF),
              (int)(ntohl(from.sin_addr.s_addr) & 0xFF));

  // debug
  knx_dump_hex("RX packet", buf, len);
  _handleIncoming(buf, len);
}

// ======== Public TX API ========
bool KnxIpCore::_sendSearchResponse(bool extended, const uint8_t* req, int reqLen) {
  // Reply by unicast to the HPAI supplied in the request.
  // Search requests place the HPAI at offset 6 (length 8, IPv4/UDP, IP, port).
  struct sockaddr_in to{};
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  if (reqLen >= 14 && req[0] == 0x06 && req[1] == KNX_PROTOCOL_VERSION && req[6] == 0x08 && req[7] == 0x01) {
    // IP (8..11), Port (12..13)
    uint32_t ip;
    memcpy(&ip, &req[8], 4);
    uint16_t port;
    memcpy(&port, &req[12], 2);
    to.sin_addr.s_addr = ip;
    to.sin_port = *(uint16_t*)&req[12]; // Preserve network byte order.
  } else {
    // Preserve the broadcast fallback for requests without a complete IPv4 HPAI.
    to.sin_addr.s_addr = inet_addr("255.255.255.255");
    to.sin_port = htons(KNX_IP_UDP_PORT);
  }

  // HPAI describing this device's control endpoint.
  uint8_t hpai[8];
  hpai[0] = 0x08;
  hpai[1] = 0x01; // len, IPv4/UDP
  IPAddress ip = KnxNetworkInterface::localIP();
  hpai[2] = ip[0];
  hpai[3] = ip[1];
  hpai[4] = ip[2];
  hpai[5] = ip[3];
  hpai[6] = (uint8_t)(KNX_IP_UDP_PORT >> 8);
  hpai[7] = (uint8_t)(KNX_IP_UDP_PORT & 0xFF);

  // Device information DIB (type 0x01, fixed length 0x36).
  uint8_t dib_dev[0x36];
  memset(dib_dev, 0, sizeof(dib_dev));
  dib_dev[0] = 0x36;
  dib_dev[1] = 0x01; // len, type
  dib_dev[2] = 0x20; // Medium IP
  dib_dev[3] = 0x00; // Device status OK
  // Individual address, or zero if not configured.
  dib_dev[4] = (uint8_t)(_pa >> 8);
  dib_dev[5] = (uint8_t)(_pa & 0xFF);
  // Project/Installation ID:
  dib_dev[6] = 0x00;
  dib_dev[7] = 0x00;
  // Use the MAC address as the serial number.
  uint8_t mac[6];
  KnxNetworkInterface::localMAC(mac);
  memcpy(&dib_dev[8], mac, 6);
  // Multicast 224.0.23.12
  dib_dev[14] = 224;
  dib_dev[15] = 0;
  dib_dev[16] = 23;
  dib_dev[17] = 12;
  // MAC again
  memcpy(&dib_dev[18], mac, 6);
  // Friendly name, at most 30 bytes including the terminator.
  extern char serverDescription[33]; // Provided by the WLED core.
  const char* fallbackName = "WLED KNX";
  const char* name = (serverDescription[0] != '\0') ? serverDescription : fallbackName;
  strncpy((char*)&dib_dev[24], name, 30);
  dib_dev[24 + 29] = '\0'; // Always terminate the name.
  // Remaining bytes stay zero for padding.

  // Supported service families DIB (type 0x02).
  // Minimal: Core v1, Routing v1
  uint8_t dib_svc[10];
  memset(dib_svc, 0, sizeof(dib_svc));
  dib_svc[0] = 0x0A;
  dib_svc[1] = 0x02;
  dib_svc[2] = 0x02;
  dib_svc[3] = 0x01; // Core v1
  dib_svc[4] = 0x05;
  dib_svc[5] = 0x01; // Routing v1
  // Tunneling is not advertised.

  // Extended requests use the same DIB payload for this routing-only implementation.

  // --- KNXnet/IP Header ---
  // 06 10 02 02/0C <len>
  const uint16_t svc = extended ? KNX_SVC_SEARCH_RES_EXT : KNX_SVC_SEARCH_RES;
  const size_t payloadLen = 6 + sizeof(hpai) + sizeof(dib_dev) + sizeof(dib_svc);
  uint8_t* pkt = (uint8_t*)malloc(payloadLen);
  if (!pkt) {
    KNX_LOG("SearchResponse: OOM");
    return false;
  }

  size_t p = 0;
  pkt[p++] = 0x06;
  pkt[p++] = KNX_PROTOCOL_VERSION;
  pkt[p++] = (uint8_t)(svc >> 8);
  pkt[p++] = (uint8_t)(svc & 0xFF);
  pkt[p++] = (uint8_t)(payloadLen >> 8);
  pkt[p++] = (uint8_t)(payloadLen & 0xFF);

  memcpy(&pkt[p], hpai, sizeof(hpai));
  p += sizeof(hpai);
  memcpy(&pkt[p], dib_dev, sizeof(dib_dev));
  p += sizeof(dib_dev);
  memcpy(&pkt[p], dib_svc, sizeof(dib_svc));
  p += sizeof(dib_svc);

  // Send the discovery response.
  ssize_t sent = ::sendto(_sock, pkt, payloadLen, 0, (struct sockaddr*)&to, sizeof(to));
  bool ok = (sent == (ssize_t)payloadLen);
  if (!ok)
    _txErrors++;
  else
    _txPackets++;
  KNX_LOG("TX: %s (%u bytes) to %s:%u",
          extended ? "SearchResponseExtended" : "SearchResponse",
          (unsigned)payloadLen,
          ip.toString().c_str(), KNX_IP_UDP_PORT);
  free(pkt);
  return ok;
}
