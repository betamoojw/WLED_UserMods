#pragma once

// ======== Tunables / constants ========
static constexpr uint8_t KNX_PROTOCOL_VERSION = 0x10;      // KNXnet/IP proto version
static constexpr uint16_t KNX_SVC_ROUTING_IND = 0x0530;    // Routing Indication (rx)
static constexpr uint8_t CEMI_LDATA_IND = 0x29;            // cEMI L_Data.ind (rx)
static constexpr uint16_t KNX_SVC_SEARCH_REQ = 0x0201;     // SearchRequest
static constexpr uint16_t KNX_SVC_SEARCH_RES = 0x0202;     // SearchResponse
static constexpr uint16_t KNX_SVC_SEARCH_REQ_EXT = 0x020B; // SearchRequestExtended
static constexpr uint16_t KNX_SVC_SEARCH_RES_EXT = 0x020C; // SearchResponseExtended

// cEMI Control fields defaults:
//  - Standard frame, no repeat suppression, priority=Low
//  - Group Address, hop count 6 (typical), ACK disabled
static constexpr uint8_t CEMI_CTRL1_DEFAULT = 0xBC;   // std frame, low prio
static constexpr uint8_t CEMI_CTRL2_GROUP_HC6 = 0xE0; // 1xxxxxxx (group) + hop=6

// small local hex-dump helper for debugging (first up to 96 bytes)
static void knx_dump_hex(const char* tag, const uint8_t* data, size_t len) {
  if (!data || !len) {
    return;
  }

  const size_t maxDump = len < 96 ? len : 96;
  KNX_LOG("%s (%u bytes): ", tag, (unsigned)len);

  for (size_t i = 0; i < maxDump; ++i) {
#ifdef KNX_DEBUG
    Serial.printf("%02X", data[i]);
    if (i + 1 < maxDump) Serial.print(' ');
#endif
  }

#ifdef KNX_DEBUG
  if (maxDump < len) Serial.print(" ...");
  Serial.print('\n');
#endif
}
