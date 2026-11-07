#include "esp-knx-ip.h"
#include "knx_ip_protocol.h"

bool KnxIpCore::groupValueWrite(uint16_t ga, const uint8_t* data, uint8_t len) {
  return sendCemiToGroup(ga, KnxService::GroupValue_Write, data, len);
}

bool KnxIpCore::groupValueRead(uint16_t ga) {
  // No ASDU for read
  return sendCemiToGroup(ga, KnxService::GroupValue_Read, nullptr, 0);
}

bool KnxIpCore::groupValueResponse(uint16_t ga, const uint8_t* data, uint8_t len) {
  return sendCemiToGroup(ga, KnxService::GroupValue_Response, data, len);
}

// ======== Low-level send ========
bool KnxIpCore::sendCemiToGroup(uint16_t ga, KnxService svc, const uint8_t* asdu, uint8_t asduLen) {
  if (!_running) {
    KNX_LOG("TX: not running, drop.");
    return false;
  }

  // ---- decide true 1-bit telegram by DPT, not by len ----
  bool oneBit = false;
  auto it = _gos.find(ga);
  if (it != _gos.end()) {
    oneBit = (it->second.dpt == DptMain::DPT_1xx);
  } else {
    // if GA unknown, be conservative: treat as multi-byte (no embedded bit)
    oneBit = false;
  }

  // Read has no ASDU
  if (svc == KnxService::GroupValue_Read) {
    asdu = nullptr;
    asduLen = 0;
  }

  // ---- build cEMI L_Data.ind ----
  uint8_t cemi[64];
  uint8_t idx = 0;

  const uint8_t msgCode = CEMI_LDATA_IND;
  const uint8_t addInfo = 0x00;

  cemi[idx++] = msgCode;
  cemi[idx++] = addInfo;
  cemi[idx++] = CEMI_CTRL1_DEFAULT;   // 0xBC
  cemi[idx++] = CEMI_CTRL2_GROUP_HC6; // 0xE0
  cemi[idx++] = (uint8_t)(_pa >> 8);
  cemi[idx++] = (uint8_t)(_pa & 0xFF);
  cemi[idx++] = (uint8_t)(ga >> 8);
  cemi[idx++] = (uint8_t)(ga & 0xFF);

  // TPDU header
  const uint8_t tpdu0 = 0x00; // UDT group
  uint8_t tpdu1 = 0x00;
  switch (svc) {
    case KnxService::GroupValue_Read:
      tpdu1 = (0b00 << 6);
      break;
    case KnxService::GroupValue_Response:
      tpdu1 = (0b01 << 6);
      break;
    case KnxService::GroupValue_Write:
      tpdu1 = (0b10 << 6);
      break;
  }

  // APDU bytes (incl. TPDU0 + TPDU1 + payload); cEMI "length" is APDU-1
  const uint8_t apduBytes = oneBit ? 2 : (uint8_t)(2 + asduLen);
  cemi[idx++] = (uint8_t)(apduBytes - 1);

  // Write TPDU and payload
  cemi[idx++] = tpdu0;
  if (oneBit && asdu) {
    // embed the bit into TPDU[1] LSB (only for DPT_1xx)
    const uint8_t bit = (asdu[0] & 0x01);
    cemi[idx++] = (uint8_t)(tpdu1 | bit);
  } else {
    // multi-byte payload (or read/unknown GA)
    cemi[idx++] = tpdu1;
    for (uint8_t i = 0; i < asduLen; ++i) cemi[idx++] = asdu[i];
  }

  // ---- KNXnet/IP routing wrapper ----
  uint8_t frame[96];
  uint8_t p = 0;
  const uint16_t totalLen = (uint16_t)(6 + idx);

  frame[p++] = 0x06;                 // header size
  frame[p++] = KNX_PROTOCOL_VERSION; // 0x10
  frame[p++] = (uint8_t)(KNX_SVC_ROUTING_IND >> 8);
  frame[p++] = (uint8_t)(KNX_SVC_ROUTING_IND & 0xFF);
  frame[p++] = (uint8_t)(totalLen >> 8);
  frame[p++] = (uint8_t)(totalLen & 0xFF);

  for (uint8_t i = 0; i < idx; ++i) frame[p++] = cemi[i];

  // debug (optional)
  knx_dump_hex("TX frame", frame, p);

  // ---- send to 224.0.23.12:3671 ----
  // Tasmota-style redundancy: send the same frame multiple times if enabled
  const uint8_t repeats = _enhanced ? _enhancedSendCount : 1;
  for (uint8_t i = 0; i < repeats; ++i) {
    ssize_t sent = ::sendto(_sock, frame, p, 0, (struct sockaddr*)&_mcastAddr, sizeof(_mcastAddr));
    if (sent != p) {
      _txErrors++;
      KNX_LOG("TX: sendto()=%d/%d errno=%d txErrors=%u", (int)sent, p, errno, (unsigned)_txErrors);
      return false;
    }
    _txPackets++;
    KNX_LOG("TX: sent %d bytes rpt=%u/%u (txPackets=%u)", p, (unsigned)(i + 1), (unsigned)repeats, (unsigned)_txPackets);
    if (_enhancedGapMs) delay(_enhancedGapMs);
  }
  return true;
}

// ======== Internal RX path ========
void KnxIpCore::_handleIncoming(const uint8_t* buf, int len) {
  // Validate KNXnet/IP header (min 6 bytes)
  if (len < 6) {
    _rxErrors++;
    KNX_LOG("RX: too short (%d).", len);
    return;
  }

  const uint8_t headerSize = buf[0];
  const uint8_t proto = buf[1];
  const uint16_t svc = (uint16_t(buf[2]) << 8) | buf[3];
  const uint16_t totalLen = (uint16_t(buf[4]) << 8) | buf[5];

  if (headerSize != 0x06 || proto != KNX_PROTOCOL_VERSION) {
    _rxErrors++;
    KNX_LOG("RX: bad header: size=0x%02X proto=0x%02X.", headerSize, proto);
    return;
  }
  if (totalLen != len) {
    if (totalLen > len) {
      _rxErrors++;
      KNX_LOG("RX: totalLen(%u)>len(%d).", (unsigned)totalLen, len);
      return;
    }
  }

  // Accept SearchRequest(Extended) and Routing Indication
  if (svc == KNX_SVC_SEARCH_REQ || svc == KNX_SVC_SEARCH_REQ_EXT) {
    _sendSearchResponse(svc == KNX_SVC_SEARCH_REQ_EXT, buf, len);
    return;
  }

  if (svc != KNX_SVC_ROUTING_IND) {
    KNX_LOG("RX: ignore svc=0x%04X (not Routing_Ind).", (unsigned)svc);
    return;
  }

  if (len < 6 + 10) {
    _rxErrors++;
    KNX_LOG("RX: cEMI too short (len=%d).", len);
    return;
  }

  const uint8_t* cemi = buf + 6;
  const int cemiLen = len - 6;

  const uint8_t msgCode = cemi[0];
  const uint8_t addInfo = cemi[1];
  (void)addInfo;

  if (msgCode != CEMI_LDATA_IND) {
    KNX_LOG("RX: msgCode=0x%02X not L_Data.ind.", msgCode);
    return;
  }
  if (cemiLen < 10) {
    _rxErrors++;
    KNX_LOG("RX: cEMI header truncated (cemiLen=%d).", cemiLen);
    return;
  }

  const uint8_t ctrl1 = cemi[2];
  const uint8_t ctrl2 = cemi[3];
  (void)ctrl1;

  const uint16_t src = (uint16_t(cemi[4]) << 8) | cemi[5];
  const uint16_t dst = (uint16_t(cemi[6]) << 8) | cemi[7];

  // cEMI [8] = APDU length minus 1
  const uint8_t apduLenMinus1 = cemi[8];
  const uint8_t apduBytes = uint8_t(apduLenMinus1 + 1); // TPDU0+TPDU1+payload

  // TPDU starts at cemi[9]
  if (cemiLen < 9 + apduBytes) {
    _rxErrors++;
    KNX_LOG("RX: TPDU truncated (need %u, have %d).", (unsigned)apduBytes, cemiLen - 9);
    return;
  }
  const uint8_t* tpdu = cemi + 9;
  if (apduBytes < 2) {
    _rxErrors++;
    KNX_LOG("RX: APDU < 2 bytes.");
    return;
  }

  // Must be group address (ctrl2 bit7)
  const bool isGroup = (ctrl2 & 0x80) != 0;
  if (!isGroup) {
    KNX_LOG("RX: dst=0x%04X not group (ctrl2=0x%02X).", dst, ctrl2);
    return;
  }

  // Drop our own multicast (mirrored GA protection)
  if (src == _pa && _pa != 0) {
    KNX_LOG("RX: own frame (src=%u.%u.%u) ignored.",
            (unsigned)((src >> 12) & 0x0F), (unsigned)((src >> 8) & 0x0F), (unsigned)(src & 0xFF));
    return;
  }

  // ---------- APCI & service ----------
  const uint8_t apci4 = uint8_t(((tpdu[0] & 0x03) << 2) | ((tpdu[1] & 0xC0) >> 6));
  KnxService svcDetected;
  switch (apci4) {
    case 0x0:
      svcDetected = KnxService::GroupValue_Read;
      break;
    case 0x1:
      svcDetected = KnxService::GroupValue_Response;
      break;
    case 0x2:
      svcDetected = KnxService::GroupValue_Write;
      break;
    default:
      KNX_LOG("RX: APCI=0x%X not handled.", apci4);
      return;
  }

  // ---------- ASDU extraction ----------
  const uint8_t* asdu = nullptr;
  uint8_t asduLen = 0;

  if (svcDetected == KnxService::GroupValue_Write && apduBytes == 2) {
    static uint8_t one;
    one = (tpdu[1] & 0x0F); // allow for DPT_1xx, DPT_2xx and DPT_3xx
    asdu = &one;
    asduLen = 1;
  } else if (apduBytes > 2) {
    asdu = tpdu + 2;
    asduLen = uint8_t(apduBytes - 2);
  }

  KNX_LOG("RX: src=%u.%u.%u dst=%u/%u/%u (0x%04X) apduBytes=%u svc=%u lenASDU=%u",
          (unsigned)((src >> 12) & 0x0F), (unsigned)((src >> 8) & 0x0F), (unsigned)(src & 0xFF),
          knxGaMain(dst), knxGaMiddle(dst), knxGaSub(dst), dst,
          (unsigned)apduBytes, (unsigned)svcDetected, (unsigned)asduLen);

  // ---------- Communication enhancement: RX de-dup + toggle throttle ----------
  if (_enhanced) {
    const uint32_t now = millis();

    // Build a small signature across src, dst, apci, and up to 4 bytes of payload
    uint32_t sig = ((uint32_t)src << 16) ^ (uint32_t)dst ^ ((uint32_t)apci4 << 28);
    if (asdu && asduLen) {
      uint32_t d = 0;
      memcpy(&d, asdu, (asduLen >= 4 ? 4 : asduLen));
      sig ^= _mix32(d + ((uint32_t)asduLen << 24));
    }
    sig = _mix32(sig);

    // Deduplicate within window
    for (size_t i = 0; i < _rxSeen.size(); ++i) {
      const RxSig& r = _rxSeen[i];
      if (r.sig == sig && (now - r.ts) <= _rxDedupWindowMs) {
        KNX_LOG("RX: duplicate suppressed (sig=0x%08X, %ums)", (unsigned)sig, (unsigned)(now - r.ts));
        return;
      }
    }
    _rxSeen[_rxSeenIdx] = RxSig{sig, now};
    _rxSeenIdx = (_rxSeenIdx + 1) % _rxSeen.size();

    // AI: below section was generated by an AI
    // Byte length alone cannot distinguish switches from DPT3 dimming or DPT5 levels.
    const auto object = _gos.find(dst);
    if (object != _gos.end() && object->second.dpt == DptMain::DPT_1xx &&
        svcDetected == KnxService::GroupValue_Write && asdu && asduLen == 1) {
    // AI: end
      const uint8_t bit = (asdu[0] & 0x01);
      auto& st = _rxBitCache[dst];
      if (st.ts != 0 && (now - st.ts) < 1000 && st.v != bit) {
        KNX_LOG("RX: toggle throttled on GA 0x%04X (%u->%u in %u ms)", dst, st.v, bit, (unsigned)(now - st.ts));
        return;
      }
      st.v = bit;
      st.ts = now;
    }
  }

  // ---------- Dispatch ----------
  auto itGo = _gos.find(dst);
  if (itGo != _gos.end()) {
    DptMain dpt = itGo->second.dpt;
    auto itCb = _callbacks.find(dst);
    if (itCb != _callbacks.end() && itCb->second) {
      itCb->second(dst, dpt, svcDetected, asdu, asduLen);
      KNX_LOG("RX: dispatched to GA 0x%04X callback.", dst);
    }
  } else {
    KNX_LOG("RX: no registered GA for 0x%04X.", dst);
  }

  _rxPackets++;
  KNX_LOG("RX: done (rxPackets=%u).", (unsigned)_rxPackets);
}

// ======== Compose and send APCI ========
bool KnxIpCore::_composeAndSendApci(uint16_t ga, KnxService svc, const uint8_t* asdu, uint8_t asduLen) {
  return sendCemiToGroup(ga, svc, asdu, asduLen);
}

void KnxIpCore::clearRegistrations() {
  _gos.clear();
  _callbacks.clear();
}

// ======== (Optional) convenience wrappers used by header ========
// (end of file)
