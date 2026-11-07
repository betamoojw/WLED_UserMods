#include "usermod_knx_ip.h"
#include "knx_control_helpers.h"
#include "wled.h" // access to global 'strip' and segments
#include "fcn_declare.h"
#include "DPT.h"
#include "esp-knx-ip.h" // for KNX_UDP_LOG macro
#include "src/dependencies/network/Network.h"
#include <sys/time.h>

#if defined(ESP32)
#include <esp_idf_version.h>
#include <esp_sntp.h>

// Stop the SDK SNTP client using the API available in the selected ESP-IDF version.
static void stopSntpClient() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_sntp_stop();
#else
  sntp_stop();
#endif
}
#endif

// Global error details buffer used by WLED GUI and usermods.
// Provide a weak definition here so the symbol exists if core doesn't provide it,
// but can be overridden by a stronger definition in the core firmware.
char errorDetails[256] __attribute__((weak)) = {'\0'};

// Helper: simple hash for config strings and segment offsets
uint32_t KnxIpUsermod::computeGATableHash() const {
  // FNV-1a hash
  uint32_t hash = 2166136261u;
  auto hashstr = [&](const char* s) {
    while (*s) {
      hash ^= (uint8_t)(*s++);
      hash *= 16777619u;
    }
  };
  // Hash all GA strings
  const char* gaStrings[] = {
    gaInPower, gaInBri, gaInR, gaInG, gaInB, gaInW, gaInCct, gaInWW, gaInCW, gaInH, gaInS, gaInV, gaInFx, gaInPreset, gaInRGB, gaInHSV, gaInRGBW, gaInTime, gaInDate, gaInDateTime,
    gaInBriRel, gaInRRel, gaInGRel, gaInBRel, gaInWRel, gaInWWRel, gaInCWRel, gaInHRel, gaInSRel, gaInVRel, gaInFxRel, gaInRGBRel, gaInHSVRel, gaInRGBWRel,
    gaOutPower, gaOutBri, gaOutR, gaOutG, gaOutB, gaOutW, gaOutCct, gaOutWW, gaOutCW, gaOutH, gaOutS, gaOutV, gaOutFx, gaOutPreset, gaOutRGB, gaOutHSV, gaOutRGBW,
    gaOutIntTemp, gaOutTemp, gaOutIntTempAlarm, gaOutTempAlarm};
  for (const char* s : gaStrings) hashstr(s);
  // Hash segment offsets
  hash ^= segmentOffsetL;
  hash *= 16777619u;
  hash ^= segmentOffsetM;
  hash *= 16777619u;
  hash ^= segmentOffsetN;
  hash *= 16777619u;
  return hash;
}

// WLED core globals from ntp.cpp
extern unsigned long ntpLastSyncTime;

extern "C" {
// Dallas usermod may (optionally) provide this. If not, we'll just skip Dallas.
float __attribute__((weak)) wled_get_temperature_c();
}

enum class LedProfile : uint8_t { MONO = 1,
                                  CCT,
                                  RGB,
                                  RGBW,
                                  RGBCCT };
static LedProfile g_ledProfile = LedProfile::RGB;

// Map segment light-capabilities to profile.
// lc bit0=RGB (1), bit1=White (2), bit2=CCT (4)
static LedProfile detectLedProfileFromSegments() {
  uint8_t lc = 0;
  const uint16_t n = strip.getSegmentsNum();
  for (uint16_t i = 0; i < n; i++) {
    lc |= strip.getSegment(i).getLightCapabilities();
  }
  if (lc == 0 && n > 0) lc = strip.getMainSegment().getLightCapabilities(); // fallback

  const bool hasRGB = lc & 0x01;
  const bool hasW = lc & 0x02;
  const bool hasCCT = lc & 0x04;

  if (hasRGB && hasCCT) return LedProfile::RGBCCT;
  if (hasRGB && hasW) return LedProfile::RGBW;
  if (hasRGB) return LedProfile::RGB;
  if (hasCCT) return LedProfile::CCT;
  if (hasW) return LedProfile::MONO;
  return LedProfile::MONO;
}

// Track last-sent preset so we only transmit GA_OUT_PRE when it changes  
static int s_lastPresetSent = -1; // -1 sentinel; currentPreset is byte

static float g_lastEspTemp = -999.0f;    // sentinel for ESP internal temp
static float g_lastDallasTemp = -999.0f; // sentinel for Dallas temp

// single global KNX instance is declared in esp-knx-ip.{h,cpp}
extern KnxIpCore KNX;

// ------------- helpers -------------
// Read one decimal address component, leaving p at its separator or terminator.
// Keep the existing 1000 cutoff and leading-zero acceptance for both address formats.
static bool parseAddressComponent(const char*& p, uint32_t& out) {
  if (*p < '0' || *p > '9') return false; // must start with digit
  uint32_t value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10u + (uint32_t)(*p - '0');
    if (value > 1000u) return false; // simple overflow guard
    ++p;
  }
  out = value;
  return true;
}

// Parse "x/y/z" -> GA (returns 0 if invalid). Strict unsigned parsing with range enforcement.
static uint16_t parseGA(const char* s) {
  if (!s || !*s) return 0;
  uint32_t a = 0, b = 0, c = 0;
  const char* p = s;
  if (!parseAddressComponent(p, a) || *p != '/') return 0;
  ++p;
  if (!parseAddressComponent(p, b) || *p != '/') return 0;
  ++p;
  if (!parseAddressComponent(p, c) || *p != '\0') return 0;
  if (a > 31u || b > 7u || c > 255u) return 0; // KNX 3-level GA limits
  return knxMakeGroupAddress((uint8_t)a, (uint8_t)b, (uint8_t)c);
}

// Parse individual address "x.x.x" -> 16-bit PA: area(4) | line(4) | dev(8). Returns 0 if invalid.
static uint16_t parsePA(const char* s) {
  if (!s || !*s) return 0;
  uint32_t area = 0, line = 0, dev = 0;
  const char* p = s;
  if (!parseAddressComponent(p, area) || *p != '.') return 0;
  ++p;
  if (!parseAddressComponent(p, line) || *p != '.') return 0;
  ++p;
  if (!parseAddressComponent(p, dev) || *p != '\0') return 0;
  if (area > 15u || line > 15u || dev > 255u) return 0; // strict
  return (uint16_t)((area & 0x0F) << 12) | (uint16_t)((line & 0x0F) << 8) | (uint16_t)(dev & 0xFF);
}

// Public validation wrappers (reuse parsing logic without logging)
bool KnxIpUsermod::validateGroupAddressString(const char* s) {
  return parseGA(s) != 0;
}
bool KnxIpUsermod::validateIndividualAddressString(const char* s) {
  return parsePA(s) != 0;
}

// Calculate per-segment GA from central GA + segment offset
uint16_t KnxIpUsermod::calculateSegmentGA(const char* centralGA, uint8_t segmentIndex) const {
  if (!centralGA || !*centralGA) return 0;

  uint16_t centralParsed = parseGA(centralGA);
  if (centralParsed == 0) return 0;

  // Extract central address components
  uint8_t centralMain = (centralParsed >> 11) & 0x1F;  // bits 15-11
  uint8_t centralMiddle = (centralParsed >> 8) & 0x07; // bits 10-8
  uint8_t centralSub = centralParsed & 0xFF;           // bits 7-0

  // Calculate segment address: Segment N = (main + L*N, middle + M*N, sub + N*N)
  uint16_t newMain = centralMain + (segmentOffsetL * segmentIndex);
  uint16_t newMiddle = centralMiddle + (segmentOffsetM * segmentIndex);
  uint16_t newSub = centralSub + (segmentOffsetN * segmentIndex);

  // Validate KNX limits
  if (newMain > 31 || newMiddle > 7 || newSub > 255) {
    KNX_UM_WARNF("[KNX-UM][WARN] Segment %d GA would exceed limits: %d/%d/%d (from %s + %d/%d/%d)\n",
                 segmentIndex, newMain, newMiddle, newSub, centralGA, segmentOffsetL, segmentOffsetM, segmentOffsetN);
    return 0;
  }

  return knxMakeGroupAddress((uint8_t)newMain, (uint8_t)newMiddle, (uint8_t)newSub);
}

std::vector<uint16_t> KnxIpUsermod::getAllUsedGAs() const {
  std::vector<uint16_t> usedGAs;

  // Helper to add GA if valid
  auto addGA = [&usedGAs](const char* gaStr) {
    if (gaStr && *gaStr) {
      uint16_t ga = parseGA(gaStr);
      if (ga > 0) usedGAs.push_back(ga);
    }
  };

  // Essential central input GAs
  addGA(gaInPower);
  addGA(gaInBri);
  addGA(gaInFx);
  addGA(gaInR);
  addGA(gaInG);
  addGA(gaInB);
  addGA(gaInW);
  addGA(gaInH);
  addGA(gaInS);
  addGA(gaInV);
  addGA(gaInPreset);
  addGA(gaInRGB);
  addGA(gaInHSV);
  addGA(gaInRGBW);

  // Essential central output GAs
  addGA(gaOutPower);
  addGA(gaOutBri);
  addGA(gaOutFx);
  addGA(gaOutR);
  addGA(gaOutG);
  addGA(gaOutB);
  addGA(gaOutW);
  addGA(gaOutPreset);
  addGA(gaOutRGB);
  addGA(gaOutHSV);
  addGA(gaOutRGBW);

  // Currently registered segment GAs
  if (GA_SEG_IN_PWR) {
    for (uint8_t i = 0; i < numSegments; i++) {
      if (GA_SEG_IN_PWR[i] > 0) usedGAs.push_back(GA_SEG_IN_PWR[i]);
    }
  }
  if (GA_SEG_IN_BRI) {
    for (uint8_t i = 0; i < numSegments; i++) {
      if (GA_SEG_IN_BRI[i] > 0) usedGAs.push_back(GA_SEG_IN_BRI[i]);
    }
  }
  if (GA_SEG_IN_FX) {
    for (uint8_t i = 0; i < numSegments; i++) {
      if (GA_SEG_IN_FX[i] > 0) usedGAs.push_back(GA_SEG_IN_FX[i]);
    }
  }
  if (GA_SEG_OUT_PWR) {
    for (uint8_t i = 0; i < numSegments; i++) {
      if (GA_SEG_OUT_PWR[i] > 0) usedGAs.push_back(GA_SEG_OUT_PWR[i]);
    }
  }
  if (GA_SEG_OUT_BRI) {
    for (uint8_t i = 0; i < numSegments; i++) {
      if (GA_SEG_OUT_BRI[i] > 0) usedGAs.push_back(GA_SEG_OUT_BRI[i]);
    }
  }
  if (GA_SEG_OUT_FX) {
    for (uint8_t i = 0; i < numSegments; i++) {
      if (GA_SEG_OUT_FX[i] > 0) usedGAs.push_back(GA_SEG_OUT_FX[i]);
    }
  }

  return usedGAs;
}

bool KnxIpUsermod::isGAInUse(uint16_t ga) const {
  if (ga == 0) return false;

  auto usedGAs = getAllUsedGAs();
  return std::find(usedGAs.begin(), usedGAs.end(), ga) != usedGAs.end();
}

bool KnxIpUsermod::hasGAConflicts(uint8_t maxSegments) const {
  if (maxSegments == 0) {
    maxSegments = strip.getSegmentsNum();
    if (maxSegments > 32) maxSegments = 32;
  }

  // Clear previous conflict details
  extern char errorDetails[256];
  errorDetails[0] = '\0';

  // Special case: single segment with zero offsets is valid (segment 0 = central)
  if (maxSegments == 1 && segmentOffsetL == 0 && segmentOffsetM == 0 && segmentOffsetN == 0) {
    KNX_UM_DEBUGF("[KNX-UM] Single segment with zero offsets - no conflicts (segment 0 = central)\n");
    return false;
  }

  std::vector<uint16_t> allGAs = getAllUsedGAs();
  std::vector<uint16_t> potentialSegmentGAs;
  std::vector<uint16_t> centralGAs_parsed;
  std::vector<std::string> conflictList; // Store conflicts for error message

  // Parse central GAs for comparison
  const char* centralGAs[] = {gaInPower, gaInBri, gaInFx, gaOutPower, gaOutBri, gaOutFx};
  const int numCentralGAs = sizeof(centralGAs) / sizeof(centralGAs[0]);

  for (int i = 0; i < numCentralGAs; i++) {
    uint16_t centralGA = parseGA(centralGAs[i]);
    if (centralGA > 0) {
      centralGAs_parsed.push_back(centralGA);
    }
  }

  // Generate all potential segment GAs
  for (uint8_t seg = 0; seg < maxSegments; seg++) {
    for (int i = 0; i < numCentralGAs; i++) {
      uint16_t segmentGA = calculateSegmentGA(centralGAs[i], seg);
      if (segmentGA > 0) {
        potentialSegmentGAs.push_back(segmentGA);

        // For segment 0, check if it matches central GA (which is expected and valid)
        if (seg == 0) {
          uint16_t centralGA = parseGA(centralGAs[i]);
          if (segmentGA == centralGA) {
            // This is expected for segment 0 - not a conflict
            continue;
          }
        }
      }
    }
  }

  bool hasConflicts = false;

  // Check for conflicts between existing GAs and potential segment GAs
  // (excluding the valid segment 0 = central GA case)
  for (uint16_t segGA : potentialSegmentGAs) {
    if (std::find(allGAs.begin(), allGAs.end(), segGA) != allGAs.end()) {
      // Check if this is a valid segment 0 = central GA case
      bool isValidSegment0Match = false;
      if (std::find(centralGAs_parsed.begin(), centralGAs_parsed.end(), segGA) != centralGAs_parsed.end()) {
        // This segment GA matches a central GA - check if it's from segment 0
        for (int i = 0; i < numCentralGAs; i++) {
          uint16_t seg0GA = calculateSegmentGA(centralGAs[i], 0);
          if (seg0GA == segGA) {
            isValidSegment0Match = true;
            break;
          }
        }
      }

      if (!isValidSegment0Match) {
        KNX_UM_WARNF("[KNX-UM][CONFLICT] Segment GA %d/%d/%d (0x%04X) conflicts with existing GA\n",
                     (segGA >> 11) & 0x1F, (segGA >> 8) & 0x07, segGA & 0xFF, segGA);

        // Add to conflict list for error message
        char conflictStr[32];
        snprintf(conflictStr, sizeof(conflictStr), "%d/%d/%d",
                 (segGA >> 11) & 0x1F, (segGA >> 8) & 0x07, segGA & 0xFF);
        conflictList.push_back(std::string(conflictStr));
        hasConflicts = true;
      }
    }
  }

  // Check for conflicts within segment GAs themselves (duplicates)
  std::sort(potentialSegmentGAs.begin(), potentialSegmentGAs.end());
  for (size_t i = 1; i < potentialSegmentGAs.size(); i++) {
    if (potentialSegmentGAs[i] == potentialSegmentGAs[i - 1]) {
      uint16_t conflictGA = potentialSegmentGAs[i];
      KNX_UM_WARNF("[KNX-UM][CONFLICT] Duplicate segment GA %d/%d/%d (0x%04X)\n",
                   (conflictGA >> 11) & 0x1F, (conflictGA >> 8) & 0x07, conflictGA & 0xFF, conflictGA);

      // Add to conflict list for error message
      char conflictStr[32];
      snprintf(conflictStr, sizeof(conflictStr), "%d/%d/%d",
               (conflictGA >> 11) & 0x1F, (conflictGA >> 8) & 0x07, conflictGA & 0xFF);

      // Avoid duplicates in conflict list
      bool alreadyListed = false;
      for (const auto& existing : conflictList) {
        if (existing == conflictStr) {
          alreadyListed = true;
          break;
        }
      }
      if (!alreadyListed) {
        conflictList.push_back(std::string(conflictStr));
      }
      hasConflicts = true;
    }
  }

  // Build error message with conflict details
  if (hasConflicts) {
    const size_t errorDetailsSize = 256; // Match the size defined in wled.h
    int pos = snprintf(errorDetails, errorDetailsSize,
                       "KNX GA conflicts: ");

    for (size_t i = 0; i < conflictList.size() && pos < (int)errorDetailsSize - 20; i++) {
      if (i > 0) {
        pos += snprintf(errorDetails + pos, errorDetailsSize - pos, ", ");
      }
      pos += snprintf(errorDetails + pos, errorDetailsSize - pos,
                      "%s", conflictList[i].c_str());
    }

    // Add suggestion to error message
    if (pos < (int)errorDetailsSize - 50) {
      snprintf(errorDetails + pos, errorDetailsSize - pos,
               ". Check segment offsets.");
    }

    // Debug: show what we put in the error details
    KNX_UM_DEBUGF("[KNX-UM] Error details set: '%s'\n", errorDetails);
  }

  return hasConflicts;
}

bool KnxIpUsermod::validateSegmentGAs() const {
  KNX_UM_DEBUGF("[KNX-UM] Validating segment GAs for conflicts...\n");

  uint8_t segmentCount = strip.getSegmentsNum();
  if (segmentCount > 32) segmentCount = 32;

  if (segmentCount == 0) {
    KNX_UM_DEBUGF("[KNX-UM] No segments to validate\n");
    return true;
  }

  // Check for conflicts
  bool hasConflicts = hasGAConflicts(segmentCount);

  if (hasConflicts) {
    KNX_UM_WARNF("[KNX-UM][WARN] GA conflicts detected! Segment registration may fail or cause unexpected behavior.\n");
    KNX_UM_WARNF("[KNX-UM][WARN] Consider adjusting segment offsets or central GAs to avoid conflicts.\n");

    // Set WLED GUI error flag to notify user
    extern byte errorFlag;
    errorFlag = 33; // ERR_KNX_GA_CONFLICT

    return false;
  }

  KNX_UM_DEBUGF("[KNX-UM] Segment GA validation passed - no conflicts detected\n");
  return true;
}

void KnxIpUsermod::analyzeGAConflicts() const {
  KNX_UM_DEBUGF("[KNX-UM] ==========================================\n");
  KNX_UM_DEBUGF("[KNX-UM] GA Conflict Analysis\n");
  KNX_UM_DEBUGF("[KNX-UM] ==========================================\n");

  uint8_t segmentCount = strip.getSegmentsNum();
  if (segmentCount > 32) segmentCount = 32;

  KNX_UM_DEBUGF("[KNX-UM] Current configuration:\n");
  KNX_UM_DEBUGF("[KNX-UM] - Segments: %d\n", segmentCount);
  KNX_UM_DEBUGF("[KNX-UM] - Offsets: L=%d, M=%d, N=%d\n", segmentOffsetL, segmentOffsetM, segmentOffsetN);

  // Special case: single segment with zero offsets
  if (segmentCount == 1 && segmentOffsetL == 0 && segmentOffsetM == 0 && segmentOffsetN == 0) {
    KNX_UM_DEBUGF("[KNX-UM] ✓ Single segment configuration: Segment 0 uses central GAs (VALID)\n");
    KNX_UM_DEBUGF("[KNX-UM] ==========================================\n");
    return;
  }

  // Show central GAs
  KNX_UM_DEBUGF("[KNX-UM] Central GAs:\n");
  KNX_UM_DEBUGF("[KNX-UM] - Power IN: %s, OUT: %s\n", gaInPower, gaOutPower);
  KNX_UM_DEBUGF("[KNX-UM] - Brightness IN: %s, OUT: %s\n", gaInBri, gaOutBri);
  KNX_UM_DEBUGF("[KNX-UM] - Effect IN: %s, OUT: %s\n", gaInFx, gaOutFx);

  // Show calculated segment GAs and conflicts
  KNX_UM_DEBUGF("[KNX-UM] Calculated segment GAs:\n");
  std::vector<uint16_t> allUsed = getAllUsedGAs();
  std::vector<uint16_t> centralGAs_parsed;

  // Parse central GAs for comparison
  const char* centralGAs[] = {gaInPower, gaInBri, gaInFx, gaOutPower, gaOutBri, gaOutFx};
  const int numCentralGAs = sizeof(centralGAs) / sizeof(centralGAs[0]);

  for (int i = 0; i < numCentralGAs; i++) {
    uint16_t centralGA = parseGA(centralGAs[i]);
    if (centralGA > 0) {
      centralGAs_parsed.push_back(centralGA);
    }
  }

  for (uint8_t seg = 0; seg < std::min((uint8_t)segmentCount, (uint8_t)5); seg++) { // Show first 5 segments
    uint16_t powerIn = calculateSegmentGA(gaInPower, seg);
    uint16_t powerOut = calculateSegmentGA(gaOutPower, seg);
    uint16_t briIn = calculateSegmentGA(gaInBri, seg);
    uint16_t briOut = calculateSegmentGA(gaOutBri, seg);
    uint16_t fxIn = calculateSegmentGA(gaInFx, seg);
    uint16_t fxOut = calculateSegmentGA(gaOutFx, seg);

    KNX_UM_DEBUGF("[KNX-UM] Segment %d:\n", seg);

    auto checkConflict = [&](uint16_t ga, const char* type) {
      if (ga > 0) {
        uint8_t main = (ga >> 11) & 0x1F;
        uint8_t middle = (ga >> 8) & 0x07;
        uint8_t sub = ga & 0xFF;

        bool conflict = false;
        bool isValidSegment0Match = false;

        // Check if this GA is already in use
        if (std::find(allUsed.begin(), allUsed.end(), ga) != allUsed.end()) {
          // For segment 0, check if matching central GA is valid
          if (seg == 0 && std::find(centralGAs_parsed.begin(), centralGAs_parsed.end(), ga) != centralGAs_parsed.end()) {
            isValidSegment0Match = true;
          } else {
            conflict = true;
          }
        }

        if (isValidSegment0Match) {
          KNX_UM_DEBUGF("[KNX-UM]   %s: %d/%d/%d ✓ (matches central)\n", type, main, middle, sub);
        } else if (conflict) {
          KNX_UM_DEBUGF("[KNX-UM]   %s: %d/%d/%d ⚠ CONFLICT\n", type, main, middle, sub);
        } else {
          KNX_UM_DEBUGF("[KNX-UM]   %s: %d/%d/%d ✓\n", type, main, middle, sub);
        }
      } else {
        KNX_UM_DEBUGF("[KNX-UM]   %s: INVALID (exceeds limits)\n", type);
      }
    };

    checkConflict(powerIn, "Power IN ");
    checkConflict(powerOut, "Power OUT");
    checkConflict(briIn, "Bri IN   ");
    checkConflict(briOut, "Bri OUT  ");
    checkConflict(fxIn, "FX IN    ");
    checkConflict(fxOut, "FX OUT   ");
  }

  if (segmentCount > 5) {
    KNX_UM_DEBUGF("[KNX-UM] ... (%d more segments)\n", segmentCount - 5);
  }

  // Provide suggestions
  KNX_UM_DEBUGF("[KNX-UM] ==========================================\n");
  KNX_UM_DEBUGF("[KNX-UM] Suggestions to avoid conflicts:\n");
  KNX_UM_DEBUGF("[KNX-UM] 1. Increase segment offset values (L/M/N)\n");
  KNX_UM_DEBUGF("[KNX-UM] 2. Use different central GA ranges\n");
  KNX_UM_DEBUGF("[KNX-UM] 3. Reduce number of segments\n");
  KNX_UM_DEBUGF("[KNX-UM] 4. Use reserved GA ranges for segments\n");
  KNX_UM_DEBUGF("[KNX-UM] ==========================================\n");
}

/**
 * Test the GA conflict detection system
 */
void KnxIpUsermod::testGAConflictDetection() {
  KNX_UM_DEBUGF("[KNX-TEST] Testing GA conflict detection system...\n");

  // Save original configuration
  uint8_t origL = segmentOffsetL;
  uint8_t origM = segmentOffsetM;
  uint8_t origN = segmentOffsetN;
  char origPowerIn[16], origBriIn[16], origFxIn[16];
  strlcpy(origPowerIn, gaInPower, sizeof(origPowerIn));
  strlcpy(origBriIn, gaInBri, sizeof(origBriIn));
  strlcpy(origFxIn, gaInFx, sizeof(origFxIn));

  // Test 1: Valid configuration (no conflicts)
  KNX_UM_DEBUGF("[KNX-TEST] Test 1: Valid configuration\n");
  segmentOffsetL = 10; // Large offset to avoid conflicts
  segmentOffsetM = 0;
  segmentOffsetN = 50;
  strlcpy(gaInPower, "1/1/1", sizeof(gaInPower));
  strlcpy(gaInBri, "1/1/10", sizeof(gaInBri));
  strlcpy(gaInFx, "1/1/20", sizeof(gaInFx));

  if (!hasGAConflicts(3)) {
    KNX_UM_DEBUGF("[KNX-TEST] ✓ No conflicts detected with valid offsets\n");
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] ✗ Unexpected conflicts with valid offsets\n");
  }

  // Test 2: Conflicting configuration (segment 0 = central)
  KNX_UM_DEBUGF("[KNX-TEST] Test 2: Zero offsets (segment 0 = central)\n");
  segmentOffsetL = 0;
  segmentOffsetM = 0;
  segmentOffsetN = 0;

  if (hasGAConflicts(2)) {
    KNX_UM_DEBUGF("[KNX-TEST] ✓ Conflicts detected with zero offsets\n");
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] ✗ Should have detected conflicts with zero offsets\n");
  }

  // Test 3: Cross-segment conflicts
  KNX_UM_DEBUGF("[KNX-TEST] Test 3: Cross-segment conflicts\n");
  segmentOffsetL = 1;
  segmentOffsetM = 0;
  segmentOffsetN = 0;
  strlcpy(gaInPower, "1/2/10", sizeof(gaInPower));
  strlcpy(gaInBri, "2/2/10", sizeof(gaInBri)); // Segment 1 power will be 2/2/10 (conflicts with central brightness)

  if (hasGAConflicts(2)) {
    KNX_UM_DEBUGF("[KNX-TEST] ✓ Cross-segment conflicts detected\n");
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] ✗ Should have detected cross-segment conflicts\n");
  }

  // Test 4: Individual GA usage check
  KNX_UM_DEBUGF("[KNX-TEST] Test 4: Individual GA usage check\n");
  uint16_t testGA = parseGA("1/2/10");
  if (isGAInUse(testGA)) {
    KNX_UM_DEBUGF("[KNX-TEST] ✓ GA 1/2/10 correctly detected as in use\n");
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] ✗ GA 1/2/10 should be detected as in use\n");
  }

  uint16_t unusedGA = parseGA("7/7/7");
  if (!isGAInUse(unusedGA)) {
    KNX_UM_DEBUGF("[KNX-TEST] ✓ GA 7/7/7 correctly detected as unused\n");
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] ✗ GA 7/7/7 should be detected as unused\n");
  }

  // Show detailed analysis
  KNX_UM_DEBUGF("[KNX-TEST] Detailed conflict analysis:\n");
  analyzeGAConflicts();

  // Restore original configuration
  segmentOffsetL = origL;
  segmentOffsetM = origM;
  segmentOffsetN = origN;
  strlcpy(gaInPower, origPowerIn, sizeof(gaInPower));
  strlcpy(gaInBri, origBriIn, sizeof(gaInBri));
  strlcpy(gaInFx, origFxIn, sizeof(gaInFx));

  KNX_UM_DEBUGF("[KNX-TEST] GA conflict detection test completed\n");
}

/**
 * Test validation integration with registration
 */
void KnxIpUsermod::testValidationIntegration() {
  KNX_UM_DEBUGF("[KNX-TEST] Testing validation integration...\n");

  // Save current state
  uint8_t origSegments = numSegments;
  uint16_t* origPWR = GA_SEG_IN_PWR;
  uint16_t* origBRI = GA_SEG_IN_BRI;
  uint16_t* origFX = GA_SEG_IN_FX;
  uint8_t origL = segmentOffsetL;

  // Clear arrays to test clean state
  GA_SEG_IN_PWR = nullptr;
  GA_SEG_IN_BRI = nullptr;
  GA_SEG_IN_FX = nullptr;
  numSegments = 0;

  // Test 1: Registration should fail with conflicts
  KNX_UM_DEBUGF("[KNX-TEST] Test 1: Registration with conflicts\n");
  segmentOffsetL = 0; // Zero offset will cause conflicts

  // Clear any existing registrations first
  clearSegmentKOs();

  // Try to register - should fail due to conflicts
  registerSegmentKOs();

  if (GA_SEG_IN_PWR == nullptr && numSegments == 0) {
    KNX_UM_DEBUGF("[KNX-TEST] ✓ Registration correctly failed due to conflicts\n");
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] ✗ Registration should have failed\n");
  }

  // Test 2: Registration should succeed without conflicts
  KNX_UM_DEBUGF("[KNX-TEST] Test 2: Registration without conflicts\n");
  segmentOffsetL = 10; // Large offset to avoid conflicts

  registerSegmentKOs();

  if (strip.getSegmentsNum() > 0) {
    if (GA_SEG_IN_PWR != nullptr && numSegments > 0) {
      KNX_UM_DEBUGF("[KNX-TEST] ✓ Registration succeeded without conflicts\n");
    } else {
      KNX_UM_DEBUGF("[KNX-TEST] ✗ Registration should have succeeded\n");
    }
  } else {
    KNX_UM_DEBUGF("[KNX-TEST] - No segments available for registration test\n");
  }

  // Clean up
  clearSegmentKOs();

  // Restore original state
  numSegments = origSegments;
  GA_SEG_IN_PWR = origPWR;
  GA_SEG_IN_BRI = origBRI;
  GA_SEG_IN_FX = origFX;
  segmentOffsetL = origL;

  KNX_UM_DEBUGF("[KNX-TEST] Validation integration test completed\n");
}

/**
 * Check for GA conflicts and set GUI error message if found
 * This can be called periodically or when configuration changes
 */
void KnxIpUsermod::checkGAConflictsAndNotifyGUI() {
  if (hasGAConflicts()) {
    // Set WLED GUI error flag
    extern byte errorFlag;
    errorFlag = 33; // ERR_KNX_GA_CONFLICT

    KNX_UM_WARNF("[KNX-UM][CONFLICT] GA conflicts detected - check WLED GUI for notification\n");

// Log detailed conflict information for debugging
#ifdef KNX_UM_DEBUG
    analyzeGAConflicts();
#endif
  } else {
    // Clear error flag if no conflicts (only if it was our error)
    extern byte errorFlag;
    if (errorFlag == 33) {
      errorFlag = 0; // ERR_NONE
    }
  }
}

/**
 * Run all GA conflict tests - can be called from setup() or externally
 */
void KnxIpUsermod::runGAConflictTests() {
  KNX_UM_DEBUGF("[KNX-TEST] ==========================================\n");
  KNX_UM_DEBUGF("[KNX-TEST] Starting GA Conflict Detection Tests\n");
  KNX_UM_DEBUGF("[KNX-TEST] ==========================================\n");

  testGAConflictDetection();
  testValidationIntegration();

  KNX_UM_DEBUGF("[KNX-TEST] ==========================================\n");
  KNX_UM_DEBUGF("[KNX-TEST] GA Conflict Detection Tests Completed\n");
  KNX_UM_DEBUGF("[KNX-TEST] ==========================================\n");
}

void KnxIpUsermod::clearSegmentKOs() {
  // Free existing arrays (only the main controls)
  delete[] GA_SEG_IN_PWR;
  GA_SEG_IN_PWR = nullptr;
  delete[] GA_SEG_IN_BRI;
  GA_SEG_IN_BRI = nullptr;
  delete[] GA_SEG_IN_FX;
  GA_SEG_IN_FX = nullptr;

  delete[] GA_SEG_OUT_PWR;
  GA_SEG_OUT_PWR = nullptr;
  delete[] GA_SEG_OUT_BRI;
  GA_SEG_OUT_BRI = nullptr;
  delete[] GA_SEG_OUT_FX;
  GA_SEG_OUT_FX = nullptr;

  numSegments = 0;
}

void KnxIpUsermod::registerSegmentKOs() {
  // reset any previous allocations
  clearSegmentKOs();

  // how many WLED segments?
  numSegments = strip.getSegmentsNum();
  if (numSegments > 32) {
    KNX_UM_WARNF("[KNX-UM][WARN] Too many segments (%d), limiting to 32\n", strip.getSegmentsNum());
    numSegments = 32;
  }
  if (numSegments == 0) {
    KNX_UM_DEBUGF("[KNX-UM] No segments found, skipping per-segment KO registration\n");
    return;
  }

  // Validate GA plan before registering anything
  if (!validateSegmentGAs()) {
    KNX_UM_WARNF("[KNX-UM][ERROR] GA conflicts detected! Skipping segment KO registration to prevent issues.\n");
    KNX_UM_WARNF("[KNX-UM][ERROR] Please adjust segment offsets (L=%d, M=%d, N=%d) or central GAs.\n",
                 segmentOffsetL, segmentOffsetM, segmentOffsetN);
    extern byte errorFlag;
    if (errorFlag == 0) errorFlag = 33; // ERR_KNX_GA_CONFLICT
    return;
  }

  KNX_UM_DEBUGF("[KNX-UM] Registering per-segment KOs for %d segments\n", numSegments);

  // Allocate arrays (main controls only)
  GA_SEG_IN_PWR = new uint16_t[numSegments]();
  GA_SEG_IN_BRI = new uint16_t[numSegments]();
  GA_SEG_IN_FX = new uint16_t[numSegments]();

  GA_SEG_OUT_PWR = new uint16_t[numSegments]();
  GA_SEG_OUT_BRI = new uint16_t[numSegments]();
  GA_SEG_OUT_FX = new uint16_t[numSegments]();

  // Parse central GAs once so we can avoid duplicate seg0 registration
  const uint16_t centralInPwr = parseGA(gaInPower);
  const uint16_t centralInBri = parseGA(gaInBri);
  const uint16_t centralInFx = parseGA(gaInFx);
  const uint16_t centralOutPwr = parseGA(gaOutPower);
  const uint16_t centralOutBri = parseGA(gaOutBri);
  const uint16_t centralOutFx = parseGA(gaOutFx);

  // For each segment, calculate and (conditionally) register
  for (uint8_t seg = 0; seg < numSegments; seg++) {
    // Calculate per-segment addresses from central + offsets
    GA_SEG_IN_PWR[seg] = calculateSegmentGA(gaInPower, seg);
    GA_SEG_IN_BRI[seg] = calculateSegmentGA(gaInBri, seg);
    GA_SEG_IN_FX[seg] = calculateSegmentGA(gaInFx, seg);

    GA_SEG_OUT_PWR[seg] = calculateSegmentGA(gaOutPower, seg);
    GA_SEG_OUT_BRI[seg] = calculateSegmentGA(gaOutBri, seg);
    GA_SEG_OUT_FX[seg] = calculateSegmentGA(gaOutFx, seg);

    // ===== IN: register only if not duplicating the central GA on seg0 =====
    if (GA_SEG_IN_PWR[seg] &&
        !(seg == 0 && GA_SEG_IN_PWR[seg] == centralInPwr)) {
      KNX.addGroupObject(GA_SEG_IN_PWR[seg], DptMain::DPT_1xx, false, true);
      KNX.onGroup(GA_SEG_IN_PWR[seg],
                  [this, seg](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
                    if (svc == KnxService::GroupValue_Write && p && len >= 1) this->onKnxSegmentPower(seg, p[0] & 1);
                  });
    }

    if (GA_SEG_IN_BRI[seg] &&
        !(seg == 0 && GA_SEG_IN_BRI[seg] == centralInBri)) {
      KNX.addGroupObject(GA_SEG_IN_BRI[seg], DptMain::DPT_5xx, false, true);
      KNX.onGroup(GA_SEG_IN_BRI[seg],
                  [this, seg](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
                    if (svc == KnxService::GroupValue_Write && p && len >= 1) this->onKnxSegmentBrightness(seg, KnxIpCore::unpackScaling(p, len));
                  });
    }

    if (GA_SEG_IN_FX[seg] &&
        !(seg == 0 && GA_SEG_IN_FX[seg] == centralInFx)) {
      KNX.addGroupObject(GA_SEG_IN_FX[seg], DptMain::DPT_5xx, false, true);
      KNX.onGroup(GA_SEG_IN_FX[seg],
                  [this, seg](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
                    if (svc == KnxService::GroupValue_Write && p && len >= 1) this->onKnxSegmentEffect(seg, p[0]);
                  });
    }

    // ===== OUT: same skip rule for status objects =====
    if (GA_SEG_OUT_PWR[seg] &&
        !(seg == 0 && GA_SEG_OUT_PWR[seg] == centralOutPwr)) {
      KNX.addGroupObject(GA_SEG_OUT_PWR[seg], DptMain::DPT_1xx, true, false);
    }
    if (GA_SEG_OUT_BRI[seg] &&
        !(seg == 0 && GA_SEG_OUT_BRI[seg] == centralOutBri)) {
      KNX.addGroupObject(GA_SEG_OUT_BRI[seg], DptMain::DPT_5xx, true, false);
    }
    if (GA_SEG_OUT_FX[seg] &&
        !(seg == 0 && GA_SEG_OUT_FX[seg] == centralOutFx)) {
      KNX.addGroupObject(GA_SEG_OUT_FX[seg], DptMain::DPT_5xx, true, false);
    }
  }

  KNX_UM_DEBUGF("[KNX-UM] Per-segment KO registration complete\n");
}

#ifdef UNIT_TEST
// Expose internal helpers for unit tests (pure logic only)
extern "C" {
uint16_t knx_test_parseGA(const char* s) {
  return parseGA(s);
}
uint16_t knx_test_parsePA(const char* s) {
  return parsePA(s);
}
uint8_t knx_test_step_pct(uint8_t sc) {
  return knx_step_pct(sc);
}
int16_t knx_test_step_delta(uint8_t nibble, uint16_t maxVal) {
  return knx_step_delta(nibble, maxVal);
}
void knx_test_rgbToHsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v) {
  KnxIpUsermod::rgbToHsv(r, g, b, h, s, v);
}
void knx_test_hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  KnxIpUsermod::hsvToRgb(h, s, v, r, g, b);
}
uint8_t knx_test_clamp100(uint8_t v) {
  return clamp100(v);
}
uint8_t knx_test_pct_to_0_255(uint8_t pct) {
  return pct_to_0_255(pct);
}
uint8_t knx_test_to_pct_0_100(uint8_t v0_255) {
  return to_pct_0_100(v0_255);
}
// CCT conversion helpers using default ranges (2700-6500K)
uint8_t knx_test_kelvin_to_cct255(uint16_t k) {
  const uint16_t kmin = 2700, kmax = 6500;
  if (k <= kmin) return 0;
  if (k >= kmax) return 255;
  uint32_t span = (uint32_t)kmax - (uint32_t)kmin;
  uint32_t pos = (uint32_t)k - (uint32_t)kmin;
  return (uint8_t)((pos * 255u + (span / 2)) / span);
}
uint16_t knx_test_cct255_to_kelvin(uint8_t cct) {
  const uint16_t kmin = 2700, kmax = 6500;
  uint32_t span = (uint32_t)kmax - (uint32_t)kmin;
  return (uint16_t)(kmin + (uint32_t)cct * span / 255u);
}
}
#endif

// Per-segment KO handlers
void KnxIpUsermod::onKnxSegmentPower(uint8_t segmentIndex, bool on) {
  if (segmentIndex >= strip.getSegmentsNum()) return;

  Segment& segment = strip.getSegment(segmentIndex);
  segment.on = on;

  KNX_UM_DEBUGF("[KNX-UM] Segment %d power: %s\n", segmentIndex, on ? "ON" : "OFF");
  stateUpdated(CALL_MODE_DIRECT_CHANGE, segmentIndex);
  // Note: Don't call scheduleStatePublish() to avoid feedback loops
}

void KnxIpUsermod::onKnxSegmentBrightness(uint8_t segmentIndex, uint8_t pct) {
  if (segmentIndex >= strip.getSegmentsNum()) return;

  pct = clamp100(pct);
  uint8_t bri255 = pct_to_0_255(pct);

  Segment& segment = strip.getSegment(segmentIndex);
  segment.opacity = bri255;
  if (bri255 > 0) segment.on = true;

  KNX_UM_DEBUGF("[KNX-UM] Segment %d brightness: %d%% (%d/255)\n", segmentIndex, pct, bri255);
  stateUpdated(CALL_MODE_DIRECT_CHANGE, segmentIndex);
}

void KnxIpUsermod::onKnxSegmentRGB(uint8_t segmentIndex, uint8_t r, uint8_t g, uint8_t b) {
  if (segmentIndex >= strip.getSegmentsNum()) return;

  Segment& segment = strip.getSegment(segmentIndex);
  uint32_t currentCol = segment.colors[0];
  uint8_t w = W(currentCol); // Preserve white component

  segment.setColor(0, RGBW32(r, g, b, w));

  KNX_UM_DEBUGF("[KNX-UM] Segment %d RGB: R=%d G=%d B=%d\n", segmentIndex, r, g, b);
  stateUpdated(CALL_MODE_DIRECT_CHANGE, segmentIndex);
}

void KnxIpUsermod::onKnxSegmentEffect(uint8_t segmentIndex, uint8_t fxIndex) {
  if (segmentIndex >= strip.getSegmentsNum()) return;

  Segment& segment = strip.getSegment(segmentIndex);
  segment.mode = fxIndex;

  KNX_UM_DEBUGF("[KNX-UM] Segment %d effect: %d\n", segmentIndex, fxIndex);
  stateUpdated(CALL_MODE_DIRECT_CHANGE, segmentIndex);
}

bool KnxIpUsermod::readEspInternalTempC(float& outC) const {
#if defined(ESP8266) || defined(CONFIG_IDF_TARGET_ESP32S2)
  KNX_UM_DEBUGF("ESP-int: not supported on this chip\n");
  return false;
#else
  // ESP32 / ESP32S3 / ESP32C3: direct internal sensor read
  float v = temperatureRead();                // degrees C (approximate)
  if (isnan(v) || v < -40.0f || v > 150.0f) { // quick sanity
    KNX_UM_DEBUGF("ESP-internal Temp: invalid (%.1f)\n", v);
    return false;
  }
  // match the other usermod’s rounding (0.1 °C)
  v = roundf(v * 10.0f) / 10.0f;
  KNX_UM_DEBUGF("ESP-internal Temp: OK (%.1f °C)\n", v);
  outC = v;
  return true;
#endif
}

bool KnxIpUsermod::readDallasTempC(float& outC) const {
  if (&wled_get_temperature_c != nullptr) {
    float v = wled_get_temperature_c();
    if (!isnan(v)) {
      KNX_UM_DEBUGF("Dallas Temp probe: OK (%.1f °C)\n", v);
      outC = v;
      return true;
    }
    KNX_UM_DEBUGF("Dallas Temp probe: NaN\n");
  } else {
    KNX_UM_DEBUGF("Dallas Temp probe: symbol missing\n");
  }
  return false;
}

void KnxIpUsermod::publishTemperatureIfChanged() {
  if (!KNX.running()) {
    return;
  }

  KNX_UM_DEBUGF("[KNX-UM][TEMP] publishTemperatureIfChanged() called\n");

  bool anyTempChanged = false;

  // Check ESP internal temperature
  if (GA_OUT_INT_TEMP) {
    float tEsp;
    if (readEspInternalTempC(tEsp)) {
      // Compare with last published value (with 0.1°C tolerance for floating point)
      if (_firstSchedule || fabs(tEsp - g_lastEspTemp) >= 0.05f) {
        uint8_t buf[4];
        KnxIpCore::pack4ByteFloat(tEsp, buf);
        bool ok = KNX.groupValueWrite(GA_OUT_INT_TEMP, buf, 4);
        KNX_UM_DEBUGF("TX ESP internal Temp: %.1f °C (%s)\n", tEsp, ok ? "OK" : "FAIL");
        evalAndPublishTempAlarm(GA_OUT_INT_TEMP_ALARM, tEsp, intTempAlarmMaxC, lastIntTempAlarmState, "ESP-int");
        g_lastEspTemp = tEsp;
        anyTempChanged = true;
      }
    }
  }

  // Check Dallas temperature
  if (GA_OUT_TEMP) {
    float tDallas;
    if (readDallasTempC(tDallas)) {
      // Compare with last published value (with 0.1°C tolerance for floating point)
      if (_firstSchedule || fabs(tDallas - g_lastDallasTemp) >= 0.05f) {
        uint8_t buf[4];
        KnxIpCore::pack4ByteFloat(tDallas, buf);
        bool ok = KNX.groupValueWrite(GA_OUT_TEMP, buf, 4);
        KNX_UM_DEBUGF("TX Dallas Temp: %.1f °C (%s)\n", tDallas, ok ? "OK" : "FAIL");
        evalAndPublishTempAlarm(GA_OUT_TEMP_ALARM, tDallas, dallasTempAlarmMaxC, lastDallasTempAlarmState, "Dallas");
        g_lastDallasTemp = tDallas;
        anyTempChanged = true;
      }
    }
  }

  if (!anyTempChanged && !_firstSchedule) {
    KNX_UM_DEBUGF("[KNX-UM][TEMP] No temperature changes detected - skipping publish\n");
  }
}

void KnxIpUsermod::evalAndPublishTempAlarm(uint16_t ga, float tempC, float maxC, bool& lastState, const char* tag) {
  if (!ga) return; // not configured

  // Disabled threshold? Allow negative to mean "off"
  if (!(maxC > -100.0f)) {
    // Optional: clear if previously set
    if (lastState) {
      uint8_t b0 = 0;
      KNX.groupValueWrite(ga, &b0, 1);
      lastState = false;
      KNX_UM_DEBUGF("[KNX-UM][TEMP] %s alarm DISABLED -> send 0 to 0x%04X\n", tag, ga);
    }
    return;
  }

  bool trip = (tempC >= maxC);
  bool clear = (tempC <= (maxC - tempAlarmHystC));

  bool newState = lastState;
  if (!lastState && trip)
    newState = true; // 0 -> 1 (trip)
  else if (lastState && clear)
    newState = false; // 1 -> 0 (clear)

  if (newState != lastState) {
    uint8_t bit = newState ? 1 : 0;             // DPST-1-5: 1 = alarm active
    bool ok = KNX.groupValueWrite(ga, &bit, 1); // core packs 1-bit correctly
    KNX_UM_DEBUGF("[KNX-UM][TEMP] %s alarm %s @ %.2f°C (thr=%.2f°C, hyst=%.2f) -> GA 0x%04X (%s)\n",
                  tag, newState ? "ON" : "OFF", tempC, maxC, tempAlarmHystC, ga, ok ? "OK" : "FAIL");
    lastState = newState;
  } else {
    KNX_UM_DEBUGF("[KNX-UM][TEMP] %s alarm unchanged (%s) @ %.2f°C "
                  "(thr=%.2f°C, hyst=%.2f)\n",
                  tag, lastState ? "ON" : "OFF",
                  tempC, maxC, tempAlarmHystC);
  }
}

void KnxIpUsermod::setSystemClockYMDHMS(int year, int month, int day, int hour, int minute, int second) {
  // KNX 0..99 -> 2000..2099 (adjust if you prefer a different epoch)
  if (year < 100) year += 2000;

  struct tm t{};
  t.tm_year = year - 1900; // years since 1900
  t.tm_mon  = month - 1;   // 0..11
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = minute;
  t.tm_sec  = second;
  t.tm_isdst = -1;      // <— let mktime() figure out DST from the TZ rules

  #if defined(ESP32)
    stopSntpClient();
  #endif

  time_t epoch = mktime(&t); // local time
  if (epoch == (time_t)-1) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] mktime() failed for %04d-%02d-%02d %02d:%02d:%02d\n",
                  year, month, day, hour, minute, second);
    return;
  }
  // Set system clock (both POSIX ways)
  struct timeval tv{ .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  tzset();

#if defined(ESP32)
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
#endif
ntpLastSyncTime = (unsigned long)epoch;

KNX_UM_DEBUGF("[KNX-UM][TIME] Clock set to %04d-%02d-%02d %02d:%02d:%02d (local)\n",
                year, month, day, hour, minute, second);

time_t chk = time(nullptr);
struct tm cur{}; localtime_r(&chk, &cur);
KNX_UM_DEBUGF("[KNX-UM][TIME] Clock set -> %04d-%02d-%02d %02d:%02d:%02d (local, read-back)\n",
                cur.tm_year + 1900, cur.tm_mon + 1, cur.tm_mday,
                cur.tm_hour, cur.tm_min, cur.tm_sec);
}

void KnxIpUsermod::setSystemClockYMDHMS_withDST(int year, int month, int day, int hour, int minute, int second, int isDst /* -1 auto, 0 standard, 1 DST */){
  if (year < 100) year += 2000;

  struct tm t{};
  t.tm_year  = year - 1900;
  t.tm_mon   = month - 1;
  t.tm_mday  = day;
  t.tm_hour  = hour;
  t.tm_min   = minute;
  t.tm_sec   = second;
  t.tm_isdst = isDst;      // <— use the hint from DPT19 if provided

  #if defined(ESP32)
    stopSntpClient();
  #endif

  time_t epoch = mktime(&t);
  if (epoch == (time_t)-1) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] mktime() failed (DST=%d)\n", isDst);
    return;
  }
  struct timeval tv{ .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  tzset();

#if defined(ESP32)
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
#endif
  ntpLastSyncTime = (unsigned long)epoch;

  time_t chk = time(nullptr);
  struct tm cur{}; localtime_r(&chk, &cur);
  KNX_UM_DEBUGF("[KNX-UM][TIME] DPT19 set -> %04d-%02d-%02d %02d:%02d:%02d (local, read-back, DST=%d)\n",
                cur.tm_year + 1900, cur.tm_mon + 1, cur.tm_mday,
                cur.tm_hour, cur.tm_min, cur.tm_sec, cur.tm_isdst);
}

static inline void applyKnxWallClock(int year, int month, int day, int hour, int minute, int second, int isDst /* -1 auto, 0 std, 1 DST */, bool stopSntp /*true to prevent override*/) {
  if (year < 100) year += 2000;

  struct tm t{};
  t.tm_year  = year - 1900;
  t.tm_mon   = month - 1;
  t.tm_mday  = day;
  t.tm_hour  = hour;
  t.tm_min   = minute;
  t.tm_sec   = second;
  t.tm_isdst = isDst;   // for DPT19 use summerTime?1:0; for 10/11 use -1

  // Convert local wall time -> UTC epoch using WLED’s configured TZ/DST
  time_t epoch = mktime(&t);
  if (epoch == (time_t)-1) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] mktime() failed for %04d-%02d-%02d %02d:%02d:%02d (isDst=%d)\n",
                  year, month, day, hour, minute, second, isDst);
    return;
  }

#if defined(ESP32)
  if (stopSntp) stopSntpClient();   // optional: keep KNX authoritative
#endif

  // Hand off to WLED core (updates everything the Info page uses)
  setTimeFromAPI((uint32_t)epoch);

  // Debug: read back local time
  time_t chk = time(nullptr);
  struct tm cur{}; localtime_r(&chk, &cur);
  KNX_UM_DEBUGF("[KNX-UM][TIME] KNX->API set: %04d-%02d-%02d %02d:%02d:%02d (local, read-back)\n",
                cur.tm_year + 1900, cur.tm_mon + 1, cur.tm_mday,
                cur.tm_hour, cur.tm_min, cur.tm_sec);
}

static void dumpBytesHexLocal(const uint8_t* p, uint8_t len) {
  if (!p || !len) return;

#ifdef KNX_UM_DEBUG
  KNX_UM_DEBUG("[KNX-UM][TIME] Raw: ");
#endif

  for (uint8_t i = 0; i < len; i++) {
#ifdef KNX_UM_DEBUG
    if (p[i] < 16) KNX_UM_DEBUG('0');
    KNX_UM_DEBUG(p[i], HEX);
    KNX_UM_DEBUG(' ');
#endif
  }
  KNX_UM_DEBUGLN();
}

void KnxIpUsermod::onKnxTime_10_001(const uint8_t* p, uint8_t len) {
  if (!p || len < 3) return;
  dumpBytesHexLocal(p, len);

  // DPT 10 (Time of day) encoding:
  //  - byte0: bits 0..4 = hour (0..23), bit7 may indicate 'invalid'
  //  - byte1: bits 0..5 = minute (0..59)
  //  - byte2: bits 0..5 = second (0..59)
  const bool invalidFlag = (p[0] & 0x80) != 0;
  const int hour = p[0] & 0x1F;   // 5 bits
  const int minute = p[1] & 0x3F; // 6 bits
  const int second = p[2] & 0x3F; // 6 bits

  // Validate: respect KNX 'invalid' flag and sensible ranges
  if (invalidFlag) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] DPT10 invalid flag set (byte0=0x%02X) -> ignore\n", p[0]);
    return;
  }
  if (hour > 23 || minute > 59 || second > 59) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] DPT10 out-of-range values: %02d:%02d:%02d -> ignore\n", hour, minute, second);
    return;
  }

  // Get current date to merge with
  time_t nowUtc = time(nullptr);
  struct tm curLocal{};
  localtime_r(&nowUtc, &curLocal);

  struct tm t{};
  t.tm_year = year(localTime) - 1900; // keep current date
  t.tm_mon = month(localTime) - 1;
  t.tm_mday = day(localTime);
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_isdst = -1; // let libc apply TZ/DST rules

  time_t epoch = mktime(&t); // interpret as *local* wall clock -> UTC epoch
  if (epoch == (time_t)-1) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] DPT10 mktime() failed for %02d:%02d:%02d\n", hour, minute, second);
    return;
  }

#if defined(ESP32)
  stopSntpClient(); // keep KNX authoritative
#endif
  setTimeFromAPI((uint32_t)epoch);
#if defined(ESP32)
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
#endif

  struct tm rb{};
  localtime_r(&epoch, &rb);
  KNX_UM_DEBUGF("[KNX-UM][TIME] DPT10 set -> %04d-%02d-%02d %02d:%02d:%02d (local)\n",
                rb.tm_year + 1900, rb.tm_mon + 1, rb.tm_mday, rb.tm_hour, rb.tm_min, rb.tm_sec);
}

void KnxIpUsermod::onKnxDate_11_001(const uint8_t* p, uint8_t len) {
  if (!p || len < 3) return;
  dumpBytesHexLocal(p, len);

  const int day = p[0] & 0x1F; // 1..31, mask is defensive
  const int month = p[1];      // Preserve the full byte; mktime() normalizes the date.
  int year = p[2];             // 0..99
  year = (year < 100) ? (2000 + year) : year;

  // Keep current time-of-day
  time_t nowUtc = time(nullptr);
  struct tm curLocal{};
  localtime_r(&nowUtc, &curLocal);

  struct tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = (month - 1);
  t.tm_mday = day;
  t.tm_hour = curLocal.tm_hour;
  t.tm_min = curLocal.tm_min;
  t.tm_sec = curLocal.tm_sec;
  t.tm_isdst = -1; // let libc apply TZ/DST rules

  time_t epoch = mktime(&t); // local wall clock -> UTC epoch
  if (epoch == (time_t)-1) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] DPT11 mktime() failed for %04d-%02d-%02d\n", year, month, day);
    return;
  }

#if defined(ESP32)
  stopSntpClient();
#endif
  setTimeFromAPI((uint32_t)epoch);
#if defined(ESP32)
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
#endif

  struct tm rb{};
  localtime_r(&epoch, &rb);
  KNX_UM_DEBUGF("[KNX-UM][TIME] DPT11 set -> %04d-%02d-%02d %02d:%02d:%02d (local)\n",
                rb.tm_year + 1900, rb.tm_mon + 1, rb.tm_mday, rb.tm_hour, rb.tm_min, rb.tm_sec);
}

void KnxIpUsermod::onKnxDateTime_19_001(const uint8_t* p, uint8_t len) {
  if (!p || len < 8) return;
  dumpBytesHexLocal(p, len);

  const uint8_t year8 = p[0];
  const uint8_t month = p[1];
  const uint8_t day = p[2];
  /*const uint8_t wday = p[3];*/ // weekday not used for setting
  const uint8_t hourB = p[4];
  const uint8_t minB = p[5];
  const uint8_t secB = p[6];
  const uint8_t flags = p[7];

  const bool invalidDate = (flags & 0x08) != 0;
  const bool invalidTime = (flags & 0x04) != 0;
  const bool summerTime = (flags & 0x10) != 0;

  if (invalidDate || invalidTime) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] DPT19 invalid flags=0x%02X -> ignore\n", flags);
    return;
  }

  int year = year8 + 1900;        // offset 1900
  const int hour = hourB & 0x1F;  // 0..23
  const int minute = minB & 0x3F; // 0..59
  const int second = secB & 0x3F; // 0..59

  // Build local wall time. Use the DPT flag to steer DST explicitly.
  struct tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_isdst = summerTime ? 1 : 0; // explicit per DPT19

  time_t epoch = mktime(&t); // local wall clock -> UTC epoch
  if (epoch == (time_t)-1) {
    KNX_UM_DEBUGF("[KNX-UM][TIME] DPT19 mktime() failed for %04d-%02d-%02d %02d:%02d:%02d (DST=%d)\n",
                  year, month, day, hour, minute, second, (int)summerTime);
    return;
  }

#if defined(ESP32)
  stopSntpClient();
#endif
  setTimeFromAPI((uint32_t)epoch);
#if defined(ESP32)
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
#endif

  struct tm rb{};
  localtime_r(&epoch, &rb);
  KNX_UM_DEBUGF("[KNX-UM][TIME] DPT19 set -> %04d-%02d-%02d %02d:%02d:%02d (local, DST=%d, flags=0x%02X)\n",
                rb.tm_year + 1900, rb.tm_mon + 1, rb.tm_mday,
                rb.tm_hour, rb.tm_min, rb.tm_sec, (int)summerTime, flags);
}

/**
 * Generate HTML table showing all GA mappings for main group and segments
 */
String KnxIpUsermod::getGATableHTML() const {
  if (!enabled) {
    KNX_UM_DEBUGF("[KNX-UM] getGATableHTML: usermod not enabled\n");
    return "";
  }

  uint8_t segmentCount = strip.getSegmentsNum();
  if (segmentCount == 0) segmentCount = 1;
  uint32_t hash = computeGATableHash();
  if (gaTableCacheHash == hash && gaTableCacheSegments == segmentCount && gaTableCache.length() > 0) {
    KNX_UM_DEBUGF("[KNX-UM] getGATableHTML: using cached table (%d chars)\n", gaTableCache.length());
    return gaTableCache;
  }
  KNX_UM_DEBUGF("[KNX-UM] getGATableHTML: rebuilding table for %d segments\n", segmentCount);
  String html = "<table style='font-size:11px;border-collapse:collapse;width:100%;'>";
  html += "<tr style='background:#333;color:white;'><th style='border:1px solid #666;padding:4px;'>GA Type</th><th style='border:1px solid #666;padding:4px;'>Main</th>";
  for (uint8_t seg = 1; seg < segmentCount && seg < 6; seg++) {
    html += "<th style='border:1px solid #666;padding:4px;'>Seg" + String(seg) + "</th>";
  }
  html += "</tr>";
  auto formatGA = [](uint16_t ga) -> String {
    if (ga == 0) return "-";
    uint8_t main = (ga >> 11) & 0x1F;
    uint8_t middle = (ga >> 8) & 0x07;
    uint8_t sub = ga & 0xFF;
    return String(main) + "/" + String(middle) + "/" + String(sub);
  };
  std::vector<uint16_t> allUsedGAs;
  auto addRow = [&](const char* label, const char* centralGA, const char* section = "", bool globalOnly = false) {
    if (strlen(centralGA) == 0) return;
    html += "<tr><td style='border:1px solid #666;padding:2px 4px;color:white;'>" + String(label) + "</td>";
    if (globalOnly) {
      uint16_t mainGA = parseGA(centralGA);
      bool mainConflict = (mainGA > 0) && (std::find(allUsedGAs.begin(), allUsedGAs.end(), mainGA) != allUsedGAs.end());
      if (mainGA > 0) allUsedGAs.push_back(mainGA);
      String mainBg = mainConflict ? "background:#cc3333;" : "";
      html += "<td style='border:1px solid #666;padding:2px 4px;color:white;" + mainBg + "'>" + formatGA(mainGA) + "</td>";
      for (uint8_t seg = 1; seg < segmentCount && seg < 6; seg++) {
        html += "<td style='border:1px solid #666;padding:2px 4px;color:white;'>-</td>";
      }
    } else {
      uint16_t mainGA = calculateSegmentGA(centralGA, 0);
      bool mainConflict = (mainGA > 0) && (std::find(allUsedGAs.begin(), allUsedGAs.end(), mainGA) != allUsedGAs.end());
      if (mainGA > 0) allUsedGAs.push_back(mainGA);
      String mainBg = mainConflict ? "background:#cc3333;" : "";
      html += "<td style='border:1px solid #666;padding:2px 4px;color:white;" + mainBg + "'>" + formatGA(mainGA) + "</td>";
      for (uint8_t seg = 1; seg < segmentCount && seg < 6; seg++) {
        uint16_t segGA = calculateSegmentGA(centralGA, seg);
        bool segConflict = (segGA > 0) && (std::find(allUsedGAs.begin(), allUsedGAs.end(), segGA) != allUsedGAs.end());
        if (segGA > 0) allUsedGAs.push_back(segGA);
        String segBg = segConflict ? "background:#cc3333;" : "";
        html += "<td style='border:1px solid #666;padding:2px 4px;color:white;" + segBg + "'>" + formatGA(segGA) + "</td>";
      }
    }
    html += "</tr>";
  };
  html += "<tr><td colspan='" + String(1 + min((int)segmentCount, 6)) + "' style='border:1px solid #666;padding:4px;text-align:center;color:white;font-weight:bold;'>INPUT GAs (KNX → WLED)</td></tr>";
  addRow("Power", gaInPower);
  addRow("Brightness", gaInBri);
  addRow("Red", gaInR);
  addRow("Green", gaInG);
  addRow("Blue", gaInB);
  addRow("White", gaInW);
  addRow("CCT", gaInCct);
  addRow("Warm White", gaInWW);
  addRow("Cold White", gaInCW);
  addRow("Hue", gaInH);
  addRow("Saturation", gaInS);
  addRow("Value", gaInV);
  addRow("Effect", gaInFx);
  addRow("Preset", gaInPreset);
  addRow("RGB", gaInRGB);
  addRow("HSV", gaInHSV);
  addRow("RGBW", gaInRGBW);
  addRow("Time", gaInTime, "", true);
  addRow("Date", gaInDate, "", true);
  addRow("DateTime", gaInDateTime, "", true);
  addRow("Brightness Rel", gaInBriRel);
  addRow("Red Rel", gaInRRel);
  addRow("Green Rel", gaInGRel);
  addRow("Blue Rel", gaInBRel);
  addRow("White Rel", gaInWRel);
  addRow("Warm White Rel", gaInWWRel);
  addRow("Cold White Rel", gaInCWRel);
  addRow("Hue Rel", gaInHRel);
  addRow("Saturation Rel", gaInSRel);
  addRow("Value Rel", gaInVRel);
  addRow("Effect Rel", gaInFxRel);
  addRow("RGB Rel", gaInRGBRel);
  addRow("HSV Rel", gaInHSVRel);
  addRow("RGBW Rel", gaInRGBWRel);
  html += "<tr><td colspan='" + String(1 + min((int)segmentCount, 6)) + "' style='border:1px solid #666;padding:4px;text-align:center;color:white;font-weight:bold;'>OUTPUT GAs (WLED → KNX)</td></tr>";
  addRow("Power", gaOutPower);
  addRow("Brightness", gaOutBri);
  addRow("Red", gaOutR);
  addRow("Green", gaOutG);
  addRow("Blue", gaOutB);
  addRow("White", gaOutW);
  addRow("CCT", gaOutCct);
  addRow("Warm White", gaOutWW);
  addRow("Cold White", gaOutCW);
  addRow("Hue", gaOutH);
  addRow("Saturation", gaOutS);
  addRow("Value", gaOutV);
  addRow("Effect", gaOutFx);
  addRow("Preset", gaOutPreset);
  addRow("RGB", gaOutRGB);
  addRow("HSV", gaOutHSV);
  addRow("RGBW", gaOutRGBW);
  addRow("Internal Temp", gaOutIntTemp, "", true);
  addRow("Temp Sensor", gaOutTemp, "", true);
  addRow("Int Temp Alarm", gaOutIntTempAlarm, "", true);
  addRow("Temp Alarm", gaOutTempAlarm, "", true);
  html += "</table>";
  html += "<div style='font-size:10px;margin-top:4px;color:#aaa;'>Offsets: L=" + String(segmentOffsetL) + ", M=" + String(segmentOffsetM) + ", N=" + String(segmentOffsetN) + "</div>";
  gaTableCache = html;
  gaTableCacheHash = hash;
  gaTableCacheSegments = segmentCount;
  KNX_UM_DEBUGF("[KNX-UM] getGATableHTML: generated %d chars\n", html.length());
  return gaTableCache;
}

// -------------------- Usermod API --------------------
void KnxIpUsermod::setup() {
  if (!enabled) return;

  // Initialize last time
  lastTime = millis();

  // --- Early validation of PA and GA strings (mirrors readFromConfig pre-validation) ---
  if (*individualAddr && !KnxIpUsermod::validateIndividualAddressString(individualAddr)) {
    KNX_UM_WARNF("[KNX-UM][WARN] Invalid individual address '%s' at startup -> reverting to 1.1.100\n", individualAddr);
    strlcpy(individualAddr, "1.1.100", sizeof(individualAddr));
  }
  auto validateOrClear = [](char* s, const char* tag) {
    if (*s && !KnxIpUsermod::validateGroupAddressString(s)) {
      KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA '%s' (%s) at startup -> disabled\n", s, tag);
      s[0] = 0;
    }
  };
  validateOrClear(gaInPower, "gaInPower");
  validateOrClear(gaInBri, "gaInBri");
  validateOrClear(gaInR, "gaInR");
  validateOrClear(gaInG, "gaInG");
  validateOrClear(gaInB, "gaInB");
  validateOrClear(gaInW, "gaInW");
  validateOrClear(gaInCct, "gaInCct");
  validateOrClear(gaInWW, "gaInWW");
  validateOrClear(gaInCW, "gaInCW");
  validateOrClear(gaInH, "gaInH");
  validateOrClear(gaInS, "gaInS");
  validateOrClear(gaInV, "gaInV");
  validateOrClear(gaInFx, "gaInFx");
  validateOrClear(gaInPreset, "gaInPreset");
  validateOrClear(gaInRGB, "gaInRGB");
  validateOrClear(gaInHSV, "gaInHSV");
  validateOrClear(gaInRGBW, "gaInRGBW");
  validateOrClear(gaInTime, "gaInTime");
  validateOrClear(gaInDate, "gaInDate");
  validateOrClear(gaInDateTime, "gaInDateTime");
  validateOrClear(gaInBriRel, "gaInBriRel");
  validateOrClear(gaInRRel, "gaInRRel");
  validateOrClear(gaInGRel, "gaInGRel");
  validateOrClear(gaInBRel, "gaInBRel");
  validateOrClear(gaInWRel, "gaInWRel");
  validateOrClear(gaInWWRel, "gaInWWRel");
  validateOrClear(gaInCWRel, "gaInCWRel");
  validateOrClear(gaInHRel, "gaInHRel");
  validateOrClear(gaInSRel, "gaInSRel");
  validateOrClear(gaInVRel, "gaInVRel");
  validateOrClear(gaInFxRel, "gaInFxRel");
  validateOrClear(gaInRGBRel, "gaInRGBRel");
  validateOrClear(gaInHSVRel, "gaInHSVRel");
  validateOrClear(gaInRGBWRel, "gaInRGBWRel");
  validateOrClear(gaOutPower, "gaOutPower");
  validateOrClear(gaOutBri, "gaOutBri");
  validateOrClear(gaOutR, "gaOutR");
  validateOrClear(gaOutG, "gaOutG");
  validateOrClear(gaOutB, "gaOutB");
  validateOrClear(gaOutW, "gaOutW");
  validateOrClear(gaOutCct, "gaOutCct");
  validateOrClear(gaOutWW, "gaOutWW");
  validateOrClear(gaOutCW, "gaOutCW");
  validateOrClear(gaOutH, "gaOutH");
  validateOrClear(gaOutS, "gaOutS");
  validateOrClear(gaOutV, "gaOutV");
  validateOrClear(gaOutFx, "gaOutFx");
  validateOrClear(gaOutPreset, "gaOutPreset");
  validateOrClear(gaOutRGB, "gaOutRGB");
  validateOrClear(gaOutHSV, "gaOutHSV");
  validateOrClear(gaOutRGBW, "gaOutRGBW");
  validateOrClear(gaOutIntTemp, "gaOutIntTemp");
  validateOrClear(gaOutTemp, "gaOutTemp");
  validateOrClear(gaOutIntTempAlarm, "gaOutIntTempAlarm");
  validateOrClear(gaOutTempAlarm, "gaOutTempAlarm");

  // Parse & set PA
  if (uint16_t pa = parsePA(individualAddr)) {
    KNX.setIndividualAddress(pa);
    KNX_UM_DEBUGF("[KNX-UM] PA set to %u.%u.%u (0x%04X)\n",
                  (unsigned)((pa >> 12) & 0x0F), (unsigned)((pa >> 8) & 0x0F), (unsigned)(pa & 0xFF), pa);
  } else {
    KNX_UM_WARNF("[KNX-UM][WARN] Invalid individual address '%s' -> using previous/not set\n", individualAddr);
  }

  // Apply Communication Enhancement to the core
  KNX.setCommunicationEnhancement(commEnhance, commResends, commResendGapMs, commRxDedupMs);
  KNX_UM_DEBUGF("[KNX-UM] CommEnhance %s (resends=%u gapMs=%u dedupMs=%u)\n",
                commEnhance ? "ON" : "OFF", commResends, commResendGapMs, commRxDedupMs);

  // Parse IN GA strings (always)
  GA_IN_PWR = parseGA(gaInPower);
  GA_IN_BRI = parseGA(gaInBri);
  GA_IN_R = parseGA(gaInR);
  GA_IN_G = parseGA(gaInG);
  GA_IN_B = parseGA(gaInB);
  GA_IN_FX = parseGA(gaInFx);
  GA_IN_PRE = parseGA(gaInPreset);
  if (!GA_IN_PWR && *gaInPower) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in power '%s'\n", gaInPower);
  if (!GA_IN_BRI && *gaInBri) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in bri '%s'\n", gaInBri);
  if (!GA_IN_R && *gaInR) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in r '%s'\n", gaInR);
  if (!GA_IN_G && *gaInG) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in g '%s'\n", gaInG);
  if (!GA_IN_B && *gaInB) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in b '%s'\n", gaInB);
  if (!GA_IN_FX && *gaInFx) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in fx '%s'\n", gaInFx);
  if (!GA_IN_PRE && *gaInPreset) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in preset '%s'\n", gaInPreset);
  KNX_UM_DEBUGF("[KNX-UM] IN  pwr=0x%04X bri=0x%04X R=0x%04X G=0x%04X B=0x%04X fx=0x%04X pre=0x%04X\n",
                GA_IN_PWR, GA_IN_BRI, GA_IN_R, GA_IN_G, GA_IN_B, GA_IN_FX, GA_IN_PRE);

  GA_IN_RGB = parseGA(gaInRGB);
  GA_IN_HSV = parseGA(gaInHSV);
  GA_IN_RGBW = parseGA(gaInRGBW);
  GA_IN_RGB_REL = parseGA(gaInRGBRel);
  GA_IN_HSV_REL = parseGA(gaInHSVRel);
  GA_IN_RGBW_REL = parseGA(gaInRGBWRel);

  // Debug: Log composite group addresses
  KNX_UM_DEBUGF("[KNX-UM] COMPOSITE: RGB=0x%04X HSV=0x%04X RGBW=0x%04X RGB_REL=0x%04X HSV_REL=0x%04X RGBW_REL=0x%04X\n",
                GA_IN_RGB, GA_IN_HSV, GA_IN_RGBW, GA_IN_RGB_REL, GA_IN_HSV_REL, GA_IN_RGBW_REL);
  GA_IN_H = parseGA(gaInH);
  GA_IN_S = parseGA(gaInS);
  GA_IN_V = parseGA(gaInV);
  if (!GA_IN_H && *gaInH) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in h '%s'\n", gaInH);
  if (!GA_IN_S && *gaInS) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in s '%s'\n", gaInS);
  if (!GA_IN_V && *gaInV) KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA in v '%s'\n", gaInV);

  GA_IN_TIME = parseGA(gaInTime);
  GA_IN_DATE = parseGA(gaInDate);
  GA_IN_DATETIME = parseGA(gaInDateTime);
  GA_IN_BRI_REL = parseGA(gaInBriRel);
  GA_IN_R_REL = parseGA(gaInRRel);
  GA_IN_G_REL = parseGA(gaInGRel);
  GA_IN_B_REL = parseGA(gaInBRel);
  GA_IN_W_REL = parseGA(gaInWRel);
  GA_IN_WW_REL = parseGA(gaInWWRel);
  GA_IN_CW_REL = parseGA(gaInCWRel);
  GA_IN_H_REL = parseGA(gaInHRel);
  GA_IN_S_REL = parseGA(gaInSRel);
  GA_IN_V_REL = parseGA(gaInVRel);
  GA_IN_FX_REL = parseGA(gaInFxRel);

  // --- LED capability detect & gate inputs ---
  g_ledProfile = detectLedProfileFromSegments();
  bool allowRGB = (g_ledProfile == LedProfile::RGB || g_ledProfile == LedProfile::RGBW || g_ledProfile == LedProfile::RGBCCT);
  bool allowW = (g_ledProfile == LedProfile::RGBW || g_ledProfile == LedProfile::MONO);
  bool allowCCT = (g_ledProfile == LedProfile::CCT || g_ledProfile == LedProfile::RGBCCT);

  KNX_UM_DEBUGF("[KNX-UM] LED profile: %s (RGB=%d, W=%d, CCT=%d)\n",
                (g_ledProfile == LedProfile::MONO ? "MONO" : g_ledProfile == LedProfile::CCT  ? "CCT"
                                                           : g_ledProfile == LedProfile::RGB  ? "RGB"
                                                           : g_ledProfile == LedProfile::RGBW ? "RGBW"
                                                                                              : "RGBCCT"),
                (int)allowRGB, (int)allowW, (int)allowCCT);

  if (!allowRGB) {
    KNX_UM_DEBUGF("[KNX-UM] DEBUG: allowRGB=false, disabling RGB group addresses\n");
    GA_IN_R = GA_IN_G = GA_IN_B = GA_IN_R_REL = GA_IN_G_REL = GA_IN_B_REL = GA_IN_RGB =
      GA_IN_HSV = GA_IN_H = GA_IN_S = GA_IN_V = GA_IN_H_REL = GA_IN_S_REL = GA_IN_V_REL =
        GA_IN_RGB_REL = GA_IN_HSV_REL = 0;
  } else {
    KNX_UM_DEBUGF("[KNX-UM] DEBUG: allowRGB=true, RGB group addresses enabled\n");
  }
  if (!allowW) {
    GA_IN_W = GA_IN_W_REL = 0;
  }
  if (!allowCCT) {
    GA_IN_CCT = GA_IN_WW = GA_IN_CW = GA_IN_WW_REL = GA_IN_CW_REL = 0;
  }
  if (!(g_ledProfile == LedProfile::RGBW || g_ledProfile == LedProfile::RGBCCT)) {
    GA_IN_RGBW = GA_IN_RGBW_REL = 0;
  }

  // Debug: Final values after allowRGB logic
  KNX_UM_DEBUGF("[KNX-UM] FINAL: RGB=0x%04X HSV=0x%04X RGBW=0x%04X\n", GA_IN_RGB, GA_IN_HSV, GA_IN_RGBW);

  // Absolute W/CCT inputs are re-parsed here, preserving their existing registration behavior.
  GA_IN_W = parseGA(gaInW);
  if (GA_IN_W) {
    register1ByteHandler(GA_IN_W, DptMain::DPT_5xx, [this](uint8_t v) { onKnxWhite(v); });
  }

  GA_IN_CCT = parseGA(gaInCct);
  if (GA_IN_CCT) {
    registerMultiHandler(GA_IN_CCT, DptMain::DPT_7xx, 2, [this](const uint8_t* p) {
      uint16_t kelvin = ((uint16_t)p[0] << 8) | (uint16_t)p[1];
      onKnxCct(kelvin);
    });
  }

  // Optional direct WW/CW level inputs
  GA_IN_WW = parseGA(gaInWW);
  if (GA_IN_WW) {
    register1ByteHandler(GA_IN_WW, DptMain::DPT_5xx, [this](uint8_t v) { onKnxWW(v); });
  }

  GA_IN_CW = parseGA(gaInCW);
  if (GA_IN_CW) {
    register1ByteHandler(GA_IN_CW, DptMain::DPT_5xx, [this](uint8_t v) { onKnxCW(v); });
  }

  registerColorHandlers();

  // DPT 10.001 TimeOfDay (3 bytes)
  if (GA_IN_TIME) {
    KNX.addGroupObject(GA_IN_TIME, DptMain::DPT_10xx, /*tx=*/false, /*rx=*/true);
    KNX.onGroup(GA_IN_TIME, [this](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
      if (svc == KnxService::GroupValue_Write && p && len >= 3) onKnxTime_10_001(p, len);
    });
  }
  // DPT 11.001 Date (3 bytes)
  if (GA_IN_DATE) {
    KNX.addGroupObject(GA_IN_DATE, DptMain::DPT_11xx, false, true);
    KNX.onGroup(GA_IN_DATE, [this](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
      if (svc == KnxService::GroupValue_Write && p && len >= 3) onKnxDate_11_001(p, len);
    });
  }
  // DPT 19.001 DateTime (8 bytes)
  if (GA_IN_DATETIME) {
    KNX.addGroupObject(GA_IN_DATETIME, DptMain::DPT_19xx, false, true);
    KNX.onGroup(GA_IN_DATETIME, [this](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
      if (svc == KnxService::GroupValue_Write && p && len >= 8) onKnxDateTime_19_001(p, len);
    });
  }

  // Parse OUT GA strings
  GA_OUT_PWR = parseGA(gaOutPower);
  GA_OUT_BRI = parseGA(gaOutBri);
  GA_OUT_FX = parseGA(gaOutFx);
  GA_OUT_R = parseGA(gaOutR);
  GA_OUT_G = parseGA(gaOutG);
  GA_OUT_B = parseGA(gaOutB);
  GA_OUT_PRE = parseGA(gaOutPreset);
  GA_OUT_W = parseGA(gaOutW);
  GA_OUT_CCT = parseGA(gaOutCct);
  GA_OUT_WW = parseGA(gaOutWW);
  GA_OUT_CW = parseGA(gaOutCW);
  GA_OUT_RGB = parseGA(gaOutRGB);
  GA_OUT_HSV = parseGA(gaOutHSV);
  GA_OUT_RGBW = parseGA(gaOutRGBW);
  GA_OUT_H = parseGA(gaOutH);
  GA_OUT_S = parseGA(gaOutS);
  GA_OUT_V = parseGA(gaOutV);
  GA_OUT_INT_TEMP = parseGA(gaOutIntTemp);
  GA_OUT_TEMP = parseGA(gaOutTemp);
  GA_OUT_INT_TEMP_ALARM = parseGA(gaOutIntTempAlarm);
  GA_OUT_TEMP_ALARM = parseGA(gaOutTempAlarm);

  KNX_UM_DEBUGF("[KNX-UM] OUT pwr=0x%04X bri=0x%04X R=0x%04X G=0x%04X B=0x%04X W=0x%04X CCT=0x%04X WW=0x%04X CW=0x%04X fx=0x%04X pre=0x%04X H=%04X S=%04X V=%04X\n",
                GA_OUT_PWR, GA_OUT_BRI, GA_OUT_R, GA_OUT_G, GA_OUT_B, GA_OUT_W, GA_OUT_CCT, GA_OUT_WW, GA_OUT_CW, GA_OUT_FX, GA_OUT_PRE, GA_OUT_H, GA_OUT_S, GA_OUT_V);

  // --- Gate outputs by LED capability ---
  if (!(g_ledProfile == LedProfile::RGB || g_ledProfile == LedProfile::RGBW || g_ledProfile == LedProfile::RGBCCT)) {
    GA_OUT_R = GA_OUT_G = GA_OUT_B = 0;
  }
  if (!(g_ledProfile == LedProfile::RGBW)) {
    GA_OUT_W = 0;
  }
  if (!(g_ledProfile == LedProfile::CCT || g_ledProfile == LedProfile::RGBCCT)) {
    GA_OUT_CCT = GA_OUT_WW = GA_OUT_CW = 0;
  }
  if (!(g_ledProfile == LedProfile::RGB || g_ledProfile == LedProfile::RGBW || g_ledProfile == LedProfile::RGBCCT)) {
    GA_OUT_RGB = GA_OUT_HSV = GA_OUT_H = GA_OUT_S = GA_OUT_V = 0;
  }
  if (!(g_ledProfile == LedProfile::RGBW || g_ledProfile == LedProfile::RGBCCT)) {
    GA_OUT_RGBW = 0;
  }

  // Register inbound objects and callbacks
  if (GA_IN_PWR) {
    KNX.addGroupObject(GA_IN_PWR, DptMain::DPT_1xx, false, true);
    KNX.onGroup(GA_IN_PWR, [this](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) {
      if (svc == KnxService::GroupValue_Write && p && len >= 1) onKnxPower((p[0] & 0x01) != 0);
    });
  }

  if (GA_IN_BRI) {
    registerScalingHandler(GA_IN_BRI, [this](uint8_t pct) { onKnxBrightness(pct); });
  }
  // RGB inputs (masked to 0 if unsupported)
  if (GA_IN_R) {
    register1ByteHandler(GA_IN_R, DptMain::DPT_5xx, [this](uint8_t r) {
      uint8_t cr, cg, cb, cw;
      getCurrentRGBW(cr, cg, cb, cw);
      onKnxRGB(r, cg, cb);
    });
  }
  if (GA_IN_G) {
    register1ByteHandler(GA_IN_G, DptMain::DPT_5xx, [this](uint8_t g) {
      uint8_t cr, cg, cb, cw;
      getCurrentRGBW(cr, cg, cb, cw);
      onKnxRGB(cr, g, cb);
    });
  }
  if (GA_IN_B) {
    register1ByteHandler(GA_IN_B, DptMain::DPT_5xx, [this](uint8_t b) {
      uint8_t cr, cg, cb, cw;
      getCurrentRGBW(cr, cg, cb, cw);
      onKnxRGB(cr, cg, b);
    });
  }

  if (GA_IN_FX) {
    register1ByteHandler(GA_IN_FX, DptMain::DPT_5xx, [this](uint8_t v) { onKnxEffect(v); });
  }

  if (GA_IN_PRE) {
    KNX.addGroupObject(GA_IN_PRE, DptMain::DPT_5xx, false, true);
    KNX.onGroup(GA_IN_PRE, [this](uint16_t, DptMain, KnxService svc, const uint8_t* p, uint8_t len) { if (svc==KnxService::GroupValue_Write && p && len>=1) onKnxPreset(p[0]); });
  }

  // Outbound/status objects
  if (GA_OUT_PWR) KNX.addGroupObject(GA_OUT_PWR, DptMain::DPT_1xx, /*tx=*/true, /*rx=*/false);
  if (GA_OUT_BRI) KNX.addGroupObject(GA_OUT_BRI, DptMain::DPT_5xx, true, false);
  if (GA_OUT_FX) KNX.addGroupObject(GA_OUT_FX, DptMain::DPT_5xx, true, false);
  if (GA_OUT_R) KNX.addGroupObject(GA_OUT_R, DptMain::DPT_5xx, true, false);
  if (GA_OUT_G) KNX.addGroupObject(GA_OUT_G, DptMain::DPT_5xx, true, false);
  if (GA_OUT_B) KNX.addGroupObject(GA_OUT_B, DptMain::DPT_5xx, true, false);
  if (GA_OUT_PRE) KNX.addGroupObject(GA_OUT_PRE, DptMain::DPT_5xx, true, false);
  if (GA_OUT_W) KNX.addGroupObject(GA_OUT_W, DptMain::DPT_5xx, true, false);
  if (GA_OUT_CCT) KNX.addGroupObject(GA_OUT_CCT, DptMain::DPT_7xx, true, false);
  if (GA_OUT_WW) KNX.addGroupObject(GA_OUT_WW, DptMain::DPT_5xx, true, false);
  if (GA_OUT_CW) KNX.addGroupObject(GA_OUT_CW, DptMain::DPT_5xx, true, false);
  if (GA_OUT_RGB) KNX.addGroupObject(GA_OUT_RGB, DptMain::DPT_232xx, true, false);
  if (GA_OUT_HSV) KNX.addGroupObject(GA_OUT_HSV, DptMain::DPT_232xx, true, false);
  if (GA_OUT_RGBW) KNX.addGroupObject(GA_OUT_RGBW, DptMain::DPT_251xx, true, false);
  if (GA_OUT_H) KNX.addGroupObject(GA_OUT_H, DptMain::DPT_5xx, true, false);
  if (GA_OUT_S) KNX.addGroupObject(GA_OUT_S, DptMain::DPT_5xx, true, false);
  if (GA_OUT_V) KNX.addGroupObject(GA_OUT_V, DptMain::DPT_5xx, true, false);
  if (GA_OUT_INT_TEMP) KNX.addGroupObject(GA_OUT_INT_TEMP, DptMain::DPT_14xx, true, false);
  if (GA_OUT_TEMP) KNX.addGroupObject(GA_OUT_TEMP, DptMain::DPT_14xx, true, false);
  if (GA_OUT_INT_TEMP_ALARM) KNX.addGroupObject(GA_OUT_INT_TEMP_ALARM, DptMain::DPT_1xx, /*tx=*/true, /*rx=*/false);
  if (GA_OUT_TEMP_ALARM) KNX.addGroupObject(GA_OUT_TEMP_ALARM, DptMain::DPT_1xx, /*tx=*/true, /*rx=*/false);

  // Register per-segment KOs
  registerSegmentKOs();

  KNX_UM_DEBUGF("[KNX-UM] OUT intTemp=0x%04X temp=0x%04X\n", GA_OUT_INT_TEMP, GA_OUT_TEMP);
  KNX_UM_DEBUGF("[KNX-UM] OUT intTempAlarm=0x%04X tempAlarm=0x%04X (thr: %.1f/%.1f °C, hyst=%.1f)\n",

                GA_OUT_INT_TEMP_ALARM, GA_OUT_TEMP_ALARM, intTempAlarmMaxC, dallasTempAlarmMaxC, tempAlarmHystC);
  // Start KNX
  IPAddress ip = WLEDNetwork.localIP();
  if (!ip || ip.toString() == String("0.0.0.0")) {
    KNX_UM_DEBUGLN("[KNX-UM] Network connected but no IP yet, deferring KNX.begin().");
    return;
  }

#ifdef ARDUINO_ARCH_ESP32
  if (WLEDNetwork.isEthernet()) {
    // For Ethernet, we don't need to disable WiFi sleep
    KNX_UM_DEBUGLN("[KNX-UM] Using Ethernet connection");
  } else {
    WiFi.setSleep(false); // modem-sleep off helps WiFi multicast reliability
    KNX_UM_DEBUGLN("[KNX-UM] Using WiFi connection, sleep disabled");

    delay(100); // Give lwIP time to complete initialization

    // Verify WiFi is still connected after delay
    if (WiFi.status() != WL_CONNECTED) {
      KNX_UM_DEBUGLN("[KNX-UM] WiFi disconnected during lwIP wait, deferring KNX.begin().");
      return;
    }
  }
#endif

  // Additional safety: ensure we're not in a critical network transition
  yield();

  bool ok = KNX.begin();
  KNX_UM_DEBUGF("[KNX-UM] KNX.begin() -> %s (localIP=%s)\n", ok ? "OK" : "FAILED", ip.toString().c_str());

  // Send UDP debug for remote monitoring
  KNX_UDP_LOG("[KNX-UM] KNX usermod setup: begin() -> %s (localIP=%s)", ok ? "OK" : "FAILED", ip.toString().c_str());
  if (WLEDNetwork.isEthernet()) {
    KNX_UDP_LOG("[KNX-UM] Network type: Ethernet");
  } else {
    KNX_UDP_LOG("[KNX-UM] Network type: WiFi");
  }

  // Check for GA conflicts and notify GUI if found
  // Do this after KNX initialization to ensure all GAs are registered
  checkGAConflictsAndNotifyGUI();
}

void KnxIpUsermod::loop() {
  if (!enabled) return;

  unsigned long now = millis();
  if (int((now - lastTime) / 1000) > TIMEOUT_60_SECONDS) {
    // Reset last time
    lastTime = now;
    // Implement 1-min task logic
    KNX_UM_DEBUGLN("[KNX-UM] 1-min task triggered\n");

    // 1-min task logic here
  }

  // --- Detect LED capability (lc) change at runtime and rebuild GA mapping immediately ---
  static uint8_t s_lastLc = 0xFF;
  static unsigned long s_lcChangedAt = 0;

  // OR light-capabilities across all segments (robust for multi-bus setups)
  uint8_t lcNow = 0;
  const uint16_t segCount = strip.getSegmentsNum();
  for (uint16_t i = 0; i < segCount; i++) {
    lcNow |= strip.getSegment(i).getLightCapabilities(); // bit0=RGB, bit1=W, bit2=CCT
  }

  if (lcNow != s_lastLc) {
    s_lastLc = lcNow;
    s_lcChangedAt = millis();
    KNX_UM_DEBUGF("[KNX-UM] LED capabilities changed (lc=0x%02X). Pending rebuild...\n", lcNow);
  }

  // Debounce (avoid thrashing while user edits LED settings in the UI)
  if (s_lcChangedAt && (millis() - s_lcChangedAt >= 300)) {
    s_lcChangedAt = 0;

    LedProfile newProf = detectLedProfileFromSegments();
    if (newProf != g_ledProfile) {
      KNX_UM_DEBUGF("[KNX-UM] LED profile changed %s -> %s. Re-registering KNX GAs now.\n",
                    (g_ledProfile == LedProfile::MONO ? "MONO" : g_ledProfile == LedProfile::CCT  ? "CCT"
                                                               : g_ledProfile == LedProfile::RGB  ? "RGB"
                                                               : g_ledProfile == LedProfile::RGBW ? "RGBW"
                                                                                                  : "RGBCCT"),
                    (newProf == LedProfile::MONO ? "MONO" : newProf == LedProfile::CCT  ? "CCT"
                                                          : newProf == LedProfile::RGB  ? "RGB"
                                                          : newProf == LedProfile::RGBW ? "RGBW"
                                                                                        : "RGBCCT"));

      // Full rebuild: drop socket + GA registry; setup() will detect and re-register
      KNX.end();
      KNX.clearRegistrations();
      g_ledProfile = newProf; // update hint; setup() re-detects and gates GAs
      setup();

      // Optional: primer so routers/ETS learn us immediately
      if (KNX.running()) {
        const uint16_t primer = knxMakeGroupAddress(0, 0, 1);
        KNX.groupValueRead(primer);
      }
    }
  }

  // If KNX could not start in setup() due to missing IP, retry once we have one.
  static bool knxStartedLogged = false;
  if (!knxStartedLogged) {
    if (WLEDNetwork.isConnected()) {
      IPAddress ip = WLEDNetwork.localIP();
      if (ip && ip.toString() != String("0.0.0.0")) {
        KNX_UM_DEBUGLN("[KNX-UM] Network ready (got IP). Retrying KNX.begin()...");

// Same lwIP safety as in setup() - prevent "Invalid mbox" crash
#ifdef ARDUINO_ARCH_ESP32
        if (!WLEDNetwork.isEthernet()) {
          delay(100); // Give lwIP time to stabilize
          if (WiFi.status() != WL_CONNECTED) {
            KNX_UM_DEBUGLN("[KNX-UM] WiFi disconnected during retry wait.");
            return;
          }
        }
#endif
        yield();

        bool ok = KNX.begin();
        KNX_UM_DEBUGF("[KNX-UM] KNX.begin() -> %s (localIP=%s)\n",
                      ok ? "OK" : "FAILED",
                      ip.toString().c_str());
        if (ok) knxStartedLogged = true;
      } else {
        KNX_UM_DEBUGLN("[KNX-UM] Network connected, waiting for IP...");
      }
    } else {
      KNX_UM_DEBUGLN("[KNX-UM] Network not connected yet.");
    }
  }

  // Detect network IP changes and refresh IGMP membership without tearing socket down
  static IPAddress _lastIpForKnx;
  if (KNX.running()) {
    IPAddress cur = WLEDNetwork.localIP();
    if (cur && cur.toString() != String("0.0.0.0")) {
      if (_lastIpForKnx != cur) {
        KNX_UM_DEBUGF("[KNX-UM] Network IP changed %s -> %s, refreshing KNX multicast membership...\n",
                      _lastIpForKnx.toString().c_str(), cur.toString().c_str());
        if (!KNX.rejoinMulticast()) {
          // Fallback: hard restart the KNX socket if rejoin fails
          KNX.end();
          KNX.begin();
        }
        _lastIpForKnx = cur;
      }
    }
  }

  KNX.loop();

  // GUI-driven changes now unified: we trigger scheduleStatePublish() elsewhere (handlers or periodic).
  // Light color/effect changes occurring outside KNX handlers rely on LAST_* snapshot differences handled
  // in scheduleStatePublish() via _pendingTxColor flag. To avoid over-chatter, we maintain a debounce window.
  {
    uint32_t uiNow = millis();
    if (uiNow - _lastUiSendMs >= _minUiSendIntervalMs) {
      // We just mark that a potential GUI-originated change occurred by invoking the scheduler.
      // The scheduler will compare against its last snapshot and set pending flags appropriately.
      _lastUiSendMs = uiNow;
      scheduleStatePublish();
    }
  }

  // Preset: rely solely on scheduleStatePublish() detecting preset changes; no direct send here.

  // Optional periodic state publish
  if (periodicEnabled) {
    uint32_t now2 = millis();
    if (now2 - _lastPeriodicMs >= periodicIntervalMs) {
      _lastPeriodicMs = now2;
      scheduleStatePublish();
    }
  }

  processPendingState();

}

void KnxIpUsermod::addToConfig(JsonObject& root) {
  JsonObject top = root.createNestedObject("KNX_IP");
  top["enabled"] = enabled;
  top["individual_address"] = individualAddr;
  top["tx_rate_limit_ms"] = txRateLimitMs;
  top["color_out_mode"] = colorOutMode; // 0=channels,1=composites,2=both
  top["periodic_enabled"] = periodicEnabled;
  top["periodic_interval_ms"] = periodicIntervalMs;
  top["cct_kelvin_min"] = kelvinMin;
  top["cct_kelvin_max"] = kelvinMax;
  top["communication_enhancement"] = commEnhance;
  top["communication_resends"] = commResends;
  top["communication_resend_gap"] = commResendGapMs;
  top["communication_rx_dedup"] = commRxDedupMs;
  top["Internal Temperature Alarm"] = intTempAlarmMaxC;
  top["Temperature Sensor Alarm"] = dallasTempAlarmMaxC;
  top["Temperature Alarm Hysteresis"] = tempAlarmHystC;
  top["auto_enable_on_color"] = autoEnableOnColor;
  top["auto_enable_brightness"] = autoEnableBrightness;
  top["segment_offset_L"] = segmentOffsetL;
  top["segment_offset_M"] = segmentOffsetM;
  top["segment_offset_N"] = segmentOffsetN;

  JsonObject gIn = top.createNestedObject("GA in");
  gIn["power"] = gaInPower;
  gIn["bri"] = gaInBri;
  gIn["r"] = gaInR;
  gIn["g"] = gaInG;
  gIn["b"] = gaInB;
  gIn["w"] = gaInW;
  gIn["cct"] = gaInCct;
  gIn["ww"] = gaInWW;
  gIn["cw"] = gaInCW;
  gIn["h"] = gaInH; // DPT 5.003
  gIn["s"] = gaInS; // DPT 5.001
  gIn["v"] = gaInV; // DPT 5.001
  gIn["fx"] = gaInFx;
  gIn["preset"] = gaInPreset;
  gIn["rgb"] = gaInRGB;   // DPST-232-600 (3B)
  gIn["hsv"] = gaInHSV;   // DPST-232-600 (3B)
  gIn["rgbw"] = gaInRGBW; // DPST-251-600 (6B)
  gIn["time"] = gaInTime;
  gIn["date"] = gaInDate;
  gIn["datetime"] = gaInDateTime;
  gIn["bri_rel"] = gaInBriRel;   // DPT 3.007 step/direction
  gIn["r_rel"] = gaInRRel;       // DPT 3.007
  gIn["g_rel"] = gaInGRel;       // DPT 3.007
  gIn["b_rel"] = gaInBRel;       // DPT 3.007
  gIn["w_rel"] = gaInWRel;       // DPT 3.007
  gIn["ww_rel"] = gaInWWRel;     // DPT 3.007
  gIn["cw_rel"] = gaInCWRel;     // DPT 3.007
  gIn["h_rel"] = gaInHRel;       // DPT 3.007
  gIn["s_rel"] = gaInSRel;       // DPT 3.007
  gIn["v_rel"] = gaInVRel;       // DPT 3.007
  gIn["fx_rel"] = gaInFxRel;     // DPT 3.007
  gIn["rgb_rel"] = gaInRGBRel;   // DPST-232-600 (3B, each byte low nibble DPT3)
  gIn["hsv_rel"] = gaInHSVRel;   // DPST-232-600 (3B, each byte low nibble DPT3)
  gIn["rgbw_rel"] = gaInRGBWRel; // DPST-251-600 (4+ bytes, first 4 used)

  JsonObject gOut = top.createNestedObject("GA out");
  gOut["power"] = gaOutPower;
  gOut["bri"] = gaOutBri;
  gOut["r"] = gaOutR;
  gOut["g"] = gaOutG;
  gOut["b"] = gaOutB;
  gOut["w"] = gaOutW;
  gOut["cct"] = gaOutCct;
  gOut["ww"] = gaOutWW;
  gOut["cw"] = gaOutCW;
  gOut["h"] = gaOutH; // DPT 5.003
  gOut["s"] = gaOutS; // DPT 5.001
  gOut["v"] = gaOutV; // DPT 5.001
  gOut["fx"] = gaOutFx;
  gOut["preset"] = gaOutPreset;
  gOut["rgb"] = gaOutRGB;   // DPST-232-600 (3B)
  gOut["hsv"] = gaOutHSV;   // DPST-232-600 (3B)
  gOut["rgbw"] = gaOutRGBW; // DPST-251-600 (6B)
  gOut["Internal_Temperature"] = gaOutIntTemp;
  gOut["Temperature_Sensor"] = gaOutTemp;
  gOut["Internal_Temperature_Alarm"] = gaOutIntTempAlarm;
  gOut["Temperature_Sensor_Alarm"] = gaOutTempAlarm;
}

bool KnxIpUsermod::readFromConfig(JsonObject& root) {
  JsonObject top = root["KNX_IP"];
  if (top.isNull()) {
    top = root["KNX-IP"]; // legacy
    if (top.isNull()) return false;
  }

  enabled = top["enabled"] | enabled;
  strlcpy(individualAddr, top["individual_address"] | individualAddr, sizeof(individualAddr));
  kelvinMin = top["cct_kelvin_min"] | kelvinMin;
  kelvinMax = top["cct_kelvin_max"] | kelvinMax;
  periodicEnabled = top["periodic_enabled"] | periodicEnabled;
  periodicIntervalMs = top["periodic_interval_ms"] | periodicIntervalMs;
  colorOutMode = top["color_out_mode"] | colorOutMode;
  commEnhance = top["communication_enhancement"] | commEnhance;
  commResends = top["communication_resends"] | commResends;
  commResendGapMs = top["communication_resend_gap"] | commResendGapMs;
  commRxDedupMs = top["communication_rx_dedup"] | commRxDedupMs;
  autoEnableOnColor = top["auto_enable_on_color"] | autoEnableOnColor;
  autoEnableBrightness = top["auto_enable_brightness"] | autoEnableBrightness;
  if (autoEnableBrightness > 255) autoEnableBrightness = 255;
  segmentOffsetL = top["segment_offset_L"] | segmentOffsetL;
  segmentOffsetM = top["segment_offset_M"] | segmentOffsetM;
  segmentOffsetN = top["segment_offset_N"] | segmentOffsetN;
  if (segmentOffsetL > 31) segmentOffsetL = 31;
  if (segmentOffsetM > 7) segmentOffsetM = 7;
  if (segmentOffsetN > 255) segmentOffsetN = 255;

  // accept either "GA in"/"GA out" (what we save) or "in"/"out"
  JsonObject gIn = top["GA in"];
  if (gIn.isNull()) gIn = top["in"];
  JsonObject gOut = top["GA out"];
  if (gOut.isNull()) gOut = top["out"];

  if (!gIn.isNull()) {
    strlcpy(gaInPower, gIn["power"] | gaInPower, sizeof(gaInPower));
    strlcpy(gaInBri, gIn["bri"] | gaInBri, sizeof(gaInBri));
    strlcpy(gaInR, gIn["r"] | gaInR, sizeof(gaInR));
    strlcpy(gaInG, gIn["g"] | gaInG, sizeof(gaInG));
    strlcpy(gaInB, gIn["b"] | gaInB, sizeof(gaInB));
    strlcpy(gaInW, gIn["w"] | gaInW, sizeof(gaInW));
    strlcpy(gaInCct, gIn["cct"] | gaInCct, sizeof(gaInCct));
    strlcpy(gaInWW, gIn["ww"] | gaInWW, sizeof(gaInWW));
    strlcpy(gaInCW, gIn["cw"] | gaInCW, sizeof(gaInCW));
    strlcpy(gaInH, gIn["h"] | gaInH, sizeof(gaInH));
    strlcpy(gaInS, gIn["s"] | gaInS, sizeof(gaInS));
    strlcpy(gaInV, gIn["v"] | gaInV, sizeof(gaInV));
    strlcpy(gaInFx, gIn["fx"] | gaInFx, sizeof(gaInFx));
    strlcpy(gaInPreset, gIn["preset"] | gaInPreset, sizeof(gaInPreset));
    strlcpy(gaInRGB, gIn["rgb"] | gaInRGB, sizeof(gaInRGB));
    strlcpy(gaInHSV, gIn["hsv"] | gaInHSV, sizeof(gaInHSV));
    strlcpy(gaInRGBW, gIn["rgbw"] | gaInRGBW, sizeof(gaInRGBW));
    strlcpy(gaInTime, gIn["time"] | gaInTime, sizeof(gaInTime));
    strlcpy(gaInDate, gIn["date"] | gaInDate, sizeof(gaInDate));
    strlcpy(gaInDateTime, gIn["datetime"] | gaInDateTime, sizeof(gaInDateTime));
    strlcpy(gaInBriRel, gIn["bri_rel"] | gaInBriRel, sizeof(gaInBriRel));     // DPT 3.007
    strlcpy(gaInRRel, gIn["r_rel"] | gaInRRel, sizeof(gaInRRel));             // DPT 3.007
    strlcpy(gaInGRel, gIn["g_rel"] | gaInGRel, sizeof(gaInGRel));             // DPT 3.007
    strlcpy(gaInBRel, gIn["b_rel"] | gaInBRel, sizeof(gaInBRel));             // DPT 3.007
    strlcpy(gaInWRel, gIn["w_rel"] | gaInWRel, sizeof(gaInWRel));             // DPT 3.007
    strlcpy(gaInWWRel, gIn["ww_rel"] | gaInWWRel, sizeof(gaInWWRel));         // DPT 3.007
    strlcpy(gaInCWRel, gIn["cw_rel"] | gaInCWRel, sizeof(gaInCWRel));         // DPT 3.007
    strlcpy(gaInHRel, gIn["h_rel"] | gaInHRel, sizeof(gaInHRel));             // DPT 3.007
    strlcpy(gaInSRel, gIn["s_rel"] | gaInSRel, sizeof(gaInSRel));             // DPT 3.007
    strlcpy(gaInVRel, gIn["v_rel"] | gaInVRel, sizeof(gaInVRel));             // DPT 3.007
    strlcpy(gaInFxRel, gIn["fx_rel"] | gaInFxRel, sizeof(gaInFxRel));         // DPT 3.007
    strlcpy(gaInRGBRel, gIn["rgb_rel"] | gaInRGBRel, sizeof(gaInRGBRel));     // composite rel
    strlcpy(gaInHSVRel, gIn["hsv_rel"] | gaInHSVRel, sizeof(gaInHSVRel));     // composite rel
    strlcpy(gaInRGBWRel, gIn["rgbw_rel"] | gaInRGBWRel, sizeof(gaInRGBWRel)); // composite rel
  }

  // --- Pre-validate GA / PA strings (clear invalid to prevent repeated parse warnings) ---
  // Validate individual address (personal address / PA)
  if (*individualAddr && !KnxIpUsermod::validateIndividualAddressString(individualAddr)) {
    KNX_UM_WARNF("[KNX-UM][WARN] Invalid individual address '%s' in config -> reverting to default 1.1.100\n", individualAddr);
    strlcpy(individualAddr, "1.1.100", sizeof(individualAddr));
  }

  auto validateOrClear = [](char* s, const char* tag) {
    if (*s && !KnxIpUsermod::validateGroupAddressString(s)) {
      KNX_UM_WARNF("[KNX-UM][WARN] Invalid GA '%s' (%s) -> disabled\n", s, tag);
      s[0] = 0; // disable
    }
  };

  // Inbound GAs
  validateOrClear(gaInPower, "gaInPower");
  validateOrClear(gaInBri, "gaInBri");
  validateOrClear(gaInR, "gaInR");
  validateOrClear(gaInG, "gaInG");
  validateOrClear(gaInB, "gaInB");
  validateOrClear(gaInW, "gaInW");
  validateOrClear(gaInCct, "gaInCct");
  validateOrClear(gaInWW, "gaInWW");
  validateOrClear(gaInCW, "gaInCW");
  validateOrClear(gaInH, "gaInH");
  validateOrClear(gaInS, "gaInS");
  validateOrClear(gaInV, "gaInV");
  validateOrClear(gaInFx, "gaInFx");
  validateOrClear(gaInPreset, "gaInPreset");
  validateOrClear(gaInRGB, "gaInRGB");
  validateOrClear(gaInHSV, "gaInHSV");
  validateOrClear(gaInRGBW, "gaInRGBW");
  validateOrClear(gaInTime, "gaInTime");
  validateOrClear(gaInDate, "gaInDate");
  validateOrClear(gaInDateTime, "gaInDateTime");
  validateOrClear(gaInBriRel, "gaInBriRel");
  validateOrClear(gaInRRel, "gaInRRel");
  validateOrClear(gaInGRel, "gaInGRel");
  validateOrClear(gaInBRel, "gaInBRel");
  validateOrClear(gaInWRel, "gaInWRel");
  validateOrClear(gaInWWRel, "gaInWWRel");
  validateOrClear(gaInCWRel, "gaInCWRel");
  validateOrClear(gaInHRel, "gaInHRel");
  validateOrClear(gaInSRel, "gaInSRel");
  validateOrClear(gaInVRel, "gaInVRel");
  validateOrClear(gaInFxRel, "gaInFxRel");
  validateOrClear(gaInRGBRel, "gaInRGBRel");
  validateOrClear(gaInHSVRel, "gaInHSVRel");
  validateOrClear(gaInRGBWRel, "gaInRGBWRel");

  // Outbound GAs
  if (!gOut.isNull()) {
    validateOrClear(gaOutPower, "gaOutPower");
    validateOrClear(gaOutBri, "gaOutBri");
    validateOrClear(gaOutR, "gaOutR");
    validateOrClear(gaOutG, "gaOutG");
    validateOrClear(gaOutB, "gaOutB");
    validateOrClear(gaOutW, "gaOutW");
    validateOrClear(gaOutCct, "gaOutCct");
    validateOrClear(gaOutWW, "gaOutWW");
    validateOrClear(gaOutCW, "gaOutCW");
    validateOrClear(gaOutH, "gaOutH");
    validateOrClear(gaOutS, "gaOutS");
    validateOrClear(gaOutV, "gaOutV");
    validateOrClear(gaOutFx, "gaOutFx");
    validateOrClear(gaOutPreset, "gaOutPreset");
    validateOrClear(gaOutRGB, "gaOutRGB");
    validateOrClear(gaOutHSV, "gaOutHSV");
    validateOrClear(gaOutRGBW, "gaOutRGBW");
    validateOrClear(gaOutIntTemp, "gaOutIntTemp");
    validateOrClear(gaOutTemp, "gaOutTemp");
    validateOrClear(gaOutIntTempAlarm, "gaOutIntTempAlarm");
    validateOrClear(gaOutTempAlarm, "gaOutTempAlarm");
  }

  if (!gOut.isNull()) {
    strlcpy(gaOutPower, gOut["power"] | gaOutPower, sizeof(gaOutPower));
    strlcpy(gaOutBri, gOut["bri"] | gaOutBri, sizeof(gaOutBri));
    strlcpy(gaOutR, gOut["r"] | gaOutR, sizeof(gaOutR));
    strlcpy(gaOutG, gOut["g"] | gaOutG, sizeof(gaOutG));
    strlcpy(gaOutB, gOut["b"] | gaOutB, sizeof(gaOutB));
    strlcpy(gaOutW, gOut["w"] | gaOutW, sizeof(gaOutW));
    strlcpy(gaOutCct, gOut["cct"] | gaOutCct, sizeof(gaOutCct));
    strlcpy(gaOutWW, gOut["ww"] | gaOutWW, sizeof(gaOutWW));
    strlcpy(gaOutCW, gOut["cw"] | gaOutCW, sizeof(gaOutCW));
    strlcpy(gaOutH, gOut["h"] | gaOutH, sizeof(gaOutH));
    strlcpy(gaOutS, gOut["s"] | gaOutS, sizeof(gaOutS));
    strlcpy(gaOutV, gOut["v"] | gaOutV, sizeof(gaOutV));
    strlcpy(gaOutFx, gOut["fx"] | gaOutFx, sizeof(gaOutFx));
    strlcpy(gaOutPreset, gOut["preset"] | gaOutPreset, sizeof(gaOutPreset));
    strlcpy(gaOutRGB, gOut["rgb"] | gaOutRGB, sizeof(gaOutRGB));
    strlcpy(gaOutHSV, gOut["hsv"] | gaOutHSV, sizeof(gaOutHSV));
    strlcpy(gaOutRGBW, gOut["rgbw"] | gaOutRGBW, sizeof(gaOutRGBW));
    strlcpy(gaOutIntTemp, gOut["Internal_Temperature"] | gaOutIntTemp, sizeof(gaOutIntTemp));
    strlcpy(gaOutTemp, gOut["Temperature_Sensor"] | gaOutTemp, sizeof(gaOutTemp));
    strlcpy(gaOutIntTempAlarm, gOut["Internal_Temperature_Alarm"] | gaOutIntTempAlarm, sizeof(gaOutIntTempAlarm));
    strlcpy(gaOutTempAlarm, gOut["Temperature_Sensor_Alarm"] | gaOutTempAlarm, sizeof(gaOutTempAlarm));
  }

  txRateLimitMs = top["tx_rate_limit_ms"] | txRateLimitMs;
  intTempAlarmMaxC = top["Internal Temperature Alarm"] | intTempAlarmMaxC;
  dallasTempAlarmMaxC = top["Temperature Sensor Alarm"] | dallasTempAlarmMaxC;
  tempAlarmHystC = top["Temperature Alarm Hysteresis"] | tempAlarmHystC;

  // --- decide if a rebuild is needed (compare new parsed GAs vs current cache) ---
  // keep snapshots of previous caches before we overwrite them
  const uint16_t PREV_IN_PWR = GA_IN_PWR, PREV_IN_BRI = GA_IN_BRI, PREV_IN_R = GA_IN_R,
                 PREV_IN_G = GA_IN_G, PREV_IN_B = GA_IN_B, PREV_IN_W = GA_IN_W,
                 PREV_IN_CCT = GA_IN_CCT, PREV_IN_WW = GA_IN_WW, PREV_IN_CW = GA_IN_CW,
                 PREV_IN_FX = GA_IN_FX, PREV_IN_PRE = GA_IN_PRE, PREV_IN_RGB = GA_IN_RGB,
                 PREV_IN_HSV = GA_IN_HSV, PREV_IN_RGBW = GA_IN_RGBW, PREV_IN_H = GA_IN_H,
                 PREV_IN_S = GA_IN_S, PREV_IN_V = GA_IN_V, PREV_IN_BRI_REL = GA_IN_BRI_REL,
                 PREV_IN_R_REL = GA_IN_R_REL, PREV_IN_G_REL = GA_IN_G_REL, PREV_IN_B_REL = GA_IN_B_REL,
                 PREV_IN_W_REL = GA_IN_W_REL, PREV_IN_WW_REL = GA_IN_WW_REL, PREV_IN_CW_REL = GA_IN_CW_REL,
                 PREV_IN_H_REL = GA_IN_H_REL, PREV_IN_S_REL = GA_IN_S_REL, PREV_IN_V_REL = GA_IN_V_REL,
                 PREV_IN_FX_REL = GA_IN_FX_REL, PREV_IN_RGB_REL = GA_IN_RGB_REL, PREV_IN_HSV_REL = GA_IN_HSV_REL,
                 PREV_IN_RGBW_REL = GA_IN_RGBW_REL;

  const uint16_t PREV_OUT_PWR = GA_OUT_PWR, PREV_OUT_BRI = GA_OUT_BRI, PREV_OUT_R = GA_OUT_R,
                 PREV_OUT_G = GA_OUT_G, PREV_OUT_B = GA_OUT_B, PREV_OUT_W = GA_OUT_W,
                 PREV_OUT_CCT = GA_OUT_CCT, PREV_OUT_WW = GA_OUT_WW, PREV_OUT_CW = GA_OUT_CW,
                 PREV_OUT_FX = GA_OUT_FX, PREV_OUT_PRE = GA_OUT_PRE, PREV_OUT_RGB = GA_OUT_RGB,
                 PREV_OUT_HSV = GA_OUT_HSV, PREV_OUT_RGBW = GA_OUT_RGBW, PREV_OUT_H = GA_OUT_H,
                 PREV_OUT_S = GA_OUT_S, PREV_OUT_V = GA_OUT_V;

  // parse the just-loaded GA strings into NEW values (locals)
  const uint16_t NEW_IN_PWR = parseGA(gaInPower);
  const uint16_t NEW_IN_BRI = parseGA(gaInBri);
  const uint16_t NEW_IN_R = parseGA(gaInR);
  const uint16_t NEW_IN_G = parseGA(gaInG);
  const uint16_t NEW_IN_B = parseGA(gaInB);
  const uint16_t NEW_IN_W = parseGA(gaInW);
  const uint16_t NEW_IN_CCT = parseGA(gaInCct);
  const uint16_t NEW_IN_WW = parseGA(gaInWW);
  const uint16_t NEW_IN_CW = parseGA(gaInCW);
  const uint16_t NEW_IN_H = parseGA(gaInH);
  const uint16_t NEW_IN_S = parseGA(gaInS);
  const uint16_t NEW_IN_V = parseGA(gaInV);
  const uint16_t NEW_IN_FX = parseGA(gaInFx);
  const uint16_t NEW_IN_PRE = parseGA(gaInPreset);
  const uint16_t NEW_IN_RGB = parseGA(gaInRGB);
  const uint16_t NEW_IN_HSV = parseGA(gaInHSV);
  const uint16_t NEW_IN_RGBW = parseGA(gaInRGBW);
  const uint16_t NEW_IN_RGB_REL = parseGA(gaInRGBRel);
  const uint16_t NEW_IN_HSV_REL = parseGA(gaInHSVRel);
  const uint16_t NEW_IN_RGBW_REL = parseGA(gaInRGBWRel);
  const uint16_t NEW_IN_BRI_REL = parseGA(gaInBriRel);
  const uint16_t NEW_IN_R_REL = parseGA(gaInRRel);
  const uint16_t NEW_IN_G_REL = parseGA(gaInGRel);
  const uint16_t NEW_IN_B_REL = parseGA(gaInBRel);
  const uint16_t NEW_IN_W_REL = parseGA(gaInWRel);
  const uint16_t NEW_IN_WW_REL = parseGA(gaInWWRel);
  const uint16_t NEW_IN_CW_REL = parseGA(gaInCWRel);
  const uint16_t NEW_IN_H_REL = parseGA(gaInHRel);
  const uint16_t NEW_IN_S_REL = parseGA(gaInSRel);
  const uint16_t NEW_IN_V_REL = parseGA(gaInVRel);
  const uint16_t NEW_IN_FX_REL = parseGA(gaInFxRel);

  const uint16_t NEW_OUT_PWR = parseGA(gaOutPower);
  const uint16_t NEW_OUT_BRI = parseGA(gaOutBri);
  const uint16_t NEW_OUT_R = parseGA(gaOutR);
  const uint16_t NEW_OUT_G = parseGA(gaOutG);
  const uint16_t NEW_OUT_B = parseGA(gaOutB);
  const uint16_t NEW_OUT_W = parseGA(gaOutW);
  const uint16_t NEW_OUT_CCT = parseGA(gaOutCct);
  const uint16_t NEW_OUT_WW = parseGA(gaOutWW);
  const uint16_t NEW_OUT_CW = parseGA(gaOutCW);
  const uint16_t NEW_OUT_H = parseGA(gaOutH);
  const uint16_t NEW_OUT_S = parseGA(gaOutS);
  const uint16_t NEW_OUT_V = parseGA(gaOutV);
  const uint16_t NEW_OUT_FX = parseGA(gaOutFx);
  const uint16_t NEW_OUT_PRE = parseGA(gaOutPreset);
  const uint16_t NEW_OUT_RGB = parseGA(gaOutRGB);
  const uint16_t NEW_OUT_HSV = parseGA(gaOutHSV);
  const uint16_t NEW_OUT_RGBW = parseGA(gaOutRGBW);

  // compute rebuild need (any GA mapping changed OR KNX just got enabled)
  bool anyGAChanged =
    (NEW_IN_PWR != PREV_IN_PWR) || (NEW_IN_BRI != PREV_IN_BRI) || (NEW_IN_R != PREV_IN_R) ||
    (NEW_IN_G != PREV_IN_G) || (NEW_IN_B != PREV_IN_B) || (NEW_IN_W != PREV_IN_W) ||
    (NEW_IN_CCT != PREV_IN_CCT) || (NEW_IN_WW != PREV_IN_WW) || (NEW_IN_CW != PREV_IN_CW) ||
    (NEW_IN_FX != PREV_IN_FX) || (NEW_IN_PRE != PREV_IN_PRE) ||
    (NEW_OUT_PWR != PREV_OUT_PWR) || (NEW_OUT_BRI != PREV_OUT_BRI) || (NEW_OUT_R != PREV_OUT_R) ||
    (NEW_OUT_G != PREV_OUT_G) || (NEW_OUT_B != PREV_OUT_B) || (NEW_OUT_W != PREV_OUT_W) ||
    (NEW_OUT_CCT != PREV_OUT_CCT) || (NEW_OUT_WW != PREV_OUT_WW) || (NEW_OUT_CW != PREV_OUT_CW) ||
    (NEW_OUT_FX != PREV_OUT_FX) || (NEW_OUT_PRE != PREV_OUT_PRE) ||
    (NEW_IN_RGB != PREV_IN_RGB) || (NEW_IN_HSV != PREV_IN_HSV) || (NEW_IN_RGBW != PREV_IN_RGBW) ||
    (NEW_OUT_RGB != PREV_OUT_RGB) || (NEW_OUT_HSV != PREV_OUT_HSV) || (NEW_OUT_RGBW != PREV_OUT_RGBW) ||
    (NEW_IN_H != PREV_IN_H) || (NEW_IN_S != PREV_IN_S) || (NEW_IN_V != PREV_IN_V) ||
    (NEW_OUT_H != PREV_OUT_H) || (NEW_OUT_S != PREV_OUT_S) || (NEW_OUT_V != PREV_OUT_V) ||
    (NEW_IN_BRI_REL != PREV_IN_BRI_REL) || (NEW_IN_R_REL != PREV_IN_R_REL) || (NEW_IN_G_REL != PREV_IN_G_REL) || (NEW_IN_B_REL != PREV_IN_B_REL) ||
    (NEW_IN_W_REL != PREV_IN_W_REL) || (NEW_IN_WW_REL != PREV_IN_WW_REL) || (NEW_IN_CW_REL != PREV_IN_CW_REL) ||
    (NEW_IN_H_REL != PREV_IN_H_REL) || (NEW_IN_S_REL != PREV_IN_S_REL) || (NEW_IN_V_REL != PREV_IN_V_REL) || (NEW_IN_FX_REL != PREV_IN_FX_REL) ||
    (NEW_IN_RGB_REL != PREV_IN_RGB_REL) || (NEW_IN_HSV_REL != PREV_IN_HSV_REL) || (NEW_IN_RGBW_REL != PREV_IN_RGBW_REL);

  bool prevEnabled = KNX.running(); // current runtime state is a good proxy here

  // now update the global caches with the NEW values
  GA_IN_PWR = NEW_IN_PWR;
  GA_IN_BRI = NEW_IN_BRI;
  GA_IN_R = NEW_IN_R;
  GA_IN_G = NEW_IN_G;
  GA_IN_B = NEW_IN_B;
  GA_IN_W = NEW_IN_W;
  GA_IN_CCT = NEW_IN_CCT;
  GA_IN_WW = NEW_IN_WW;
  GA_IN_CW = NEW_IN_CW;
  GA_IN_FX = NEW_IN_FX;
  GA_IN_PRE = NEW_IN_PRE;
  GA_IN_H = NEW_IN_H;
  GA_IN_S = NEW_IN_S;
  GA_IN_V = NEW_IN_V;
  GA_IN_RGB = NEW_IN_RGB;
  GA_IN_HSV = NEW_IN_HSV;
  GA_IN_RGBW = NEW_IN_RGBW;
  GA_IN_BRI_REL = NEW_IN_BRI_REL;
  GA_IN_R_REL = NEW_IN_R_REL;
  GA_IN_G_REL = NEW_IN_G_REL;
  GA_IN_B_REL = NEW_IN_B_REL;
  GA_IN_W_REL = NEW_IN_W_REL;
  GA_IN_WW_REL = NEW_IN_WW_REL;
  GA_IN_CW_REL = NEW_IN_CW_REL;
  GA_IN_H_REL = NEW_IN_H_REL;
  GA_IN_S_REL = NEW_IN_S_REL;
  GA_IN_V_REL = NEW_IN_V_REL;
  GA_IN_FX_REL = NEW_IN_FX_REL;
  GA_IN_RGB_REL = NEW_IN_RGB_REL;
  GA_IN_HSV_REL = NEW_IN_HSV_REL;
  GA_IN_RGBW_REL = NEW_IN_RGBW_REL;

  GA_OUT_PWR = NEW_OUT_PWR;
  GA_OUT_BRI = NEW_OUT_BRI;
  GA_OUT_R = NEW_OUT_R;
  GA_OUT_G = NEW_OUT_G;
  GA_OUT_B = NEW_OUT_B;
  GA_OUT_W = NEW_OUT_W;
  GA_OUT_CCT = NEW_OUT_CCT;
  GA_OUT_WW = NEW_OUT_WW;
  GA_OUT_CW = NEW_OUT_CW;
  GA_OUT_FX = NEW_OUT_FX;
  GA_OUT_PRE = NEW_OUT_PRE;
  GA_OUT_H = NEW_OUT_H;
  GA_OUT_S = NEW_OUT_S;
  GA_OUT_V = NEW_OUT_V;
  GA_OUT_RGB = NEW_OUT_RGB;
  GA_OUT_HSV = NEW_OUT_HSV;
  GA_OUT_RGBW = NEW_OUT_RGBW;

  // Re-apply enhancement to running core (safe to do regardless)
  KNX.setCommunicationEnhancement(commEnhance, commResends, commResendGapMs, commRxDedupMs);

  // ---- ENABLED/OFF handling ----
  if (!enabled) {
    // GUI disabled → leave multicast + free socket if running
    if (KNX.running()) {
      KNX_UM_DEBUGLN("[KNX-UM] KNX disabled via GUI → shutting down.");
      KNX.end(); // leaves group and closes socket
    }
    return true;
  }

  // If PA string is valid, update it without forcing a rebuild
  if (uint16_t pa = parsePA(individualAddr)) {
    KNX.setIndividualAddress(pa);
    KNX_UM_DEBUGF("[KNX-UM] PA set to %u.%u.%u (0x%04X)\n",
                  (unsigned)((pa >> 12) & 0x0F), (unsigned)((pa >> 8) & 0x0F), (unsigned)(pa & 0xFF), pa);
  } else {
    KNX_UM_WARNF("[KNX-UM][WARN] Invalid individual address '%s' (unchanged)\n", individualAddr);
  }

  // ---- rebuild vs. tweak ----
  const bool rebuildNeeded = anyGAChanged || !prevEnabled;

  if (rebuildNeeded) {
    KNX_UM_DEBUGLN("[KNX-UM] Rebuild KNX registrations & socket (GA map changed or first enable).");
    KNX.end();
    KNX.clearRegistrations(); // drop old GA registry
    setup();                  // re-register + begin() if Wi-Fi up
    KNX.setCommunicationEnhancement(commEnhance, commResends, commResendGapMs, commRxDedupMs);

    if (KNX.running()) {
      // Optional: send a primer read so routers learn our presence
      uint16_t primer = knxMakeGroupAddress(0, 0, 1);
      KNX.groupValueRead(primer);
    }
    // Push state so KNX sees the new mapping
    scheduleStatePublish();
  } else {
    // Lightweight path: keep socket, just refresh IGMP and tweak runtime
    if (KNX.running()) {
      if (!KNX.rejoinMulticast()) { // refresh IP_ADD_MEMBERSHIP + MULTICAST_IF
        KNX.end();
        KNX.begin();
      }
      // Optional primer
      uint16_t primer = knxMakeGroupAddress(0, 0, 1);
      KNX.groupValueRead(primer);
    } else {
      // Enabled but not running yet (e.g., Wi-Fi not ready) → try to start
      KNX.begin(); // joins multicast, sets TTL/LOOP/IF
    }
  }

  // Check for GA conflicts after configuration changes
  // This will set GUI error flag if conflicts are detected
  checkGAConflictsAndNotifyGUI();

  return true;
}

void KnxIpUsermod::addToJsonInfo(JsonObject& root) {
  if (!enabled) {
    KNX_UM_DEBUGF("[KNX-UM] addToJsonInfo called but usermod disabled\n");
    return;
  }

  KNX_UM_DEBUGF("[KNX-UM] addToJsonInfo called - adding usermod info to JSON\n");

  // Create usermod object if it doesn't exist
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");

  // Add basic KNX status info first (simpler test)
  JsonArray knxStatus = user.createNestedArray("KNX Status");
  if (KNX.running()) {
    knxStatus.add("Connected");
    KNX_UM_DEBUGF("[KNX-UM] Added KNX Status: Connected\n");
  } else {
    knxStatus.add("Disconnected");
    KNX_UM_DEBUGF("[KNX-UM] Added KNX Status: Disconnected\n");
  }

  // Add segment count for testing
  JsonArray segInfo = user.createNestedArray("KNX Segments");
  segInfo.add(strip.getSegmentsNum());
  segInfo.add(" segments");

  // Try adding the GA table as HTML
  String gaTable = getGATableHTML();
  if (gaTable.length() > 0 && gaTable.length() < 25000) { // increased limit for comprehensive GA table with all types
    user["KNX GA Table"] = gaTable;
    KNX_UM_DEBUGF("[KNX-UM] GA table added to JSON (%d chars)\n", gaTable.length());
  } else {
    KNX_UM_DEBUGF("[KNX-UM] GA table too large or empty (%d chars)\n", gaTable.length());
  }
}

void KnxIpUsermod::appendConfigData(Print& uiScript) {
  // Section shortcuts like in AudioReactive
  uiScript.print(F("ux='KNX_IP';"));
  uiScript.print(F("uxIn = ux+':GA in';"));
  uiScript.print(F("uxOut= ux+':GA out';"));

  // Compact layout with shorter inputs and aligned rows.
  uiScript.print(F(
    "(()=>{const css=`"
    "#knxip-card .knx-grid{display:grid;grid-template-columns:180px 1fr;gap:6px 12px;align-items:center}"
    "#knxip-card .knx-row{display:contents}"
    "#knxip-card input[type=text],#knxip-card input[type=number]{max-width:200px}"
    "#knxip-card .unit{margin-left:6px;opacity:.7;font-weight:400}"
    "`;let st=document.createElement('style');st.textContent=css;document.head.appendChild(st);})();"));

  // Add unit labels to the right of inputs (index 1 = the actual field), mirroring addInfo() usage in AudioReactive
  // ---- GA in ----
  uiScript.print(F("addInfo(uxIn+':power',1,' [-] (DPT 1.001)');"));
  uiScript.print(F("addInfo(uxIn+':bri',1,' [0..100] (DPT 5.001)');"));
  uiScript.print(F("addInfo(uxIn+':r',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':g',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':b',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':w',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':cct',1,' [Kelvin] (DPT 7.600)');"));
  uiScript.print(F("addInfo(uxIn+':ww',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':cw',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':h',1,' [0..255] (DPT 5.003)');"));
  uiScript.print(F("addInfo(uxIn+':s',1,' [0..100] (DPT 5.001)');")); // Saturation scaling (%)
  uiScript.print(F("addInfo(uxIn+':v',1,' [0..100] (DPT 5.001)');")); // Value scaling (%)
  uiScript.print(F("addInfo(uxIn+':fx',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':preset',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxIn+':rgb',1,' [0..255] (DPST-232-600)');"));
  uiScript.print(F("addInfo(uxIn+':rgbw',1,' [0..255] (DPST-251-600)');"));
  uiScript.print(F("addInfo(uxIn+':hsv',1,' [0..255] (DPST-232-600)');"));
  uiScript.print(F("addInfo(uxIn+':time',1,' [TimeOfDay,3Bytes](DPT 10.001)');"));
  uiScript.print(F("addInfo(uxIn+':date',1,' [Date,3Bytes] (DPT 11.001)');"));
  uiScript.print(F("addInfo(uxIn+':datetime',1,' [DateTime,8Bytes] (DPT 19.001)');"));
  uiScript.print(F("addInfo(uxIn+':bri_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':r_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':g_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':b_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':w_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':ww_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':cw_rel',1,' [step dir] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':h_rel',1,' [step hue] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':s_rel',1,' [step sat] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':v_rel',1,' [step val] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':fx_rel',1,' [step fx] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':rgb_rel',1,' [R,G,B,3Bytes] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':hsv_rel',1,' [H,S,V,3Bytes] (DPT 3.007)');"));
  uiScript.print(F("addInfo(uxIn+':rgbw_rel',1,' [R,G,B,W,4Bytes] (DPT 3.007)');"));

  // ---- GA out ----
  uiScript.print(F("addInfo(uxOut+':power',1,' [-]  (DPT 1.011)');"));
  uiScript.print(F("addInfo(uxOut+':bri',1,' [0..100] (DPT 5.001)');"));
  uiScript.print(F("addInfo(uxOut+':r',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':g',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':b',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':w',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':cct',1,' [Kelvin] (DPT 7.600)');"));
  uiScript.print(F("addInfo(uxOut+':ww',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':cw',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':h',1,' [0..255] (DPT 5.003)');"));
  uiScript.print(F("addInfo(uxOut+':s',1,' [0..100] (DPT 5.001)');"));
  uiScript.print(F("addInfo(uxOut+':v',1,' [0..100] (DPT 5.001)');"));
  uiScript.print(F("addInfo(uxOut+':fx',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':preset',1,' [0..255] (DPT 5.010)');"));
  uiScript.print(F("addInfo(uxOut+':rgb',1,' [0..255] (DPST-232-600)');"));
  uiScript.print(F("addInfo(uxOut+':rgbw',1,' [0..255] (DPST-251-600)');"));
  uiScript.print(F("addInfo(uxOut+':hsv',1,' [0..255] (DPST-232-600)');"));
  uiScript.print(F("addInfo(uxOut+':Internal_Temperature',1,' [°C] (DPST-14-68)');"));
  uiScript.print(F("addInfo(uxOut+':Temperature_Sensor',1,' [°C] (DPST-14-68)');"));
  uiScript.print(F("addInfo(uxOut+':Temperature_Sensor_Alarm',1,' [°C] (DPST-1-5)');"));
  uiScript.print(F("addInfo(uxOut+':Internal_Temperature_Alarm',1,' [°C] (DPST-1-5)');"));

  uiScript.print(F("addInfo(ux+':tx_rate_limit_ms',1,' [ms]');"));

  // ---- CCT range at top-level ----
  uiScript.print(F("addInfo(ux+':cct_kelvin_min',1,' [K]');"));
  uiScript.print(F("addInfo(ux+':cct_kelvin_max',1,' [K]');"));
  // Periodic interval units
  uiScript.print(F("addInfo(ux+':periodic_enabled',1,' [-]');"));
  uiScript.print(F("addInfo(ux+':periodic_interval_ms',1,' [ms]');"));
  // Color output mode
  uiScript.print(F("addInfo(ux+':color_out_mode',1,' [-] 0=single_values, 1=composite_only, 2=both');"));
  // Communication enhancement
  uiScript.print(F("addInfo(ux+':communication_enhancement',1,' [-]');"));
  uiScript.print(F("addInfo(ux+':communication_resends',1,' [-]');"));
  uiScript.print(F("addInfo(ux+':communication_resend_gap',1,' [ms]');"));
  uiScript.print(F("addInfo(ux+':communication_rx_dedup',1,' [ms]');"));

  uiScript.print(F("addInfo(ux+':Internal Temperature Alarm',1,' [°C]');"));
  uiScript.print(F("addInfo(ux+':Temperature Sensor Alarm',1,' [°C]');"));
  uiScript.print(F("addInfo(ux+':Temperature Alarm Hysteresis',1,' [°C]');"));

  // Tag the KNX card so CSS only scopes there
  uiScript.print(F(
    "(()=>{const card=[...document.querySelectorAll('.um')]"
    ".find(c=>{const h=c.querySelector('h3');return h&&h.textContent.trim()==='KNX_IP';});"
    "if(card) card.id='knxip-card';})();"));
}

static KnxIpUsermod KnxIp;
REGISTER_USERMOD(KnxIp);
