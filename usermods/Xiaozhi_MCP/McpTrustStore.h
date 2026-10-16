#pragma once

// Reuse the IDF 5.5 SDK's flash-resident roots without invoking its stripped TLS code.
// ESP_SSLClient 3.1.3 declares dynamic callbacks, but its bundled x509_minimal.c
// never calls them. Use the supported static anchor array instead.
// Format: https://github.com/espressif/esp-idf/blob/v5.5.4/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c
// Validator: https://www.bearssl.org/apidoc/bearssl__x509_8h.html
extern const uint8_t mcpBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t mcpBundleEnd[] asm("_binary_x509_crt_bundle_end");

class McpTrustStore : public bssl::CertStoreBase {
  br_pkey_decoder_context _decoder;
  br_x509_trust_anchor* _anchors = nullptr;
  size_t _count = 0;

  static uint16_t read16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
  static uint32_t read32(const uint8_t* p) { return read16(p) | (uint32_t(read16(p + 2)) << 16); }

  // Find the exact decoded bytes in the immutable SPKI. BearSSL validates the
  // encoding; matching its output keeps moduli/EC points in flash. Its public-key
  // API uses non-const pointers, but the verifier only reads these bytes.
  static unsigned char* flashKey(const uint8_t* spki, size_t length, const unsigned char* key, size_t keyLength) {
    if (!key || !keyLength || keyLength > length) return nullptr;
    for (size_t i = 0; i <= length - keyLength; i++) {
      if (!memcmp(spki + i, key, keyLength)) return const_cast<unsigned char*>(spki + i);
    }
    return nullptr;
  }

  // Decode one trusted SDK entry, checking all offsets before reading flash.
  bool decodeAnchor(br_x509_trust_anchor& anchor, size_t offset, size_t tableSize, size_t size) {
    if (offset < tableSize || offset > size - 4) return false;
    const uint8_t* cert = mcpBundleStart + offset;
    size_t nameLength = read16(cert), keyLength = read16(cert + 2);
    if (!nameLength || !keyLength || nameLength + keyLength > size - offset - 4) return false;
    const uint8_t* spki = cert + 4 + nameLength;
    br_pkey_decoder_init(&_decoder);
    br_pkey_decoder_push(&_decoder, spki, keyLength);
    if (br_pkey_decoder_last_error(&_decoder)) return false;
    anchor = {};
    // Static anchors take the full DER subject name, not its hash.
    anchor.dn = {const_cast<unsigned char*>(cert + 4), nameLength};
    anchor.flags = BR_X509_TA_CA;
    anchor.pkey.key_type = br_pkey_decoder_key_type(&_decoder);
    if (anchor.pkey.key_type == BR_KEYTYPE_RSA) {
      auto& key = anchor.pkey.key.rsa;
      key = *br_pkey_decoder_get_rsa(&_decoder);
      key.n = flashKey(spki, keyLength, key.n, key.nlen);
      key.e = flashKey(spki, keyLength, key.e, key.elen);
      return key.n && key.e;
    }
    if (anchor.pkey.key_type == BR_KEYTYPE_EC) {
      auto& key = anchor.pkey.key.ec;
      key = *br_pkey_decoder_get_ec(&_decoder);
      key.q = flashKey(spki, keyLength, key.q, key.qlen);
      return key.q != nullptr;
    }
    return false;
  }

public:
  McpTrustStore() = default;
  McpTrustStore(const McpTrustStore&) = delete;
  McpTrustStore& operator=(const McpTrustStore&) = delete;
  ~McpTrustStore() { clear(); }

  // Allocate only descriptors; subjects and public-key bytes remain in flash.
  bool load() {
    if (_anchors) return true;
    size_t size = mcpBundleEnd - mcpBundleStart;
    if (size < 8) return false;
    size_t tableSize = read32(mcpBundleStart);
    if (!tableSize || tableSize % 4 || tableSize > size - 4) return false;
    size_t count = tableSize / 4;
    if (count > SIZE_MAX / sizeof(br_x509_trust_anchor)) return false;
    _anchors = static_cast<br_x509_trust_anchor*>(p_malloc(count * sizeof(br_x509_trust_anchor)));
    if (!_anchors) return false;
    for (size_t i = 0; i < count; i++) {
      if (!decodeAnchor(_anchors[i], read32(mcpBundleStart + i * 4), tableSize, size)) {
        clear();
        return false;
      }
    }
    _count = count;
    return true;
  }

  // Call after TLS stops so its verifier cannot retain freed descriptors.
  void clear() {
    free(_anchors);
    _anchors = nullptr;
    _count = 0;
  }

  // Preserve the library's already configured algorithms, time and name checks.
  void installCertStore(br_x509_minimal_context* context) override {
    context->trust_anchors = _anchors;
    context->trust_anchors_num = _count;
  }
};
