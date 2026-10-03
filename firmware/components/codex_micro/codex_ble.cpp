// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors
// Waveshare ESP32-S3-Touch-LCD-1.85B port copyright (c) 2026 Codex Micro port

#include "codex_ble.h"

#include <cstdio>
#include <cstring>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_efuse.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "ble";

// The quota snapshot is cached in NVS so a power cycle does not blank the dial.
//
// Without this the dial falls back to "--" / "NO QUOTA" on every boot until the
// companion happens to push a snapshot, which is the whole reason a freshly
// powered board looks broken for a minute or two. A restored snapshot is always
// presented as STALE: the board has no RTC, so it cannot know how long it was
// off, and the stored reset countdown is therefore unusable. The percentage is
// still the last value the host actually reported, which is what the dial is
// for. The first live write clears the restored flag and the value becomes
// fresh again.
constexpr char kQuotaNvsNamespace[] = "codex";
constexpr char kFiveHourNvsPercentKey[] = "q5_pct_x10";
constexpr char kFiveHourNvsResetKey[] = "q5_reset_s";
constexpr char kWeeklyNvsPercentKey[] = "qw_pct_x10";
constexpr char kWeeklyNvsResetKey[] = "qw_reset_s";
constexpr uint32_t kQuotaNvsMaxPercentX10 = 1000;

// Store the percentage as tenths in a u32 so 84.5% survives a round trip
// without depending on NVS float support.
void persistQuotaSnapshot(const quota_payload::Snapshot& snapshot) {
  nvs_handle_t handle = 0;
  if (nvs_open(kQuotaNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "quota snapshot not saved: nvs_open failed");
    return;
  }
  const auto percentX10 = [](float percent) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    return static_cast<uint32_t>(percent * 10.0f + 0.5f);
  };

  esp_err_t result = nvs_set_u32(handle, kFiveHourNvsPercentKey,
                                 percentX10(snapshot.fiveHourRemainingPercent));
  if (result == ESP_OK) {
    result = nvs_set_u32(handle, kFiveHourNvsResetKey,
                         snapshot.fiveHourResetInSeconds);
  }
  if (result == ESP_OK) {
    result = nvs_set_u32(handle, kWeeklyNvsPercentKey,
                         percentX10(snapshot.weeklyRemainingPercent));
  }
  if (result == ESP_OK) {
    result = nvs_set_u32(handle, kWeeklyNvsResetKey,
                         snapshot.weeklyResetInSeconds);
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }
  nvs_close(handle);
  if (result != ESP_OK) {
    // Not fatal: the dial simply goes back to waiting after the next boot.
    ESP_LOGW(kTag, "quota snapshot not saved: %s", esp_err_to_name(result));
  }
}

// Returns true when a usable snapshot was restored. The caller decides what to
// render; `restored` is left set so the dashboard can mark it stale.
bool loadQuotaSnapshot(QuotaState& quota) {
  nvs_handle_t handle = 0;
  if (nvs_open(kQuotaNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  uint32_t fiveHourPercentX10 = 0;
  uint32_t fiveHourResetInSeconds = 0;
  uint32_t weeklyPercentX10 = 0;
  uint32_t weeklyResetInSeconds = 0;
  const esp_err_t fiveHourPercentResult = nvs_get_u32(
      handle, kFiveHourNvsPercentKey, &fiveHourPercentX10);
  const esp_err_t fiveHourResetResult = nvs_get_u32(
      handle, kFiveHourNvsResetKey, &fiveHourResetInSeconds);
  const esp_err_t weeklyPercentResult = nvs_get_u32(
      handle, kWeeklyNvsPercentKey, &weeklyPercentX10);
  const esp_err_t weeklyResetResult = nvs_get_u32(
      handle, kWeeklyNvsResetKey, &weeklyResetInSeconds);
  nvs_close(handle);

  if (fiveHourPercentResult != ESP_OK || fiveHourResetResult != ESP_OK ||
      weeklyPercentResult != ESP_OK || weeklyResetResult != ESP_OK) {
    return false;
  }
  if (fiveHourPercentX10 > kQuotaNvsMaxPercentX10 ||
      weeklyPercentX10 > kQuotaNvsMaxPercentX10) {
    return false;
  }

  quota.fiveHourRemainingPercent =
      static_cast<float>(fiveHourPercentX10) / 10.0f;
  quota.fiveHourResetInSeconds = fiveHourResetInSeconds;
  quota.weeklyRemainingPercent =
      static_cast<float>(weeklyPercentX10) / 10.0f;
  quota.weeklyResetInSeconds = weeklyResetInSeconds;
  quota.receivedAtMs = 0;
  quota.available = true;
  quota.restored = true;
  return true;
}

// --------------------------------------------------------------- bond migration
//
// A BLE bond has two halves: the host indexes its half by the address it
// connected to, this board indexes its half by the peer address. When the two
// halves stop agreeing -- a reflash, an NVS erase, or an address change on
// either side -- the host tries to resume encryption with a key the board no
// longer has, or the board answers with a key the host does not have.
//
// Windows handles that disagreement badly. It keeps a record it reports as
// "not paired" while still refusing to start a fresh pairing, so the link
// comes up, never gets encrypted, and the host tears it down a second later.
// That is what breaks this device in practice: the HID report characteristics
// require link encryption, so an unencrypted link makes the desktop app's very
// first output-report write fail locally with ERROR_INVALID_PARAMETER (0x57)
// and it never gets a reply. The battery level is readable throughout because
// that characteristic is deliberately unencrypted -- which is exactly why
// "Bluetooth reads the battery fine but nothing works".
//
// Dropping this board's half does NOT restore service on its own. It was used
// repeatedly during bring-up (revisions 2 through 5) and every bump produced
// exactly the symptom it was meant to cure, because the host still holds the
// other half: Windows keeps an LTK the board can no longer answer for, and the
// device then sits in "connected with limited functionality" until Windows
// repairs the record on its own schedule -- measured at ~10 minutes on this
// machine, which is the user-visible "Bluetooth is connected but the app cannot
// control anything for ten minutes" bug.
//
// What actually clears that state is dropping the *host's* half. That is a
// host-side operation, and the project already ships the tool for it:
//
//     python windows_companion.py --device-address <addr> --repair-pairing
//
// `UnpairAsync` removes the Windows record and the stale LTK with it. The
// `pair` half of that command fails from a console process (Windows requires a
// UI owner for the ConfirmOnly consent), so the board is then paired once
// through Settings -> Bluetooth -> Add device. After that the bond is intact on
// both sides, the host resumes encryption at connection setup, and every later
// reboot or reconnect is immediately operable -- measured on 2026-09-23, where
// the desktop app completed its `v.oai.rgbcfg` handshake 175 ms after opening
// the HID node.
//
// So this migration is off by default. A routine reflash must never be able to
// destroy a working bond: doing so is what turned a one-time pairing into a
// recurring 10-minute outage. Set CODEX_BLE_BOND_REVISION to a value greater
// than the one already stored in NVS only as a deliberate last resort, and
// expect to re-pair afterwards.
// 6 is the one-time escape used on 2026-09-25 for the address 28:84:85:B2:1C:73,
// whose Windows record had degraded to "connects, encrypts, but never binds the
// HID-over-GATT driver": the host enumerates the battery and quota services on
// every reconnect and never touches the HID service, so no HID interface is ever
// created and Codex Desktop has no node to open. Combined with the new
// kBondGeneration below it leaves both halves clean for a fresh pairing. It is
// self-disabling: the revision is written to NVS, so it fires once per board.
#ifndef CODEX_BLE_BOND_REVISION
#define CODEX_BLE_BOND_REVISION 6
#endif

constexpr char kBondNvsKey[] = "bond_rev";
constexpr uint8_t kBondRevision = CODEX_BLE_BOND_REVISION;

void clearIncompatibleBondsOnce() {
  if (kBondRevision == 0) {
    // Disabled: see the rationale above. Kept as a function so the escape hatch
    // stays one macro away.
    return;
  }
  nvs_handle_t handle = 0;
  if (nvs_open(kQuotaNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "bond migration skipped: NVS unavailable");
    return;
  }

  // Always report the decision. A silent early-return here is indistinguishable
  // from "the migration is broken", which is exactly the ambiguity that made
  // this function hard to reason about during the 2026-09-25 bring-up.
  uint8_t storedRevision = 0;
  const esp_err_t read = nvs_get_u8(handle, kBondNvsKey, &storedRevision);
  ESP_LOGI(kTag, "bond migration: stored=%u requested=%u read=%s bonds=%d",
           static_cast<unsigned>(storedRevision),
           static_cast<unsigned>(kBondRevision), esp_err_to_name(read),
           esp_ble_get_bond_device_num());
  if (storedRevision >= kBondRevision) {
    nvs_close(handle);
    return;
  }

  int removed = 0;
  const int count = esp_ble_get_bond_device_num();
  if (count > 0) {
    auto* devices = new esp_ble_bond_dev_t[count];
    int listed = count;
    if (esp_ble_get_bond_device_list(&listed, devices) == ESP_OK) {
      for (int index = 0; index < listed; ++index) {
        if (esp_ble_remove_bond_device(devices[index].bd_addr) == ESP_OK) {
          ++removed;
        }
      }
    }
    delete[] devices;
  }

  nvs_set_u8(handle, kBondNvsKey, kBondRevision);
  nvs_commit(handle);
  nvs_close(handle);
  ESP_LOGW(kTag,
           "bond migration to revision %u: dropped %d stored bond(s); the host "
           "will pair again on the next connection",
           static_cast<unsigned>(kBondRevision), removed);
}

// ---------------------------------------------------- bond generation store --
//
// The Bluetooth address the board presents is derived from the factory MAC
// XORed with a "bond generation" byte (see esp_base_mac_addr_set() in begin()).
// Bumping that byte makes the host see a device it has never met, which is the
// only way out when a host holds a record it reports as unpaired while
// refusing to start a fresh pairing.
//
// It used to be a compile-time constant, which made the escape hatch reachable
// only by reflashing. It now lives in NVS so the BOOT key can trigger it at
// runtime -- see CodexMicroBle::resetBondGenerationAndRestart().
//
// The default is 0x5F, the address generation verified by the 2026-09-25
// recovery. The key only appears in NVS once the BOOT key is used, so existing
// boards retain an explicitly selected later generation across firmware updates.
constexpr char kBondGenerationNvsKey[] = "bond_gen";
constexpr uint8_t kDefaultBondGeneration = 0x5F;

uint8_t loadBondGeneration() {
  nvs_handle_t handle = 0;
  if (nvs_open(kQuotaNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return kDefaultBondGeneration;
  }
  uint8_t value = kDefaultBondGeneration;
  if (nvs_get_u8(handle, kBondGenerationNvsKey, &value) != ESP_OK) {
    value = kDefaultBondGeneration;
  }
  nvs_close(handle);
  return value;
}

bool storeBondGeneration(uint8_t value) {
  nvs_handle_t handle = 0;
  if (nvs_open(kQuotaNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(kTag, "bond generation not saved: nvs_open failed");
    return false;
  }
  esp_err_t result = nvs_set_u8(handle, kBondGenerationNvsKey, value);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "bond generation not saved: %s", esp_err_to_name(result));
    return false;
  }
  return true;
}

constexpr char kDeviceName[] = "Codex Micro";
constexpr char kManufacturer[] = "Work Louder";
constexpr char kFirmwareVersion[] = "1.1.0-waveshare-1.85b";
constexpr size_t kPayloadSize = 61;
constexpr size_t kReportBodySize = 63;

// HID over GATT has to run on an *encrypted* link, and that is a property of
// the host's HID class driver, not of the ATT permissions: Windows will happily
// open the BLE HID device node and then refuse to carry a single output report
// over a link that was never encrypted. Codex Desktop sees that as
// `hid_write/GetOverlappedResult` -> ERROR_INVALID_PARAMETER (0x57) followed by
// `hid_read_timeout` -> ERROR_DEVICE_NOT_CONNECTED (0x48F), marks the device
// "connected with limited functionality" (controlPlaneStatus=unavailable, i.e.
// "input remains available, but lighting and battery updates are temporarily
// unavailable") and only recovers minutes later, once Windows repairs the bond
// in the background on its own schedule.
//
// The encrypted access permissions are what makes Windows start that procedure
// promptly: a host whose bond is stale gets ATT "insufficient encryption" on
// its very first HID access, and that is the trigger that makes it re-pair.
// Declaring the characteristics plaintext removes the trigger, so such a host
// stays unencrypted forever instead of self-healing. This matches the upstream
// reference implementation, which declares the HID report characteristics
// ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED.
//
// Set to 0 only as a bring-up experiment; it is not a fix for a stale bond.
//
// Worth recording what this is NOT: on 2026-09-25 a host that had gone into
// the "reads battery and quota but never the HID handles" state was diagnosed
// as "the link is unencrypted", and turning this off was tried. It changed
// nothing -- the host kept reading exactly the same four handles. What actually
// fixed it was resetting the host's Bluetooth stack (scripts/windows/bt_radio_toggle.py).
// So the encrypted permissions were never the obstacle, and the symptom of a
// host that has stopped enumerating is *not* evidence about encryption.
#ifndef CODEX_BLE_REQUIRE_ENCRYPTION
#define CODEX_BLE_REQUIRE_ENCRYPTION 1
#endif

// Bring-up aid: log Bluetooth host/controller traffic (HCI/ATT/SMP) so a link
// that drops can be told apart from a host that simply went quiet. Needs
// CONFIG_LOG_MAXIMUM_LEVEL_DEBUG to compile the DEBUG statements in. Off by
// default because it is very verbose.
#ifndef CODEX_BLE_TRACE
#define CODEX_BLE_TRACE 0
#endif

#if CODEX_BLE_REQUIRE_ENCRYPTION
constexpr uint16_t kHidPermRead = ESP_GATT_PERM_READ_ENCRYPTED;
constexpr uint16_t kHidPermWrite = ESP_GATT_PERM_WRITE_ENCRYPTED;
#else
constexpr uint16_t kHidPermRead = ESP_GATT_PERM_READ;
constexpr uint16_t kHidPermWrite = ESP_GATT_PERM_WRITE;
#endif

// The quota characteristic is separate from the HID transport on purpose.
// Windows companions very often try to read the last quota back before (or
// instead of) writing a new one; a write-only characteristic answers that
// read with ATT_READ_NOT_PERMITTED, which is one of the ways a host ends up
// reporting the whole device as unreachable and giving up on the service.
// Quota data is not a credential, so it is readable, and encryption is opt-in
// here while HID stays encrypted by default.
#ifndef CODEX_BLE_QUOTA_REQUIRE_ENCRYPTION
#define CODEX_BLE_QUOTA_REQUIRE_ENCRYPTION 0
#endif

#if CODEX_BLE_QUOTA_REQUIRE_ENCRYPTION
constexpr uint16_t kQuotaPermRead = ESP_GATT_PERM_READ_ENCRYPTED;
constexpr uint16_t kQuotaPermWrite = ESP_GATT_PERM_WRITE_ENCRYPTED;
#else
constexpr uint16_t kQuotaPermRead = ESP_GATT_PERM_READ;
constexpr uint16_t kQuotaPermWrite = ESP_GATT_PERM_WRITE;
#endif

// One vendor-defined input/output report. HIDAPI adds/removes Report ID 6,
// while the BLE characteristics carry the remaining 63-byte report body.
uint8_t s_reportMap[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (1)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x06,        // Report ID (6)
    0x15, 0x00,        // Logical Minimum (0)
    0x26, 0xFF, 0x00,  // Logical Maximum (255)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x3F,        // Report Count (63)
    0x09, 0x01,        // Usage (1)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0x95, 0x3F,        // Report Count (63)
    0x09, 0x02,        // Usage (2)
    0x91, 0x02,        // Output (Data, Variable, Absolute)
    0xC0               // End Collection
};

uint8_t s_pnpId[] = {
    0x02,
    static_cast<uint8_t>(CodexMicroBle::kVendorId & 0xFF),
    static_cast<uint8_t>(CodexMicroBle::kVendorId >> 8),
    static_cast<uint8_t>(CodexMicroBle::kProductId & 0xFF),
    static_cast<uint8_t>(CodexMicroBle::kProductId >> 8),
    0x01,
    0x01,
};
uint8_t s_hidInfo[] = {0x11, 0x01, 0x00, 0x01};
uint8_t s_protocolMode[] = {0x01};
uint8_t s_inputReport[63] = {};
uint8_t s_outputReport[63] = {};
uint8_t s_hidControl[1] = {};
uint8_t s_quotaWrite[ESP_GATT_MAX_ATTR_LEN] = {};
uint8_t s_inputReportRef[] = {CodexMicroBle::kReportId, 0x01};
uint8_t s_outputReportRef[] = {CodexMicroBle::kReportId, 0x02};
uint8_t s_batteryLevel[] = {100};
uint8_t s_batteryFormat[] = {0x04, 0x00, 0xAD, 0x27, 0x01, 0x00, 0x00};
// The HID input-report CCCD, pre-armed for notifications for the same reason
// the battery one is (see below): Windows never writes this descriptor for a
// HID-over-GATT device, so leaving it at 0x0000 means every report we push
// through `sendJson()` is confirmed locally (ESP_GATTS_CONF_EVT status 0) and
// then dropped before it reaches the link layer. The host keeps polling
// `device.status` and never sees an answer, and BOOT/touch gestures never reach
// the Codex UI at all -- the device looks connected but cannot drive anything.
//
// Set CODEX_BLE_HID_CCCD_PREARMED to 0 to restore the old behaviour and wait
// for a host write that does not come.
#ifndef CODEX_BLE_HID_CCCD_PREARMED
#define CODEX_BLE_HID_CCCD_PREARMED 1
#endif

#if CODEX_BLE_HID_CCCD_PREARMED
uint8_t s_cccd[2] = {0x01, 0x00};
#else
uint8_t s_cccd[2] = {0x00, 0x00};
#endif
// The battery CCCD gets its own storage, pre-armed for notifications.
//
// Bluedroid consults the descriptor before it puts a notification on the air:
// with the value left at 0x0000 the keepalive we send once a second is
// acknowledged locally (ESP_GATTS_CONF_EVT / status 0) and then dropped, so
// nothing ever reaches the peer's link layer. The host's supervision timer
// then expires and the connection dies with reason 0x08 on a fixed cadence.
// Windows does not subscribe to the battery CCCD for a HID-only device, so
// arm it here instead of waiting for a write that never comes.
uint8_t s_batteryCccd[2] = {0x01, 0x00};

// Quota service: 7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01 (write ...5c02).
// Bluedroid stores 128-bit UUIDs little-endian, i.e. reversed from the
// canonical string form.
uint8_t s_quotaServiceUuid[16] = {0x01, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e,
                                  0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a,
                                  0x66, 0x4e, 0x0d, 0x7f};
uint8_t s_quotaWriteUuid[16] = {0x02, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e,
                                0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a,
                                0x66, 0x4e, 0x0d, 0x7f};

uint8_t s_primaryServiceUuid[2] = {0x00, 0x28};
uint8_t s_charDeclarationUuid[2] = {0x03, 0x28};
uint8_t s_cccdUuid[2] = {0x02, 0x29};
uint8_t s_reportRefUuid[2] = {0x08, 0x29};
uint8_t s_presentationFormatUuid[2] = {0x04, 0x29};

uint8_t s_deviceInfoService[2] = {0x0A, 0x18};
uint8_t s_hidService[2] = {0x12, 0x18};
uint8_t s_batteryService[2] = {0x0F, 0x18};

uint8_t s_charManufacturer[2] = {0x29, 0x2A};
uint8_t s_charPnpId[2] = {0x50, 0x2A};
uint8_t s_charHidInfo[2] = {0x4A, 0x2A};
uint8_t s_charReportMap[2] = {0x4B, 0x2A};
uint8_t s_charHidControl[2] = {0x4C, 0x2A};
uint8_t s_charProtocolMode[2] = {0x4E, 0x2A};
uint8_t s_charReport[2] = {0x4D, 0x2A};
uint8_t s_charBatteryLevel[2] = {0x19, 0x2A};

uint8_t s_propRead = ESP_GATT_CHAR_PROP_BIT_READ;
uint8_t s_propReadNotify =
    ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
uint8_t s_propWriteNr = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
uint8_t s_propReadWriteNr =
    ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
uint8_t s_propReadWriteWriteNr = ESP_GATT_CHAR_PROP_BIT_READ |
                                 ESP_GATT_CHAR_PROP_BIT_WRITE |
                                 ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

constexpr uint8_t kCharDeclarationSize = sizeof(uint8_t);

// Bluedroid's esp_ble_gatts_create_attr_tab() accepts exactly one primary
// service per table: btc_gatts_check_valid_attr_tab() rejects a second service
// entry with ESP_GATT_ERROR (0x85). Each service therefore gets its own table,
// registered one after another from ESP_GATTS_CREAT_ATTR_TAB_EVT.
enum DeviceInfoIndex {
  kDisService,
  kDisCharManufacturer,
  kDisValManufacturer,
  kDisCharPnpId,
  kDisValPnpId,
  kDisCount
};

enum HidIndex {
  kHidService,
  kHidCharInfo,
  kHidValInfo,
  kHidCharReportMap,
  kHidValReportMap,
  kHidCharControl,
  kHidValControl,
  kHidCharProtocolMode,
  kHidValProtocolMode,
  kHidCharInput,
  kHidValInput,
  kHidDescInputRef,
  kHidDescInputCccd,
  kHidCharOutput,
  kHidValOutput,
  kHidDescOutputRef,
  kHidCount
};

enum BatteryIndex {
  kBatService,
  kBatCharLevel,
  kBatValLevel,
  kBatDescFormat,
  kBatDescCccd,
  kBatCount
};

enum QuotaIndex {
  kQuotaService,
  kQuotaCharWrite,
  kQuotaValWrite,
  kQuotaCount
};

// Attribute entries are {uuid_length, uuid_p, perm, max_length, length, value}:
// max_length is the writable capacity and length the initial value size. Every
// entry with a non-zero `length` must point at a real buffer of at least that
// many bytes, because BTA_GATTS_AddCharacteristic() memcpy()s `length` bytes
// straight out of `value` on the Bluetooth task; a NULL `value` faults there.
esp_gatts_attr_db_t s_deviceInfoTable[kDisCount] = {
    // ---------------------------------------------------- Device Information
    [kDisService] = {{ESP_GATT_AUTO_RSP},
                               {ESP_UUID_LEN_16, s_primaryServiceUuid,
                                ESP_GATT_PERM_READ, sizeof(uint16_t),
                                sizeof(s_deviceInfoService),
                                s_deviceInfoService}},
    [kDisCharManufacturer] = {{ESP_GATT_AUTO_RSP},
                              {ESP_UUID_LEN_16, s_charDeclarationUuid,
                               ESP_GATT_PERM_READ, kCharDeclarationSize,
                               kCharDeclarationSize, &s_propRead}},
    [kDisValManufacturer] = {{ESP_GATT_AUTO_RSP},
                             {ESP_UUID_LEN_16, s_charManufacturer,
                              ESP_GATT_PERM_READ, 32,
                              sizeof(kManufacturer) - 1,
                              reinterpret_cast<uint8_t*>(
                                  const_cast<char*>(kManufacturer))}},
    [kDisCharPnpId] = {{ESP_GATT_AUTO_RSP},
                       {ESP_UUID_LEN_16, s_charDeclarationUuid,
                        ESP_GATT_PERM_READ, kCharDeclarationSize,
                        kCharDeclarationSize, &s_propRead}},
    [kDisValPnpId] = {{ESP_GATT_AUTO_RSP},
                      {ESP_UUID_LEN_16, s_charPnpId, ESP_GATT_PERM_READ,
                       sizeof(s_pnpId), sizeof(s_pnpId), s_pnpId}},

};

esp_gatts_attr_db_t s_hidTable[kHidCount] = {
    // ------------------------------------------------------ HID over GATT
    [kHidService] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_16, s_primaryServiceUuid,
                         ESP_GATT_PERM_READ, sizeof(uint16_t),
                         sizeof(s_hidService), s_hidService}},
    [kHidCharInfo] = {{ESP_GATT_AUTO_RSP},
                         {ESP_UUID_LEN_16, s_charDeclarationUuid,
                          ESP_GATT_PERM_READ, kCharDeclarationSize,
                          kCharDeclarationSize, &s_propRead}},
    [kHidValInfo] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_16, s_charHidInfo, ESP_GATT_PERM_READ,
                         sizeof(s_hidInfo), sizeof(s_hidInfo), s_hidInfo}},
    [kHidCharReportMap] = {{ESP_GATT_AUTO_RSP},
                           {ESP_UUID_LEN_16, s_charDeclarationUuid,
                            ESP_GATT_PERM_READ, kCharDeclarationSize,
                            kCharDeclarationSize, &s_propRead}},
    [kHidValReportMap] = {{ESP_GATT_AUTO_RSP},
                          {ESP_UUID_LEN_16, s_charReportMap,
                           ESP_GATT_PERM_READ, sizeof(s_reportMap),
                           sizeof(s_reportMap), s_reportMap}},
    [kHidCharControl] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_16, s_charDeclarationUuid,
                             ESP_GATT_PERM_READ, kCharDeclarationSize,
                             kCharDeclarationSize, &s_propWriteNr}},
    [kHidValControl] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_16, s_charHidControl,
                         ESP_GATT_PERM_WRITE, 1, 0, s_hidControl}},
    [kHidCharProtocolMode] = {{ESP_GATT_AUTO_RSP},
                              {ESP_UUID_LEN_16, s_charDeclarationUuid,
                               ESP_GATT_PERM_READ, kCharDeclarationSize,
                               kCharDeclarationSize, &s_propReadWriteNr}},
    [kHidValProtocolMode] = {{ESP_GATT_AUTO_RSP},
                             {ESP_UUID_LEN_16, s_charProtocolMode,
                              ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 1, 1,
                              s_protocolMode}},
    [kHidCharInput] = {{ESP_GATT_AUTO_RSP},
                             {ESP_UUID_LEN_16, s_charDeclarationUuid,
                              ESP_GATT_PERM_READ, kCharDeclarationSize,
                              kCharDeclarationSize, &s_propReadNotify}},
    [kHidValInput] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_16, s_charReport,
                             static_cast<uint16_t>(kHidPermRead |
                                                   ESP_GATT_PERM_WRITE),
                             sizeof(s_inputReport), sizeof(s_inputReport),
                             s_inputReport}},
    [kHidDescInputRef] = {{ESP_GATT_AUTO_RSP},
                                {ESP_UUID_LEN_16, s_reportRefUuid,
                                 ESP_GATT_PERM_READ, sizeof(s_inputReportRef),
                                 sizeof(s_inputReportRef), s_inputReportRef}},
    [kHidDescInputCccd] = {{ESP_GATT_AUTO_RSP},
                           {ESP_UUID_LEN_16, s_cccdUuid,
                            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                            sizeof(s_cccd), sizeof(s_cccd), s_cccd}},
    [kHidCharOutput] = {{ESP_GATT_AUTO_RSP},
                              {ESP_UUID_LEN_16, s_charDeclarationUuid,
                               ESP_GATT_PERM_READ, kCharDeclarationSize,
                               kCharDeclarationSize, &s_propReadWriteWriteNr}},
    [kHidValOutput] = {{ESP_GATT_AUTO_RSP},
                       {ESP_UUID_LEN_16, s_charReport,
                        static_cast<uint16_t>(kHidPermRead | kHidPermWrite),
                        sizeof(s_outputReport), 0, s_outputReport}},
    [kHidDescOutputRef] = {{ESP_GATT_AUTO_RSP},
                                 {ESP_UUID_LEN_16, s_reportRefUuid,
                                  ESP_GATT_PERM_READ,
                                  sizeof(s_outputReportRef),
                                  sizeof(s_outputReportRef),
                                  s_outputReportRef}},

};

esp_gatts_attr_db_t s_batteryTable[kBatCount] = {
    // -------------------------------------------------------- Battery Service
    [kBatService] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_16, s_primaryServiceUuid,
                             ESP_GATT_PERM_READ, sizeof(uint16_t),
                             sizeof(s_batteryService), s_batteryService}},
    [kBatCharLevel] = {{ESP_GATT_AUTO_RSP},
                              {ESP_UUID_LEN_16, s_charDeclarationUuid,
                               ESP_GATT_PERM_READ, kCharDeclarationSize,
                               kCharDeclarationSize, &s_propReadNotify}},
    [kBatValLevel] = {{ESP_GATT_AUTO_RSP},
                             {ESP_UUID_LEN_16, s_charBatteryLevel,
                              ESP_GATT_PERM_READ, 1, 1, s_batteryLevel}},
    [kBatDescFormat] = {{ESP_GATT_AUTO_RSP},
                               {ESP_UUID_LEN_16, s_presentationFormatUuid,
                                ESP_GATT_PERM_READ, sizeof(s_batteryFormat),
                                sizeof(s_batteryFormat), s_batteryFormat}},
    [kBatDescCccd] = {{ESP_GATT_AUTO_RSP},
                             {ESP_UUID_LEN_16, s_cccdUuid,
                              ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                              sizeof(s_batteryCccd), sizeof(s_batteryCccd),
                              s_batteryCccd}},

};

esp_gatts_attr_db_t s_quotaTable[kQuotaCount] = {
    // ------------------------------------------------------- Quota Service
    [kQuotaService] = {{ESP_GATT_AUTO_RSP},
                          {ESP_UUID_LEN_16, s_primaryServiceUuid,
                           ESP_GATT_PERM_READ, ESP_UUID_LEN_128,
                           ESP_UUID_LEN_128, s_quotaServiceUuid}},
    [kQuotaCharWrite] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_16, s_charDeclarationUuid,
                             ESP_GATT_PERM_READ, kCharDeclarationSize,
                             kCharDeclarationSize, &s_propReadWriteWriteNr}},
    // Readable as well as writable: the attribute keeps the last payload the
    // host pushed (auto-response attributes are updated by the stack on
    // write), so a companion can read the current quota back off the device.
    [kQuotaValWrite] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_128, s_quotaWriteUuid,
                         static_cast<uint16_t>(kQuotaPermRead | kQuotaPermWrite),
                         ESP_GATT_MAX_ATTR_LEN, 0, s_quotaWrite}},
};

// Registration order for the per-service attribute tables.
struct ServiceTable {
  esp_gatts_attr_db_t* attributes;
  uint16_t count;
  uint16_t serviceIndex;
};

const ServiceTable kServiceTables[] = {
    {s_deviceInfoTable, kDisCount, kDisService},
    {s_hidTable, kHidCount, kHidService},
    {s_batteryTable, kBatCount, kBatService},
    {s_quotaTable, kQuotaCount, kQuotaService},
};

constexpr size_t kServiceTableCount =
    sizeof(kServiceTables) / sizeof(kServiceTables[0]);

constexpr size_t kTableDeviceInfo = 0;
constexpr size_t kTableHid = 1;
constexpr size_t kTableBattery = 2;
constexpr size_t kTableQuota = 3;

// Legacy advertising payload, 31 bytes exactly:
//   flags | 16-bit service UUIDs | appearance | 128-bit quota service UUID
uint8_t s_advertisingData[] = {
    0x02, 0x01, 0x06,
    0x05, 0x02, 0x12, 0x18, 0x0F, 0x18,
    0x03, 0x19, 0xC0, 0x03,
    0x11, 0x07, 0x01, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e, 0xbe, 0xbf,
    0x71, 0x4a, 0xc2, 0x2a, 0x66, 0x4e, 0x0d, 0x7f,
};

uint8_t s_scanResponseData[] = {
    0x0C, 0x09, 'C', 'o', 'd', 'e', 'x', ' ', 'M', 'i', 'c', 'r', 'o',
};

CodexMicroBle* g_instance = nullptr;
std::atomic<esp_gatt_if_t> g_gattsInterface{ESP_GATT_IF_NONE};

// Connection ids are needed to address notifications. Bluedroid only reports
// them through GATTS events, so keep a small table here. The peer address is
// kept alongside because a disconnect (and therefore the pairing gesture that
// forces one) has to be addressed by address, not by connection id.
constexpr size_t kMaxConnections = 8;
uint16_t g_connectionIds[kMaxConnections] = {};
esp_bd_addr_t g_connectionAddrs[kMaxConnections] = {};
std::atomic<uint16_t> g_connectionCount{0};
portMUX_TYPE g_connectionsMux = portMUX_INITIALIZER_UNLOCKED;
struct ConnectionSnapshot {
  uint16_t count;
  uint16_t ids[kMaxConnections];
  esp_bd_addr_t addresses[kMaxConnections];
};
ConnectionSnapshot copyConnections() {
  ConnectionSnapshot copy = {};
  portENTER_CRITICAL(&g_connectionsMux);
  copy.count = g_connectionCount.load();
  memcpy(copy.ids, g_connectionIds, sizeof(copy.ids));
  memcpy(copy.addresses, g_connectionAddrs, sizeof(copy.addresses));
  portEXIT_CRITICAL(&g_connectionsMux);
  return copy;
}
class ConnectionsGuard {
 public:
  ConnectionsGuard() { portENTER_CRITICAL(&g_connectionsMux); }
  ~ConnectionsGuard() { portEXIT_CRITICAL(&g_connectionsMux); }
};

// Attribute handles are only known once the GATT table is registered, and the
// write callback has to dispatch on them.
uint16_t g_inputHandle = 0;
uint16_t g_outputHandle = 0;
uint16_t g_batteryHandle = 0;
uint16_t g_batteryCccdHandle = 0;
uint16_t g_hidCccdHandle = 0;
uint16_t g_quotaHandle = 0;
std::atomic<bool> g_advertising{false};
std::atomic<bool> g_advertisingStartPending{false};
std::atomic<bool> g_advertisingConfigured{false};

// The table currently being built, and the running attribute
// count, so the whole database can be reported once it is complete.
size_t g_serviceTableIndex = 0;
uint16_t g_serviceTableAttributeCount = 0;

// Bluedroid keeps one global "table under construction" slot, so the tables
// have to be created strictly one at a time: each completion event queues the
// next one.
void registerAttributeTable(esp_gatt_if_t gattsIf, size_t index) {
  if (index >= kServiceTableCount) {
    if (g_instance != nullptr) {
      g_instance->setReportHandles(g_inputHandle, g_outputHandle,
                                   g_batteryHandle, g_quotaHandle);
    }
    ESP_LOGI(kTag, "GATT database registered (%u services, %u attributes)",
             static_cast<unsigned>(kServiceTableCount),
             static_cast<unsigned>(g_serviceTableAttributeCount));
    return;
  }
  g_serviceTableIndex = index;
  esp_ble_gatts_create_attr_tab(kServiceTables[index].attributes, gattsIf,
                               kServiceTables[index].count, 0);
}

void rememberConnection(uint16_t connId, const uint8_t* address) {
  ConnectionsGuard guard;
  for (size_t i = 0; i < g_connectionCount; ++i) {
    if (g_connectionIds[i] == connId) {
      if (address != nullptr) memcpy(g_connectionAddrs[i], address, 6);
      return;
    }
  }
  if (g_connectionCount < kMaxConnections) {
    g_connectionIds[g_connectionCount] = connId;
    if (address != nullptr) {
      memcpy(g_connectionAddrs[g_connectionCount], address, 6);
    }
    ++g_connectionCount;
  }
}

void forgetConnection(uint16_t connId) {
  ConnectionsGuard guard;
  for (size_t i = 0; i < g_connectionCount; ++i) {
    if (g_connectionIds[i] == connId) {
      for (size_t j = i + 1; j < g_connectionCount; ++j) {
        g_connectionIds[j - 1] = g_connectionIds[j];
        memcpy(g_connectionAddrs[j - 1], g_connectionAddrs[j], 6);
      }
      --g_connectionCount;
      return;
    }
  }
}

void startAdvertising() {
  if (!g_instance || !g_instance->enabled() || !g_advertisingConfigured ||
      g_connectionCount > 0 || g_advertising || g_advertisingStartPending.exchange(true)) {
    return;
  }
  esp_ble_adv_params_t params = {
      .adv_int_min = 0x20,
      .adv_int_max = 0x40,
      .adv_type = ADV_TYPE_IND,
      .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
      .peer_addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
      .channel_map = ADV_CHNL_ALL,
      .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
  };
  const esp_err_t result = esp_ble_gap_start_advertising(&params);
  if (result != ESP_OK) {
    g_advertisingStartPending = false;
    ESP_LOGW(kTag, "advertising start request failed: %s",
             esp_err_to_name(result));
  }
}

// A 360x360 dashboard redraw costs tens of milliseconds, so the link has to
// tolerate the peripheral being briefly busy.
//
// Note: asking the host to change connection parameters is deliberately NOT
// done. Windows answers the L2CAP parameter update request with
// ESP_BT_STATUS_TIMEOUT, and the rejected exchange only adds churn; the host's
// own parameters (12 ms interval, 0 latency) are already what this device
// wants. The requested supervision timeout is left to the host as well, since
// a long timeout is what keeps a busy frame from being mistaken for a dead
// link.
void logConnectionParameters(const uint8_t* remoteBda) {
  ESP_LOGI(kTag, "connected peer %02x:%02x:%02x:%02x:%02x:%02x", remoteBda[0],
           remoteBda[1], remoteBda[2], remoteBda[3], remoteBda[4],
           remoteBda[5]);
}

void configureSecurity() {
  esp_ble_auth_req_t authRequest = ESP_LE_AUTH_BOND;
  esp_ble_io_cap_t ioCapability = ESP_IO_CAP_NONE;
  uint8_t keySize = 16;
  uint8_t initKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t responseKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &authRequest,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ioCapability,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &initKey,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &responseKey,
                                 sizeof(uint8_t));
}

void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      esp_ble_gap_config_scan_rsp_data_raw(s_scanResponseData,
                                           sizeof(s_scanResponseData));
      break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
      g_advertisingConfigured = param->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS;
      startAdvertising();
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      g_advertisingStartPending = false;
      if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        g_advertising = false;
        ESP_LOGW(kTag, "advertising start failed: %d",
                 param->adv_start_cmpl.status);
      } else if (g_connectionCount > 0 || !g_instance || !g_instance->enabled()) {
        g_advertising = false;
        esp_ble_gap_stop_advertising();
        ESP_LOGI(kTag, "advertising stopped; host already connected");
      } else {
        g_advertising = true;
        ESP_LOGI(kTag, "advertising as \"%s\"", kDeviceName);
      }
      if (g_instance != nullptr) g_instance->onAdvertisingState(g_advertising);
      break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      g_advertisingStartPending = false;
      g_advertising = false;
      if (g_instance != nullptr) g_instance->onAdvertisingState(false);
      startAdvertising();
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (param->ble_security.auth_cmpl.success) {
        // Report what the pairing actually produced, not just that it ended:
        // auth_mode carries the SC/MITM/bond bits, and key_present tells us
        // whether keys were really exchanged. A "successful" pairing that
        // leaves the link unencrypted is the failure this log exists to make
        // visible -- see CODEX_BLE_REQUIRE_ENCRYPTION above.
        ESP_LOGI(kTag,
                 "pairing complete auth_mode=0x%02x key_type=0x%02x "
                 "key_present=%d addr_type=%d bonds=%d",
                 param->ble_security.auth_cmpl.auth_mode,
                 param->ble_security.auth_cmpl.key_type,
                 param->ble_security.auth_cmpl.key_present ? 1 : 0,
                 param->ble_security.auth_cmpl.addr_type,
                 esp_ble_get_bond_device_num());
      } else {
        ESP_LOGW(kTag, "pairing failed reason=0x%x",
                 param->ble_security.auth_cmpl.fail_reason);
      }
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      ESP_LOGI(kTag, "passkey %06u",
               static_cast<unsigned>(param->ble_security.key_notif.passkey));
      break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      // Should not happen: this board declares NoInputNoOutput, so the SMP
      // association model is Just Works. If a host asks anyway, answer it
      // instead of staying silent -- an unanswered request is exactly what
      // reads as "passkey entry failed" on the peer.
      ESP_LOGW(kTag, "host requested a passkey; answering with the fixed one");
      esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, 0);
      break;
    case ESP_GAP_BLE_KEY_EVT:
      ESP_LOGI(kTag, "host key type=0x%02x",
               param->ble_security.ble_key.key_type);
      break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      ESP_LOGI(kTag,
               "link params interval=%ums latency=%u timeout=%ums status=%d",
               static_cast<unsigned>(param->update_conn_params.conn_int),
               static_cast<unsigned>(param->update_conn_params.latency),
               static_cast<unsigned>(param->update_conn_params.timeout * 10),
               param->update_conn_params.status);
      break;
    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
      break;
    default:
      break;
  }
}

void gattsCallback(esp_gatts_cb_event_t event, esp_gatt_if_t gattsIf,
                   esp_ble_gatts_cb_param_t* param) {
  switch (event) {
    case ESP_GATTS_REG_EVT:
      g_gattsInterface = gattsIf;
      esp_ble_gap_set_device_name(kDeviceName);
      esp_ble_gap_config_adv_data_raw(s_advertisingData,
                                      sizeof(s_advertisingData));
      registerAttributeTable(gattsIf, kTableDeviceInfo);
      break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
      const size_t index = g_serviceTableIndex;
      if (param->add_attr_tab.status != ESP_GATT_OK) {
        ESP_LOGE(kTag, "attribute table %u failed: 0x%x",
                 static_cast<unsigned>(index), param->add_attr_tab.status);
        registerAttributeTable(gattsIf, index + 1);
        break;
      }
      g_serviceTableAttributeCount += kServiceTables[index].count;
      const uint16_t* handles = param->add_attr_tab.handles;
      switch (index) {
        case kTableHid:
          g_inputHandle = handles[kHidValInput];
          g_outputHandle = handles[kHidValOutput];
          g_hidCccdHandle = handles[kHidDescInputCccd];
          break;
        case kTableBattery:
          g_batteryHandle = handles[kBatValLevel];
          g_batteryCccdHandle = handles[kBatDescCccd];
          break;
        case kTableQuota:
          g_quotaHandle = handles[kQuotaValWrite];
          break;
        default:
          break;
      }
      const esp_err_t start = esp_ble_gatts_start_service(
          handles[kServiceTables[index].serviceIndex]);
      if (start != ESP_OK) {
        ESP_LOGW(kTag, "start service %u failed: %s",
                 static_cast<unsigned>(index), esp_err_to_name(start));
      }
      ESP_LOGI(kTag, "service %u registered (%u attributes)",
               static_cast<unsigned>(index), kServiceTables[index].count);
      registerAttributeTable(gattsIf, index + 1);
      break;
    }

    case ESP_GATTS_CONNECT_EVT: {
      rememberConnection(param->connect.conn_id, param->connect.remote_bda);
      g_advertising = false;
      g_advertisingStartPending = false;
      if (g_instance != nullptr) {
        g_instance->onAdvertisingState(false);
        g_instance->onConnectionEvent(true, param->connect.conn_id);
      }
      logConnectionParameters(param->connect.remote_bda);
      // Do NOT send an SMP Security Request here. Measured, both ways.
      //
      // With a healthy bond the host does not need to be asked: Windows
      // initiates link-layer encryption from the stored LTK as soon as the ACL
      // link is up, so the link is already encrypted before the desktop app can
      // open the HID node.
      //
      // With a stale host bond the request is fatal. Measured on 2026-09-25
      // with the board advertising an address whose Windows record held an LTK
      // this board no longer has (its half had been dropped by a bond-revision
      // migration):
      //
      //   I (5011) ble: connected peer e0:0a:f6:80:71:d2
      //   I (5011) ble: security requested id=0
      //   I (5011) ble: host connected id=0 bonds=0
      //   W (5026) BT_APPL: bta_dm_ble_smp_cback remove bond,rsn 102, BDA:0xE00AF68071D2
      //   E (5027) BT_BTM: Device not found
      //   W (5027) BT_HCI: hcif disc complete: hdl 0x1, rsn 0x13
      //   I (5028) ble: host disconnected id=0 reason=0x13
      //   W (5029) ble: pairing failed reason=0x66
      //
      // 16 ms from connect to disconnect, and this repeats indefinitely. The
      // host never even answers the request (`rsn 102` is
      // ESP_AUTH_SMP_CONN_TOUT); it just drops the link. Leaving the host to
      // drive security instead keeps the link alive, which is what lets Windows
      // eventually repair its own record.
      //
      // As a peripheral the local role is always BTM_ROLE_SLAVE, so
      // btm_ble_set_encryption() cannot shortcut this either: in
      // btm_ble_set_encryption() the BTM_BLE_SEC_ENCRYPT case only starts
      // link-layer encryption when the role is master, and every other case
      // falls through to SMP_Pair(). Both ESP_BLE_SEC_ENCRYPT and
      // ESP_BLE_SEC_ENCRYPT_NO_MITM therefore end up sending the same request.
      //
      // Set CODEX_BLE_SECURITY_ON_CONNECT to 1 only as a bring-up experiment.
      // The upstream reference implementation never calls this API; encryption
      // is driven by the host, which is what the ESP_GATT_PERM_*_ENCRYPTED
      // permissions on the report characteristics are for.
#ifndef CODEX_BLE_SECURITY_ON_CONNECT
#define CODEX_BLE_SECURITY_ON_CONNECT 0
#endif
#if CODEX_BLE_SECURITY_ON_CONNECT
      const esp_err_t security =
          esp_ble_set_encryption(param->connect.remote_bda,
                                 ESP_BLE_SEC_ENCRYPT_NO_MITM);
      if (security != ESP_OK) {
        ESP_LOGW(kTag, "security request failed id=%u: %s",
                 param->connect.conn_id, esp_err_to_name(security));
      } else {
        ESP_LOGI(kTag, "security requested id=%u", param->connect.conn_id);
      }
#endif
      // bonds=0 means this board has no half of the pairing while the host may
      // still hold one -- the exact state that produces the symptom above.
      if (g_instance && !g_instance->enabled()) esp_ble_gap_disconnect(param->connect.remote_bda);
      ESP_LOGI(kTag, "host connected id=%u bonds=%d", param->connect.conn_id,
               esp_ble_get_bond_device_num());
      break;
    }

    // Service discovery shows up here; without this the link can look idle
    // even though the host is walking the attribute table.
    case ESP_GATTS_READ_EVT:
      ESP_LOGI(kTag, "read id=%u handle=%u offset=%u long=%d", param->read.conn_id,
               param->read.handle, param->read.offset,
               param->read.is_long ? 1 : 0);
      break;

    case ESP_GATTS_EXEC_WRITE_EVT:
      ESP_LOGI(kTag, "exec write id=%u flag=%u", param->exec_write.conn_id,
               param->exec_write.exec_write_flag);
      break;

    case ESP_GATTS_DISCONNECT_EVT:
      forgetConnection(param->disconnect.conn_id);
      if (g_instance != nullptr) {
        g_instance->onConnectionEvent(false, param->disconnect.conn_id);
      }
      startAdvertising();
      ESP_LOGI(kTag, "host disconnected id=%u reason=0x%x",
               param->disconnect.conn_id, param->disconnect.reason);
      break;

    case ESP_GATTS_MTU_EVT:
      ESP_LOGI(kTag, "mtu id=%u value=%u", param->mtu.conn_id,
               param->mtu.mtu);
      break;

    case ESP_GATTS_WRITE_EVT: {
      if (g_instance == nullptr || param->write.is_prep) break;
      const uint16_t handle = param->write.handle;
      if (handle == g_outputHandle && g_outputHandle != 0) {
        g_instance->onOutput(param->write.value, param->write.len,
                             param->write.conn_id, param->write.bda);
      } else if (handle == g_quotaHandle && g_quotaHandle != 0) {
        g_instance->onQuotaWrite(param->write.value, param->write.len,
                                 param->write.need_rsp, param->write.bda);
      } else if (handle == g_batteryCccdHandle && g_batteryCccdHandle != 0) {
        ESP_LOGI(kTag, "battery cccd id=%u value=0x%02x%02x len=%u",
                 param->write.conn_id,
                 param->write.len > 1 ? param->write.value[1] : 0,
                 param->write.len > 0 ? param->write.value[0] : 0,
                 param->write.len);
      } else if (handle == g_hidCccdHandle && g_hidCccdHandle != 0) {
        ESP_LOGI(kTag, "hid cccd id=%u value=0x%02x%02x len=%u",
                 param->write.conn_id,
                 param->write.len > 1 ? param->write.value[1] : 0,
                 param->write.len > 0 ? param->write.value[0] : 0,
                 param->write.len);
      } else {
        ESP_LOGI(kTag, "write id=%u handle=%u len=%u (unhandled)",
                 param->write.conn_id, handle, param->write.len);
      }
      if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gattsIf, param->write.conn_id,
                                    param->write.trans_id, ESP_GATT_OK, nullptr);
      }
      break;
    }

    case ESP_GATTS_CONF_EVT:
      ESP_LOGI(kTag, "gatt conf id=%u handle=%u status=%u",
               param->conf.conn_id, param->conf.handle, param->conf.status);
      break;

    case ESP_GATTS_DELETE_EVT:
      break;

    default:
      break;
  }
}

}  // namespace

esp_err_t CodexMicroBle::begin(bool enabled) {
  g_instance = this;
  enabled_.store(enabled);

  stateMutex_ = xSemaphoreCreateMutex();
  outputQueue_ = xQueueCreate(16, sizeof(PendingOutputReport));
  connectionEventQueue_ = xQueueCreate(16, sizeof(PendingConnectionEvent));
  quotaWriteQueue_ = xQueueCreate(4, sizeof(PendingQuotaWrite));
  if (stateMutex_ == nullptr || outputQueue_ == nullptr ||
      connectionEventQueue_ == nullptr || quotaWriteQueue_ == nullptr) {
    ESP_LOGE(kTag, "BLE queue allocation failed");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = nvs_flash_init();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(result));
    return result;
  }

  // Bring the last known quota back before the first frame is drawn, so a
  // freshly powered board shows the previous value (marked stale) instead of a
  // blank dial while it waits for the companion.
  if (loadQuotaSnapshot(state_.quota)) {
    ESP_LOGI(kTag,
             "quota snapshot restored from NVS: 5h=%.1f weekly=%.1f (shown as stale "
             "until a companion pushes a fresh value)",
             state_.quota.fiveHourRemainingPercent,
             state_.quota.weeklyRemainingPercent);
  } else {
    ESP_LOGI(kTag, "no stored quota snapshot; dial waits for the companion");
  }

  // BLE only: release the classic Bluetooth memory the controller reserves.
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  // Change the Bluetooth interface only; the launcher owns the Wi-Fi identity.
  const uint8_t generation = loadBondGeneration();
  uint8_t factoryMac[6] = {};
  ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(factoryMac), kTag, "MAC read failed");
  factoryMac[5] = static_cast<uint8_t>((factoryMac[5] ^ generation) + 2);
  ESP_RETURN_ON_ERROR(esp_iface_mac_addr_set(factoryMac, ESP_MAC_BT), kTag, "BT MAC failed");

  esp_bt_controller_config_t controllerConfig =
      BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  result = esp_bt_controller_init(&controllerConfig);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "controller init failed: %s", esp_err_to_name(result));
    return result;
  }
  result = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "controller enable failed: %s", esp_err_to_name(result));
    return result;
  }
  result = esp_bluedroid_init();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "bluedroid init failed: %s", esp_err_to_name(result));
    return result;
  }
  result = esp_bluedroid_enable();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "bluedroid enable failed: %s", esp_err_to_name(result));
    return result;
  }

  ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(gattsCallback), kTag, "GATTS callback failed");
  ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gapCallback), kTag, "GAP callback failed");
  // Must run before anything can connect: a stale bond is the difference
  // between a link the desktop app can write to and one it cannot.
  clearIncompatibleBondsOnce();
  configureSecurity();
  esp_ble_gatt_set_local_mtu(517);

#if CODEX_BLE_TRACE
  // Bring-up aid: surface host/controller traffic (HCI, ATT, SMP) so a link
  // that drops can be told apart from a host that went quiet.
  esp_log_level_set("BT_HCI", ESP_LOG_DEBUG);
  esp_log_level_set("BT_BTM", ESP_LOG_DEBUG);
  esp_log_level_set("BT_GATTS", ESP_LOG_DEBUG);
  esp_log_level_set("BT_GATTC", ESP_LOG_DEBUG);
  esp_log_level_set("BT_L2CAP", ESP_LOG_DEBUG);
  esp_log_level_set("BT_SMP", ESP_LOG_DEBUG);
  ESP_LOGW(kTag, "BLE trace logging enabled");
#endif

  result = esp_ble_gatts_app_register(0);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "gatts app register failed: %s", esp_err_to_name(result));
    return result;
  }

  ESP_LOGI(kTag,
           "vendor HID starting VID=%04X PID=%04X usage=FF00 report=%u",
           kVendorId, kProductId, kReportId);
  return ESP_OK;
}

esp_err_t CodexMicroBle::setEnabled(bool enabled) {
  enabled_.store(enabled);
  if (enabled) {
    startAdvertising();
    return ESP_OK;
  }
  esp_err_t error = ESP_OK;
  if (g_advertising || g_advertisingStartPending) {
    error = esp_ble_gap_stop_advertising();
  }
  const auto peers = copyConnections();
  for (uint16_t i = 0; i < peers.count; ++i) {
    const esp_err_t result = esp_ble_gap_disconnect(const_cast<uint8_t*>(peers.addresses[i]));
    if (error == ESP_OK) error = result;
  }
  return error;
}

void CodexMicroBle::setReportHandles(uint16_t input, uint16_t output,
                                     uint16_t battery, uint16_t quota) {
  inputHandle_ = input;
  outputHandle_ = output;
  batteryHandle_ = battery;
  quotaHandle_ = quota;
  g_inputHandle = input;
  g_outputHandle = output;
  g_batteryHandle = battery;
  g_quotaHandle = quota;
  ESP_LOGI(kTag, "handles input=%u output=%u battery=%u quota=%u", input,
           output, battery, quota);
}

void CodexMicroBle::markDisconnected(uint16_t connId) {
  forgetConnection(connId);
}

int CodexMicroBle::enterPairingMode() {
  // 1. Drop the host link first. A bond cannot be removed cleanly while its
  //    owner is still using it, and the disconnect is also what makes the host
  //    throw away the GATT handle cache it built for this connection -- which
  //    is the other half of why a stale session misbehaves (see README 6.11).
  const auto peers = copyConnections();
  const uint16_t links = peers.count;
  for (uint16_t i = 0; i < peers.count; ++i) {
    esp_ble_gap_disconnect(const_cast<uint8_t*>(peers.addresses[i]));
  }

  // 2. Throw away this board's half of every bond. This is the actual cure:
  //    with no local LTK, the host's next attempt to resume encryption is
  //    answered with "PIN or key missing", which is the signal Windows needs
  //    to drop its own stale record and start a fresh Just Works pairing.
  int removed = 0;
  const int count = esp_ble_get_bond_device_num();
  if (count > 0) {
    auto* devices = new esp_ble_bond_dev_t[count];
    int listed = count;
    if (esp_ble_get_bond_device_list(&listed, devices) == ESP_OK) {
      for (int index = 0; index < listed; ++index) {
        if (esp_ble_remove_bond_device(devices[index].bd_addr) == ESP_OK) {
          ++removed;
        }
      }
    }
    delete[] devices;
  }

  // 3. The next connection is a new session, not a continuation of the old
  //    one. Without this the dashboard would keep reporting CODEX LIVE for a
  //    host that has just been forgotten.
  if (stateMutex_ != nullptr) xSemaphoreTake(stateMutex_, portMAX_DELAY);
  clearHostRpcIdentity();
  state_.connected = false;
  state_.hostRpcObserved = false;
  state_.lastHostRpcAtMs = 0;
  state_.dirty = true;
  if (stateMutex_ != nullptr) xSemaphoreGive(stateMutex_);
  rpcLength_ = 0;
  rpcBufferConnectionValid_ = false;

  // 4. Be pairable again. If a link was up, its disconnect event restarts
  //    advertising a moment later; this covers the case where there was no
  //    link to drop, or where that event has not been processed yet.
  startAdvertising();

  ESP_LOGW(kTag,
           "pairing mode: dropped %d bond(s), signalled %u link(s), "
           "advertising=%d",
           removed, static_cast<unsigned>(links), g_advertising ? 1 : 0);
  return removed;
}

bool CodexMicroBle::resetBondGenerationAndRestart() {
  const uint8_t current = loadBondGeneration();
  const uint8_t next = static_cast<uint8_t>(current + 1);
  if (!storeBondGeneration(next)) return false;

  // Drop the bonds as well: the new address means the host has to pair from
  // scratch anyway, and a bond it can never use again is worse than none.
  const int removed = enterPairingMode();
  ESP_LOGW(kTag,
           "bond generation 0x%02X -> 0x%02X (%d bond(s) dropped); restarting "
           "to advertise an address the host has never seen",
           current, next, removed);

  // Let the disconnect and this log reach the wire before the reset, so a
  // serial capture records what actually happened.
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return true;  // Not reached.
}

void CodexMicroBle::poll() {
  processConnectionEvents();
  reconcileConnectionSet();
  processQuotaWrites();

  if (outputQueue_ == nullptr) return;
  PendingOutputReport report;
  while (xQueueReceive(outputQueue_, &report, 0) == pdTRUE) {
    if (report.length == 0) {
      rpcLength_ = 0;
      continue;
    }
    processOutput(report.data.data(), report.length, report.connectionId,
                  report.peerAddress);
  }
}

void CodexMicroBle::setBattery(uint8_t percentage, bool charging) {
  const auto peers = copyConnections();
  batteryPercentage_ = percentage > 100 ? 100 : percentage;
  charging_ = charging;
  s_batteryLevel[0] = batteryPercentage_;
  if (batteryHandle_ == 0) return;
  esp_ble_gatts_set_attr_value(batteryHandle_, 1, s_batteryLevel);
  // Notifications are addressed by connection id, so drive the loop off the
  // id table rather than the debounced `connected()` flag: that flag lags the
  // table by one poll(), and a dropped notification is a dropped keepalive,
  // which is how an idle link gets torn down on its supervision timeout.
  uint16_t sent = 0;
  for (uint16_t i = 0; i < peers.count; ++i) {
    const esp_err_t result = esp_ble_gatts_send_indicate(
        g_gattsInterface, peers.ids[i], batteryHandle_, 1,
        s_batteryLevel, false);
    if (result != ESP_OK) {
      ESP_LOGW(kTag, "battery notify failed id=%u: %s", peers.ids[i],
               esp_err_to_name(result));
    } else {
      ++sent;
    }
  }
  ESP_LOGD(kTag, "battery tick level=%u sent=%u conns=%u", batteryPercentage_,
           sent, static_cast<unsigned>(peers.count));
}

void CodexMicroBle::sendKey(const char* key, uint8_t action, int8_t agent) {
  cJSON* message = cJSON_CreateObject();
  if (message == nullptr) return;
  cJSON_AddStringToObject(message, "method", "v.oai.hid");
  cJSON* params = cJSON_AddObjectToObject(message, "params");
  cJSON_AddStringToObject(params, "k", key);
  cJSON_AddNumberToObject(params, "act", action);
  if (agent >= 0) cJSON_AddNumberToObject(params, "ag", agent);

  char* json = cJSON_PrintUnformatted(message);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(message);
  ESP_LOGI(kTag, "HID key=%s action=%u", key, action);
}

void CodexMicroBle::sendJoystick(float angle, float distance) {
  cJSON* message = cJSON_CreateObject();
  if (message == nullptr) return;
  cJSON_AddStringToObject(message, "method", "v.oai.rad");
  cJSON* params = cJSON_AddObjectToObject(message, "params");
  cJSON_AddNumberToObject(params, "a", angle);
  cJSON_AddNumberToObject(params, "d", distance);

  char* json = cJSON_PrintUnformatted(message);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(message);
}

bool CodexMicroBle::connected() {
  if (stateMutex_ == nullptr) return false;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool result = state_.connected;
  xSemaphoreGive(stateMutex_);
  return result;
}

CodexMicroState CodexMicroBle::snapshot() {
  CodexMicroState copy;
  if (stateMutex_ == nullptr) return copy;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  copy = state_;
  state_.dirty = false;
  xSemaphoreGive(stateMutex_);
  return copy;
}

void CodexMicroBle::onConnectionEvent(bool connected, uint16_t id) {
  if (connectionEventQueue_ == nullptr) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }
  PendingConnectionEvent event;
  event.id = id;
  event.connected = connected;
  if (xQueueSend(connectionEventQueue_, &event, 0) != pdTRUE) {
    connectionEventLost_.store(true, std::memory_order_release);
  }
}

void CodexMicroBle::applyConnectionEvent(const PendingConnectionEvent& event) {
  const connection_health::ConnectionSet::Transition transition =
      connections_.apply(event.connected, event.id);
  if (transition.overflow) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }
  if (!transition.changed || stateMutex_ == nullptr) return;

  const uint8_t count = static_cast<uint8_t>(connections_.count());

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool hostDisconnected = !event.connected && hostRpcConnectionValid_ &&
                                hostRpcConnectionId_ == event.id;
  if (transition.becameConnected) {
    ++state_.connectionEpoch;
    state_.lastHostRpcAtMs = 0;
    state_.hostRpcObserved = false;
    clearHostRpcIdentity();
  } else if (transition.becameDisconnected || hostDisconnected) {
    state_.lastHostRpcAtMs = 0;
    state_.hostRpcObserved = false;
    clearHostRpcIdentity();
  }
  state_.connected = count > 0;
  state_.dirty = true;
  const uint32_t connectionEpoch = state_.connectionEpoch;
  xSemaphoreGive(stateMutex_);

  if (transition.becameConnected || transition.becameDisconnected ||
      (!event.connected && rpcBufferConnectionValid_ &&
       rpcBufferConnectionId_ == event.id)) {
    rpcLength_ = 0;
    rpcBufferConnectionValid_ = false;
  }
  ESP_LOGI(kTag, "host event=%s id=%u count=%u epoch=%lu",
           event.connected ? "connected" : "disconnected", event.id, count,
           static_cast<unsigned long>(connectionEpoch));
}

void CodexMicroBle::processConnectionEvents() {
  if (connectionEventQueue_ == nullptr) return;
  PendingConnectionEvent event;
  while (xQueueReceive(connectionEventQueue_, &event, 0) == pdTRUE) {
    applyConnectionEvent(event);
  }
}

void CodexMicroBle::reconcileConnectionSet() {
  const auto peers = copyConnections();
  const bool eventLost =
      connectionEventLost_.exchange(false, std::memory_order_acq_rel);
  if (!eventLost || stateMutex_ == nullptr) return;

  // At least one ordered event was lost. Fail closed even if final membership
  // happens to match: an unseen empty->connected cycle must never inherit the
  // old session's CODEX LIVE state.
  connection_health::ConnectionSet observed;
  bool overflow = false;
  for (uint16_t i = 0; i < peers.count; ++i) {
    if (observed.apply(true, peers.ids[i]).overflow) overflow = true;
  }
  if (overflow) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }

  connections_ = observed;
  const uint8_t count = static_cast<uint8_t>(connections_.count());
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (count > 0) ++state_.connectionEpoch;
  state_.connected = count > 0;
  state_.lastHostRpcAtMs = 0;
  state_.hostRpcObserved = false;
  clearHostRpcIdentity();
  state_.dirty = true;
  const uint32_t epoch = state_.connectionEpoch;
  xSemaphoreGive(stateMutex_);
  rpcLength_ = 0;
  rpcBufferConnectionValid_ = false;
  ESP_LOGI(kTag, "connections reconciled count=%u epoch=%lu", count,
           static_cast<unsigned long>(epoch));
}

void CodexMicroBle::clearHostRpcIdentity() {
  hostRpcConnectionValid_ = false;
}

void CodexMicroBle::noteHostRpcActivity(uint16_t connectionId) {
  if (stateMutex_ == nullptr || !connections_.contains(connectionId)) return;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (!state_.connected) {
    xSemaphoreGive(stateMutex_);
    return;
  }
  const bool promoted = !state_.hostRpcObserved;
  state_.hostRpcObserved = true;
  state_.lastHostRpcAtMs = esp_log_timestamp();
  hostRpcConnectionId_ = connectionId;
  hostRpcConnectionValid_ = true;
  if (promoted) state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}

void CodexMicroBle::onOutput(const uint8_t* data, size_t length,
                             uint16_t connectionId,
                             const uint8_t* peerAddress) {
  if (data == nullptr || length == 0 || peerAddress == nullptr ||
      outputQueue_ == nullptr) {
    return;
  }
  PendingOutputReport report;
  report.length = static_cast<uint8_t>(
      length < report.data.size() ? length : report.data.size());
  report.connectionId = connectionId;
  memcpy(report.peerAddress.data(), peerAddress, report.peerAddress.size());
  memcpy(report.data.data(), data, report.length);
  xQueueSend(outputQueue_, &report, 0);
}

namespace {

// Returns the number of unbalanced '{' minus '}' characters, skipping string
// literals and escapes. Zero means the buffer holds a complete JSON value.
int jsonBraceDepth(const char* text, size_t length) {
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == '{') ++depth;
    if (c == '}') --depth;
  }
  return depth;
}

}  // namespace

void CodexMicroBle::processOutput(const uint8_t* data, size_t length,
                                  uint16_t connectionId,
                                  const PeerAddress& peerAddress) {
  (void)peerAddress;
  if (data == nullptr || length < 2 || !connections_.contains(connectionId)) {
    return;
  }

  if (rpcBufferConnectionValid_ && rpcBufferConnectionId_ != connectionId) {
    rpcLength_ = 0;
    rpcBufferConnectionValid_ = false;
  }

  // HOGP normally strips the report ID. Accept an included ID as well so the
  // transport remains compatible with hosts that forward the raw report.
  size_t offset = (length >= 3 && data[0] == kReportId) ? 1 : 0;
  if (length < offset + 2 || data[offset] != 2) return;

  const size_t payloadLength =
      data[offset + 1] < kPayloadSize ? data[offset + 1] : kPayloadSize;
  if (length < offset + 2 + payloadLength) return;
  const char* payload = reinterpret_cast<const char*>(data + offset + 2);

  constexpr char kTopLevelPrefix[] = "{\"method\"";
  const bool startsTopLevel =
      payloadLength >= sizeof(kTopLevelPrefix) - 1 &&
      memcmp(payload, kTopLevelPrefix, sizeof(kTopLevelPrefix) - 1) == 0;
  if (startsTopLevel && rpcLength_ > 0) {
    // A new top-level object means a previous fragmented write was dropped.
    // Resynchronize immediately instead of poisoning the next request.
    rpcLength_ = 0;
  }

  if (rpcLength_ == 0) {
    size_t jsonStart = 0;
    while (jsonStart < payloadLength && payload[jsonStart] != '{') ++jsonStart;
    if (jsonStart == payloadLength) return;
    rpcLength_ = payloadLength - jsonStart;
    if (rpcLength_ >= sizeof(rpcBuffer_)) rpcLength_ = sizeof(rpcBuffer_) - 1;
    memcpy(rpcBuffer_, payload + jsonStart, rpcLength_);
    rpcBufferConnectionId_ = connectionId;
    rpcBufferConnectionValid_ = true;
  } else {
    size_t room = sizeof(rpcBuffer_) - 1 - rpcLength_;
    size_t chunk = payloadLength < room ? payloadLength : room;
    memcpy(rpcBuffer_ + rpcLength_, payload, chunk);
    rpcLength_ += chunk;
  }
  rpcBuffer_[rpcLength_] = '\0';

  if (jsonBraceDepth(rpcBuffer_, rpcLength_) > 0) return;  // still incomplete

  cJSON* request = cJSON_ParseWithLength(rpcBuffer_, rpcLength_);
  if (request == nullptr) {
    ESP_LOGW(kTag, "RPC parse error: %s", rpcBuffer_);
    rpcLength_ = 0;
    rpcBufferConnectionValid_ = false;
    return;
  }

  if (handleRpc(request)) {
    noteHostRpcActivity(connectionId);
  }
  cJSON_Delete(request);
  rpcLength_ = 0;
  rpcBufferConnectionValid_ = false;
}

void CodexMicroBle::onQuotaWrite(const uint8_t* data, size_t length,
                                 bool responseExpected,
                                 const uint8_t* peerAddress) {
  if (data == nullptr || length == 0 || length > 512 ||
      peerAddress == nullptr || quotaWriteQueue_ == nullptr) {
    ESP_LOGW(kTag, "quota update rejected: invalid length");
    return;
  }
  PendingQuotaWrite write;
  write.length = static_cast<uint16_t>(length);
  write.responseExpected = responseExpected;
  memcpy(write.peerAddress.data(), peerAddress, write.peerAddress.size());
  memcpy(write.data.data(), data, length);
  if (xQueueSend(quotaWriteQueue_, &write, 0) != pdTRUE) {
    ESP_LOGW(kTag, "quota update rejected: queue full");
  }
}

void CodexMicroBle::processQuotaWrites() {
  if (quotaWriteQueue_ == nullptr) return;
  PendingQuotaWrite write;
  while (xQueueReceive(quotaWriteQueue_, &write, 0) == pdTRUE) {
    processQuotaWrite(write.data.data(), write.length, write.responseExpected,
                      write.peerAddress);
  }
}

void CodexMicroBle::processQuotaWrite(const uint8_t* data, size_t length,
                                      bool responseExpected,
                                      const PeerAddress& peerAddress) {
  (void)responseExpected;
  (void)peerAddress;
  if (data == nullptr || length == 0 || length > 512) {
    ESP_LOGW(kTag, "quota update rejected: invalid length");
    return;
  }

  cJSON* update = cJSON_ParseWithLength(reinterpret_cast<const char*>(data),
                                        length);
  if (update == nullptr || !cJSON_IsObject(update)) {
    ESP_LOGW(kTag, "quota update rejected: not a JSON object");
    cJSON_Delete(update);
    return;
  }

  quota_payload::Snapshot snapshot;
  if (!quota_payload::parse(update, snapshot)) {
    ESP_LOGW(kTag, "quota update rejected: invalid fields");
    cJSON_Delete(update);
    return;
  }
  cJSON_Delete(update);

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  state_.quota.fiveHourRemainingPercent = snapshot.fiveHourRemainingPercent;
  state_.quota.fiveHourResetInSeconds = snapshot.fiveHourResetInSeconds;
  state_.quota.weeklyRemainingPercent = snapshot.weeklyRemainingPercent;
  state_.quota.weeklyResetInSeconds = snapshot.weeklyResetInSeconds;
  state_.quota.receivedAtMs = esp_log_timestamp();
  state_.quota.available = true;
  // A live value supersedes anything restored at boot, and unlike a restored
  // value its countdown is anchored to a known instant.
  state_.quota.restored = false;
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);

  persistQuotaSnapshot(snapshot);

  ESP_LOGI(kTag, "quota update 5h=%.1f reset=%lus weekly=%.1f reset=%lus",
           snapshot.fiveHourRemainingPercent,
           static_cast<unsigned long>(snapshot.fiveHourResetInSeconds),
           snapshot.weeklyRemainingPercent,
           static_cast<unsigned long>(snapshot.weeklyResetInSeconds));
}

bool CodexMicroBle::handleRpc(const cJSON* request) {
  const cJSON* methodItem =
      cJSON_GetObjectItemCaseSensitive(request, "method");
  const char* method =
      (cJSON_IsString(methodItem) && methodItem->valuestring != nullptr)
          ? methodItem->valuestring
          : "";
  const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "id");
  const cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");
  const host_rpc::Method supportedMethod = host_rpc::classify(request);
  ESP_LOGI(kTag, "RPC method=%s", method);

  if (supportedMethod == host_rpc::Method::SystemVersion) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "version", kFirmwareVersion);
    sendResult(id, result);
    return true;
  }

  if (supportedMethod == host_rpc::Method::DeviceStatus) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "version", kFirmwareVersion);
    cJSON_AddNumberToObject(result, "profile_index", 0);
    cJSON_AddNumberToObject(result, "layer_index", 1);
    cJSON_AddNumberToObject(result, "battery", batteryPercentage_);
    cJSON_AddBoolToObject(result, "is_charging", charging_);
    sendResult(id, result);
    return true;
  }

  if (supportedMethod == host_rpc::Method::ThreadStatus) {
    updateThreadLighting(params);
    sendSuccess(id);
    return true;
  }

  if (supportedMethod == host_rpc::Method::RgbConfig ||
      supportedMethod == host_rpc::Method::LightsPreview ||
      supportedMethod == host_rpc::Method::HostFocusedApp) {
    sendSuccess(id);
    return true;
  }

  cJSON* response = cJSON_CreateObject();
  cJSON_AddItemToObject(response, "id",
                        id != nullptr ? cJSON_Duplicate(id, true)
                                      : cJSON_CreateNull());
  cJSON* error = cJSON_AddObjectToObject(response, "error");
  cJSON_AddNumberToObject(error, "code", -32601);
  cJSON_AddStringToObject(error, "message", "Method not found");
  char* json = cJSON_PrintUnformatted(response);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(response);
  return false;
}

void CodexMicroBle::sendResult(const cJSON* id, cJSON* result) {
  cJSON* response = cJSON_CreateObject();
  cJSON_AddItemToObject(response, "id",
                        id != nullptr ? cJSON_Duplicate(id, true)
                                      : cJSON_CreateNull());
  if (result != nullptr) {
    cJSON_AddItemToObject(response, "result", result);
  } else {
    cJSON_AddNullToObject(response, "result");
  }
  char* json = cJSON_PrintUnformatted(response);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(response);
}

void CodexMicroBle::sendSuccess(const cJSON* id) {
  cJSON* result = cJSON_CreateObject();
  cJSON_AddBoolToObject(result, "ok", true);
  sendResult(id, result);
}

void CodexMicroBle::sendJson(const char* json) {
  const auto peers = copyConnections();
  if (!enabled() || json == nullptr || inputHandle_ == 0 || !connected()) {
    // Silent early-returns here are indistinguishable from "the host ignored
    // us" from the outside, which is what made the dead HID path so hard to
    // see. Say which guard fired instead.
    ESP_LOGW(kTag, "sendJson skipped: json=%d handle=%u connected=%d",
             json != nullptr ? 1 : 0, static_cast<unsigned>(inputHandle_),
             connected() ? 1 : 0);
    return;
  }

  char framed[2048];
  const size_t length = strlen(json);
  const size_t total = length + 1 < sizeof(framed) ? length + 1 : sizeof(framed);
  memcpy(framed, json, total - 1);
  framed[total - 1] = '\n';

  uint32_t chunks = 0;
  uint32_t failed = 0;
  size_t offset = 0;
  while (offset < total) {
    const size_t chunk =
        (total - offset) < kPayloadSize ? (total - offset) : kPayloadSize;
    uint8_t report[kReportBodySize] = {};
    report[0] = 2;
    report[1] = static_cast<uint8_t>(chunk);
    memcpy(report + 2, framed + offset, chunk);

    esp_ble_gatts_set_attr_value(inputHandle_, sizeof(report), report);
    for (uint16_t i = 0; i < peers.count; ++i) {
      const esp_err_t result = esp_ble_gatts_send_indicate(
          g_gattsInterface, peers.ids[i], inputHandle_, sizeof(report),
          report, false);
      if (result != ESP_OK) {
        ++failed;
        ESP_LOGW(kTag, "HID notify failed id=%u: %s", peers.ids[i],
                 esp_err_to_name(result));
      }
    }
    ++chunks;
    offset += chunk;
    vTaskDelay(pdMS_TO_TICKS(4));
  }
  // s_cccd mirrors the descriptor the stack maintains: 0x0001 means the host
  // (or our pre-arm) enabled input-report notifications. If this reads 0x0000
  // the reports above were confirmed locally and dropped, never transmitted.
  ESP_LOGI(kTag, "sendJson chunks=%lu failed=%lu bytes=%u cccd=0x%02x%02x",
           static_cast<unsigned long>(chunks),
           static_cast<unsigned long>(failed), static_cast<unsigned>(total),
           s_cccd[1], s_cccd[0]);
}

void CodexMicroBle::updateThreadLighting(const cJSON* values) {
  if (!cJSON_IsArray(values)) return;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const cJSON* value = nullptr;
  cJSON_ArrayForEach(value, values) {
    const cJSON* idItem = cJSON_GetObjectItemCaseSensitive(value, "id");
    if (!cJSON_IsNumber(idItem)) continue;
    const int id = idItem->valueint;
    if (id < 0 || id >= static_cast<int>(state_.threads.size())) continue;

    ThreadLight& light = state_.threads[id];
    const cJSON* color = cJSON_GetObjectItemCaseSensitive(value, "c");
    if (cJSON_IsNumber(color)) light.color = static_cast<uint32_t>(color->valuedouble);
    const cJSON* brightness = cJSON_GetObjectItemCaseSensitive(value, "b");
    if (cJSON_IsNumber(brightness)) {
      light.brightness = static_cast<float>(brightness->valuedouble);
    }
    const cJSON* effect = cJSON_GetObjectItemCaseSensitive(value, "e");
    if (cJSON_IsString(effect) && effect->valuestring != nullptr) {
      strncpy(light.effect, effect->valuestring, sizeof(light.effect) - 1);
      light.effect[sizeof(light.effect) - 1] = '\0';
    }
    const cJSON* speed = cJSON_GetObjectItemCaseSensitive(value, "s");
    if (cJSON_IsNumber(speed)) {
      light.speed = static_cast<float>(speed->valuedouble);
    }
  }
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}
