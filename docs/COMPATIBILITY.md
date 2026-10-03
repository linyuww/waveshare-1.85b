# Compatibility evidence

- Board: Waveshare ESP32-S3-Touch-LCD-1.85B, stock onboard peripherals, no external wiring.
- ESP-IDF: 5.5.3, official `espressif/idf:v5.5.3` Docker image.
- Upstream source commit: `139e6db584f3737fcfc6a958ee83b79fb69d317c`.
- Source: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/tree/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/05_esp-brookesia
- Vendor instructions: https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B/ESP-IDF/
- LVGL: 9.5.0; Brookesia core: locally vendored 0.6.0-beta2; display adapter: 0.3.2.
- `brookesia_core/idf_component.yml`: IDF >=5.3.
- BSP manifest: IDF >=5.3, LVGL >=8,<10.
- Vendor dependency lock: `esp_lvgl_adapter` requires IDF >=5.5 and LVGL >=8,<10.
- These exact component versions are present together in the official IDF 5.5.3 sample bundle.
- Registry versions are pinned in `main/idf_component.yml`; full resolution is recorded in `dependencies.lock`.
- No ESP-ADF or ESP-SR is included in this first release.
- `esp_codec_dev` is updated from the vendor's withdrawn 1.5.5 to 1.5.6. The official registry metadata reports IDF >=4.0 and no withdrawal; the BSP's ~1.5 and GMF's ^1.4 constraints permit it. Official 1.5.6 changelog fixes the ADC dependency build issue: https://components-file.espressif.com/components/espressif/esp_codec_dev/1.5.6/changelog.md

## Vendor adaptations

Brookesia core and Waveshare BSP/RTC/IMU sources are copied from the commit above, retaining source licenses. The sample Squareline application and `bsp_extra` audio player are not needed by this launcher. The desktop stylesheet is adapted for Chinese labels and the generated icons.

The core manifest's GMF dependencies are conditional on the AI framework being enabled; that framework is disabled in this release, so the unused audio processing stack is excluded.

Two BSP build warnings are corrected without changing wiring: the board-name macro now uses underscores, and an IMU allocation uses the conventional calloc argument order. The hardware initialization and pin assignments remain those of the vendor BSP.

The first release has one 8 MiB app slot and reserves the remaining usable flash for future assets. There is no OTA boot switching, OTA data partition, or update implementation.

Compilation verifies the component/API combination. LCD colors, touch mapping, timing, reconnection, memory usage, and power behavior still require the physical board.

## Codex Micro integration (0.2.0)

Codex Micro graphics and the native Bluedroid protocol are imported from the user's local ESP-IDF >=5.4 project. No external Bluetooth framework is introduced. `bt`, `json`, and the I2C/I2S APIs are supplied by ESP-IDF 5.5.3. GAP/GATTS and the interface-specific MAC API are verified against its official headers; the generated configuration enables BLE 4.2, GATTS, SMP, and Wi-Fi coexistence. Bluetooth controller and host run on core 0; LVGL and the application worker run on core 1.

The BSP retains ownership of the LCD, touch and I2C bus. Source provenance and adaptation details are recorded in [CODEX-MICRO-INTEGRATION.md](CODEX-MICRO-INTEGRATION.md).

`BT_ALLOCATION_FROM_SPIRAM_FIRST` is enabled to keep Bluedroid allocations from competing with Wi-Fi and LCD DMA for internal RAM. The QMI8658 header guards its `M_PI` macro against the standard math header definition.

## On-board memory correction (0.2.1)

The first USB boot of 0.2.0 reported `ESP_ERR_NO_MEM` during BLE controller initialization and SPI DMA copies. The board's 8 MiB PSRAM passed its memory test, but the internal heap still has separate capacity requirements for task stacks, the controller and SPI transfers.

The LCD profile now uses two 20-line internal RGB565 buffers (28,800 bytes total), allowing this SPI path to transmit the buffers directly instead of allocating temporary copies of PSRAM buffers. LVGL uses one drawing unit. Wi-Fi static RX/TX counts are each 8 and the RX block-ack window is 6. The ordinary allocation threshold is 1 KiB and 64 KiB of internal memory is reserved for explicit internal/DMA allocations. The Codex software framebuffer and large UI allocations continue to use PSRAM. LCD/touch pin assignments are unchanged.

The allocation choices follow ESP-IDF 5.5.3's [heap capability documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/system/mem_alloc.html) and [external RAM configuration](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/external-ram.html). Boot and once-per-minute runtime logs report internal free memory, the largest DMA block and PSRAM free memory for further hardware checks.

## UI / HTTPS memory correction (0.2.2)

- Ordinary malloc allocations prefer PSRAM (`SPIRAM_MALLOC_ALWAYSINTERNAL=0`); internal-only DMA/RTOS allocations keep their required capabilities and the 64 KiB internal reserve.
- MbedTLS explicitly uses PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC=y`) instead of competing with BLE and LCD DMA in the approximately 2 KiB left on the old connected build.
- The HTTPS probe resolves DNS, requires valid clock time, keeps certificate bundle validation, sets 12 s connect/read timeouts and 512-byte HTTP buffers, and reads response headers without downloading an entire page. DNS and TLS/socket errors are reported separately.
- Launcher icons use RGB565 plus a separate A8 alpha plane; opaque dark corners were removed. Wallpaper covers all 360 × 360 pixels.
- The launcher uses fixed dimensions from `ui_layout.hpp`; Brookesia automatic launcher sizing is disabled so it cannot overwrite the requested 20 px downward offset.
- Battery telemetry remains owned by the service task; GUI receives a mutex-protected copy and does not read I2C. Gauge refresh still runs if BLE fails to initialize. Failed reads retain their last sample and explicitly mark it stale.
- The BQ27220 standard command map was checked against TI TRM SLUUBD4A, revised November 2022. CycleCount is at 0x2A, and RawCoulombCount is unsigned and expressed in mAh. The requested charging parameters are labelled separately from measured current/voltage; 0xFFFF time means unavailable.
