#include "NodeDB.h"
#include "RadioLibInterface.h"
#include "PowerFSM.h"
#include "PowerMon.h"
#include "Throttle.h"
#include "airtime.h"
#include "mesh/Router.h"
#include "configuration.h"
#include "esp_task_wdt.h"
#include "hydra/HydraLog.h"
#include "PowerStatus.h"
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
#include "modules/Telemetry/EnvironmentTelemetry.h"
#endif
#include <WiFi.h>
#include "main.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !MESHTASTIC_EXCLUDE_BLUETOOTH
#include "nimble/NimbleBluetooth.h"
#include <esp_bt.h>
#endif

#include <MeshtasticOTA.h>

#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif

#include "esp_mac.h"
#include "meshUtils.h"
#include "sleep.h"
#include "soc/rtc.h"
#include "target_specific.h"
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <nvs.h>
#include <nvs_flash.h>

// Weak empty variant shutdown prep function.
// May be redefined by variant files.
void variant_shutdown() __attribute__((weak));
void variant_shutdown() {}

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !MESHTASTIC_EXCLUDE_BLUETOOTH
void setBluetoothEnable(bool enable)
{
#ifdef USE_WS5500
    if ((config.bluetooth.enabled == true) && (config.network.wifi_enabled == false))
#elif HAS_WIFI
    if (!isWifiAvailable() && config.bluetooth.enabled == true)
#else
    if (config.bluetooth.enabled == true)
#endif
    {
        if (!nimbleBluetooth) {
            nimbleBluetooth = new NimbleBluetooth();
        }
        if (enable && !nimbleBluetooth->isActive()) {
            powerMon->setState(meshtastic_PowerMon_State_BT_On);
            nimbleBluetooth->setup();
        }
        // For ESP32, no way to recover from bluetooth shutdown without reboot
        // BLE advertising automatically stops when MCU enters light-sleep(?)
        // For deep-sleep, shutdown hardware with nimbleBluetooth->deinit(). Requires reboot to reverse
    }
}

// hydra: with Wi-Fi configured setBluetoothEnable() never starts BT, but the DRAM reserved for the BT controller
// (CONFIG_BTDM_RESERVE_DRAM, ~56 KB) is never returned, because btInUse() is true. Give it to the heap.
// Must run after NodeDB loaded the config and before anything starts BT. BT comes back only after a reboot.
void releaseBluetoothMemoryIfWifi()
{
#if HAS_WIFI
    if (isWifiAvailable()) {
        uint32_t before = ESP.getFreeHeap();
        esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        LOG_INFO("BT memory release: %s, free heap %u -> %u", esp_err_to_name(err), before, ESP.getFreeHeap());
    }
#endif
}
#else
void setBluetoothEnable(bool enable) {}
void updateBatteryLevel(uint8_t level) {}
void releaseBluetoothMemoryIfWifi() {}
#endif

// hydra: reset reason history and minimum free heap, sent to API clients in MyNodeInfo
static uint32_t hydraResetReasons; // last 4 esp_reset_reason() values, newest in the low byte
static uint32_t hydraPrevMinHeap;  // minimum free heap of the previous run, 0 = unknown
static uint32_t hydraStoredMinHeap = UINT32_MAX;

static const char *hydraResetReasonName(uint8_t reason)
{
    static const char *const names[] = {"UNKNOWN", "POWERON",  "EXT",      "SW",       "PANIC",   "INT_WDT",
                                        "TASK_WDT", "WDT",     "DEEPSLEEP", "BROWNOUT", "SDIO"};
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "?";
}

static void hydraHealthSetup(Preferences &preferences)
{
    hydraResetReasons = (preferences.getUInt("hydraRst", 0) << 8) | (uint8_t)esp_reset_reason();
    preferences.putUInt("hydraRst", hydraResetReasons);
    hydraPrevMinHeap = preferences.getUInt("hydraMinHp", 0);
    if (preferences.isKey("hydraMinHp"))
        preferences.remove("hydraMinHp");
    LOG_INFO("Hydra reset reason %s (previous %s, %s, %s), previous run min free heap %u",
             hydraResetReasonName(hydraResetReasons & 0xff), hydraResetReasonName((hydraResetReasons >> 8) & 0xff),
             hydraResetReasonName((hydraResetReasons >> 16) & 0xff), hydraResetReasonName(hydraResetReasons >> 24),
             hydraPrevMinHeap);
}

// Saves the minimum free heap to flash only when it drops by more than 1 KB, so a crash still leaves it behind
void hydraHealthTick()
{
    static uint32_t lastCheck, lastLog;
    static bool logged;
    static uint32_t reportedMinHeap = UINT32_MAX;

    // Every loop: log a new minimum at once, so the UART log shows what was happening when the heap dipped
    uint32_t minHeap = ESP.getMinFreeHeap();
    if (minHeap + 2048 <= reportedMinHeap && millis() > 60 * 1000) {
        reportedMinHeap = minHeap;
        LOG_WARN("Hydra heap min dropped to %u (free now %u, largest block %u), packets in use %d peak %d, retrans %d peak %d",
                 minHeap, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), packetPool.inUse(), packetPool.peakInUse(),
                 retransPacketPool.inUse(), retransPacketPool.peakInUse());
        packetPool.resetPeak();
        retransPacketPool.resetPeak();
    }

    // hydra v16: noise floor sample every 10 s (only taken when the radio is idle in receive)
    static uint32_t lastNoise;
    if (RadioLibInterface::instance && !Throttle::isWithinTimespanMs(lastNoise, 10 * 1000)) {
        lastNoise = millis();
        RadioLibInterface::instance->updateNoiseFloor();
    }

    // hydra v21: report as soon as Wi-Fi (re)connects; lines logged while it was down may have been dropped from the send buffer
    static bool wifiWasUp;
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    bool reportNow = wifiUp && !wifiWasUp && millis() > 5 * 1000;
    wifiWasUp = wifiUp;

    if (!reportNow && Throttle::isWithinTimespanMs(lastCheck, 60 * 1000))
        return;
    lastCheck = millis();
    if (minHeap + 1024 < hydraStoredMinHeap) {
        Preferences preferences;
        preferences.begin("meshtastic", false);
        preferences.putUInt("hydraMinHp", minHeap);
        preferences.end();
        hydraStoredMinHeap = minHeap;
    }
    if (!logged || reportNow || !Throttle::isWithinTimespanMs(lastLog, 10 * 60 * 1000)) {
        logged = true;
        lastLog = millis();
        LOG_INFO("Hydra health: uptime %us, free heap %u, min free heap %u, largest block %u, nodes %u, packets in use %d peak %d,"
                 " noise floor %d dBm (%u samples)",
                 millis() / 1000, ESP.getFreeHeap(), minHeap, ESP.getMaxAllocHeap(), nodeDB ? nodeDB->getNumMeshNodes() : 0,
                 packetPool.inUse(), packetPool.peakInUse(),
                 RadioLibInterface::instance ? (int)RadioLibInterface::instance->getAverageNoiseFloor() : 0,
                 RadioLibInterface::instance ? RadioLibInterface::instance->getNoiseFloorSampleCount() : 0);
        // hydra v20: what the RPi used to read over the API, so it can watch the boards from the Wi-Fi log alone
        // (the API has one client slot and every new connection drops the phone)
        RadioLibInterface *radio = RadioLibInterface::instance;
        LOG_INFO("Hydra stats: reboots %u, reset %s<%s<%s<%s, online %u, rx %u bad %u dupe %u, tx %u relay %u relay_cancel %u,"
                 " ch_util %.2f, air_tx %.2f",
                 myNodeInfo.reboot_count, hydraResetReasonName(hydraResetReasons & 0xff),
                 hydraResetReasonName((hydraResetReasons >> 8) & 0xff), hydraResetReasonName((hydraResetReasons >> 16) & 0xff),
                 hydraResetReasonName(hydraResetReasons >> 24), nodeDB ? (unsigned)nodeDB->getNumOnlineMeshNodes() : 0,
                 radio ? radio->rxGood + radio->rxBad : 0, radio ? radio->rxBad : 0, router ? router->rxDupe : 0,
                 radio ? radio->txGood : 0, radio ? radio->txRelay : 0, router ? router->txRelayCanceled : 0,
                 airTime ? airTime->channelUtilizationPercent() : 0.0f, airTime ? airTime->utilizationTXPercent() : 0.0f);
        // hydra v24: current battery and environment values, so the Wi-Fi log carries what the mesh sends
        meshtastic_EnvironmentMetrics env = meshtastic_EnvironmentMetrics_init_zero;
        bool envOk = false;
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
        envOk = EnvironmentTelemetryModule::instance && EnvironmentTelemetryModule::instance->currentEnvironment(&env);
#endif
        // same power reading as DeviceTelemetry: no battery or charging means USB power, shown as 101 there
        bool usbPower = !powerStatus->getHasBattery() || powerStatus->getIsCharging();
        LOG_INFO("Hydra telemetry: power %s, batt %u%% %.2f V, env %s, temp %.2f C, pressure %.1f hPa, humidity %.1f%%",
                 usbPower ? "USB" : "battery", powerStatus->getBatteryChargePercent(), powerStatus->getBatteryVoltageMv() / 1000.0f,
                 envOk ? "ok" : "none", env.temperature, env.barometric_pressure, env.relative_humidity);
    }
}

void hydraFillMyNodeInfo(meshtastic_MyNodeInfo &info)
{
    info.hydra_reset_reasons = hydraResetReasons;
    info.hydra_min_free_heap = ESP.getMinFreeHeap();
    info.hydra_prev_min_free_heap = hydraPrevMinHeap;
    info.hydra_free_heap = ESP.getFreeHeap();
    info.hydra_noise_floor = RadioLibInterface::instance ? RadioLibInterface::instance->getAverageNoiseFloor() : 0;
}

void getMacAddr(uint8_t *dmac)
{
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_SOC_IEEE802154_SUPPORTED)
    auto res = esp_base_mac_addr_get(dmac);
    assert(res == ESP_OK);
#else
    auto res = esp_efuse_mac_get_default(dmac);
    assert(res == ESP_OK);
#endif
}

#if HAS_32768HZ
#define CALIBRATE_ONE(cali_clk) calibrate_one(cali_clk, #cali_clk)

static uint32_t calibrate_one(rtc_cal_sel_t cal_clk, const char *name)
{
    const uint32_t cal_count = 1000;
    // const float factor = (1 << 19) * 1000.0f; unused var?
    uint32_t cali_val;
    for (int i = 0; i < 5; ++i) {
        cali_val = rtc_clk_cal(cal_clk, cal_count);
    }
    return cali_val;
}

void enableSlowCLK()
{
    rtc_clk_32k_enable(true);

    CALIBRATE_ONE(RTC_CAL_RTC_MUX);
    uint32_t cal_32k = CALIBRATE_ONE(RTC_CAL_32K_XTAL);

    if (cal_32k == 0) {
        LOG_DEBUG("32k XTAL OSC has not started up");
    } else {
        rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
        LOG_DEBUG("Switch RTC Source to 32.768kHz succeeded, using 32k XTAL");
        CALIBRATE_ONE(RTC_CAL_RTC_MUX);
        CALIBRATE_ONE(RTC_CAL_32K_XTAL);
    }
    CALIBRATE_ONE(RTC_CAL_RTC_MUX);
    CALIBRATE_ONE(RTC_CAL_32K_XTAL);
    if (rtc_clk_slow_freq_get() != RTC_SLOW_FREQ_32K_XTAL) {
        LOG_WARN("Failed to switch 32K XTAL RTC source to 32.768kHz !!! ");
        return;
    }
}
#endif

void esp32Setup()
{
    /* We explicitly don't want to do call randomSeed,
    // as that triggers the esp32 core to use a less secure pseudorandom function.
    uint32_t seed = esp_random();
    LOG_DEBUG("Set random seed %u", seed);
    randomSeed(seed);
    */

#ifdef ADC_V
    pinMode(ADC_V, INPUT);
#endif

    LOG_DEBUG("Total heap: %d", ESP.getHeapSize());
    LOG_DEBUG("Free heap: %d", ESP.getFreeHeap());
    LOG_DEBUG("Total PSRAM: %d", ESP.getPsramSize());
    LOG_DEBUG("Free PSRAM: %d", ESP.getFreePsram());

    nvs_stats_t nvs_stats;
    auto res = nvs_get_stats(NULL, &nvs_stats);
    assert(res == ESP_OK);
    LOG_DEBUG("NVS: UsedEntries %d, FreeEntries %d, AllEntries %d, NameSpaces %d", nvs_stats.used_entries, nvs_stats.free_entries,
              nvs_stats.total_entries, nvs_stats.namespace_count);

    LOG_DEBUG("Setup Preferences in Flash Storage");

    // Create object to store our persistent data
    Preferences preferences;
    preferences.begin("meshtastic", false);

    uint32_t rebootCounter = preferences.getUInt("rebootCounter", 0);
    rebootCounter++;
    preferences.putUInt("rebootCounter", rebootCounter);
    hydraHealthSetup(preferences);
    // store firmware version and hwrevision for access from OTA firmware
    String fwrev = preferences.getString("firmwareVersion", "");
    if (fwrev.compareTo(optstr(APP_VERSION)) != 0)
        preferences.putString("firmwareVersion", optstr(APP_VERSION));
    uint8_t hwven = preferences.getUInt("hwVendor", 0);
    if (hwven != HW_VENDOR)
        preferences.putUInt("hwVendor", HW_VENDOR);
    preferences.end();
    LOG_DEBUG("Number of Device Reboots: %d", rebootCounter);
#if !MESHTASTIC_EXCLUDE_WIFI
    String version = MeshtasticOTA::getVersion();
    if (version.isEmpty()) {
        LOG_INFO("MeshtasticOTA firmware not available");
    } else {
        LOG_INFO("MeshtasticOTA firmware version %s", version.c_str());
    }
    MeshtasticOTA::initialize();
#endif

    // enableModemSleep();

// Since we are turning on watchdogs rather late in the release schedule, we really don't want to catch any
// false positives.  The wait-to-sleep timeout for shutting down radios is 30 secs, so pick 45 for now.
// #define APP_WATCHDOG_SECS 45
#define APP_WATCHDOG_SECS 90

#ifdef CONFIG_IDF_TARGET_ESP32C6
    esp_task_wdt_config_t *wdt_config = (esp_task_wdt_config_t *)malloc(sizeof(esp_task_wdt_config_t));
    wdt_config->timeout_ms = APP_WATCHDOG_SECS * 1000;
    wdt_config->trigger_panic = true;
    res = esp_task_wdt_init(wdt_config);
    assert(res == ESP_OK);
#else
    res = esp_task_wdt_init(APP_WATCHDOG_SECS, true);
    assert(res == ESP_OK);
#endif
    res = esp_task_wdt_add(NULL);
    assert(res == ESP_OK);

#if HAS_32768HZ
    enableSlowCLK();
#endif
}

/// loop code specific to ESP32 targets
void esp32Loop()
{
    esp_task_wdt_reset(); // service our app level watchdog
    hydraHealthTick();
    hydraLogTick();

    // for debug printing
    // radio.radioIf.canSleep();
}

void cpuDeepSleep(uint32_t msecToWake)
{
    /*
    Some ESP32 IOs have internal pullups or pulldowns, which are enabled by default.
    If an external circuit drives this pin in deep sleep mode, current consumption may
    increase due to current flowing through these pullups and pulldowns.

    To isolate a pin, preventing extra current draw, call rtc_gpio_isolate() function.
    For example, on ESP32-WROVER module, GPIO12 is pulled up externally.
    GPIO12 also has an internal pulldown in the ESP32 chip. This means that in deep sleep,
    some current will flow through these external and internal resistors, increasing deep
    sleep current above the minimal possible value.

    Note: we don't isolate pins that are used for the LORA, LED, i2c, or ST7735 Display for the Chatter2, spi or the wake
    button(s), maybe we should not include any other GPIOs...
    */
#if SOC_RTCIO_HOLD_SUPPORTED
    static const uint8_t rtcGpios[] = {
#ifndef HELTEC_VISION_MASTER_E213
        // For this variant, >20mA leaks through the display if pin 2 held
        // Todo: check if it's safe to remove this pin for all variants
        2,
#endif
#ifndef USE_JTAG
        13,
#endif
        34, 35, 37};

    for (int i = 0; i < sizeof(rtcGpios); i++)
        rtc_gpio_isolate((gpio_num_t)rtcGpios[i]);
#endif

        // FIXME, disable internal rtc pullups/pulldowns on the non isolated pins. for inputs that we aren't using
        // to detect wake and in normal operation the external part drives them hard.
#ifdef BUTTON_PIN
        // Only GPIOs which are have RTC functionality can be used in this bit map: 0,2,4,12-15,25-27,32-39.
#if SOC_RTCIO_HOLD_SUPPORTED && SOC_PM_SUPPORT_EXT_WAKEUP
    uint64_t gpioMask = (1ULL << (config.device.button_gpio ? config.device.button_gpio : BUTTON_PIN));
#endif
#ifdef ALT_BUTTON_WAKE
    gpioMask |= (1ULL << BUTTON_PIN_ALT);
#endif
#ifdef BUTTON_NEED_PULLUP
    gpio_pullup_en((gpio_num_t)BUTTON_PIN);
#endif

    // Not needed because both of the current boards have external pullups
    // FIXME change polarity in hw so we can wake on ANY_HIGH instead - that would allow us to use all three buttons (instead
    // of just the first) gpio_pullup_en((gpio_num_t)BUTTON_PIN);

#ifdef ESP32S3_WAKE_TYPE
    esp_sleep_enable_ext1_wakeup(gpioMask, ESP32S3_WAKE_TYPE);
#else
#if SOC_PM_SUPPORT_EXT_WAKEUP
#ifdef CONFIG_IDF_TARGET_ESP32
    // ESP_EXT1_WAKEUP_ALL_LOW has been deprecated since esp-idf v5.4 for any other target.
    esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ALL_LOW);
#else
    esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
#endif

#endif // #end ESP32S3_WAKE_TYPE
#endif
    variant_shutdown();

    // We want RTC peripherals to stay on
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_sleep_enable_timer_wakeup(msecToWake * 1000ULL); // call expects usecs
    esp_deep_sleep_start();                              // TBD mA sleep current (battery)
}
