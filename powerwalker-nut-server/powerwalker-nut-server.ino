#include <Arduino.h>
#include <string.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

// ============================================================
// USER SETTINGS
// ============================================================
const char *WIFI_SSID = "willstedt";
const char *WIFI_PASSWORD = "will13734";
const char *NUT_UPS_NAME = "ups";
const char *SNMP_COMMUNITY = "public";

const char *DEVICE_MFR = "BlueWalker";
const char *DEVICE_MODEL = "BlueWalker Online";
const char *DEVICE_DESC = "BlueWalker Online";
const char *DRIVER_NAME = "esp32-usb-ups";
const char *DRIVER_VERSION = "0.5";

// These are still partial estimates until more USB tags are decoded
const int DEFAULT_LOAD_PERCENT = 11;
const int DEFAULT_OUTPUT_VOLTAGE = 230;

// ============================================================
// OLED SETTINGS
// ============================================================
const int OLED_WIDTH = 128;
const int OLED_HEIGHT = 64;
const int OLED_RESET = -1;
const uint8_t OLED_ADDR = 0x3C;

// Your wiring
const int OLED_SDA = 8;
const int OLED_SCL = 9;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;
unsigned long lastDisplayUpdateMs = 0;

// ============================================================
// USB / UPS IDs
// ============================================================
static const uint16_t UPS_VID = 0x0665;
static const uint16_t UPS_PID = 0x5161;
static const uint8_t EP1 = 0x81;
static const uint8_t EP2 = 0x82;
static const size_t REPORT_SIZE = 8;

// ============================================================
// NETWORK SERVICES
// ============================================================
WebServer httpServer(80);
WiFiServer nutServer(3493);
WiFiUDP snmpUdp;

// ============================================================
// USB GLOBALS
// ============================================================
usb_host_client_handle_t g_client = nullptr;
usb_device_handle_t g_dev_hdl = nullptr;
usb_transfer_t *g_xfer81 = nullptr;
usb_transfer_t *g_xfer82 = nullptr;

volatile uint8_t g_dev_addr = 0;
volatile bool g_dev_present = false;
bool g_claimed = false;

// ============================================================
// UPS STATE
// ============================================================
struct UpsState {
  bool have_flags = false;
  bool have_charge = false;
  bool have_metric = false;

  uint32_t flags = 0;          // tag 0x32
  uint8_t charge_percent = 0;  // tag 0x34
  uint16_t battery_metric = 0; // tag 0x35

  unsigned long last_flags_ms = 0;
  unsigned long last_charge_ms = 0;
  unsigned long last_metric_ms = 0;

  unsigned long battery_start_ms = 0;
};

UpsState g_state;
UpsState g_last_printed;

bool g_have_last_metric_logged = false;
uint16_t g_last_metric_logged = 0;
unsigned long g_last_metric_log_ms = 0;

struct LastUnknown {
  bool valid = false;
  uint8_t tag = 0;
  uint8_t len = 0;
  uint8_t data[8] = {0};
};

LastUnknown g_last_unknown_81;
LastUnknown g_last_unknown_82;

// ============================================================
// SNMP TYPES
// ============================================================
struct SnmpOidEntry {
  const uint32_t *oid;
  size_t oidLen;
};

enum SnmpValueType {
  SVT_INTEGER,
  SVT_STRING,
  SVT_TIMETICKS
};

struct SnmpValue {
  SnmpValueType type;
  int32_t intValue;
  String strValue;
};

// ============================================================
// SNMP OIDs
// ============================================================
static const uint32_t OID_sysDescr[]                 = {1, 3, 6, 1, 2, 1, 1, 1, 0};
static const uint32_t OID_sysUpTime[]                = {1, 3, 6, 1, 2, 1, 1, 3, 0};

static const uint32_t OID_upsIdentManufacturer[]     = {1, 3, 6, 1, 2, 1, 33, 1, 1, 1, 0};
static const uint32_t OID_upsIdentModel[]            = {1, 3, 6, 1, 2, 1, 33, 1, 1, 2, 0};
static const uint32_t OID_upsIdentSoftwareVersion[]  = {1, 3, 6, 1, 2, 1, 33, 1, 1, 3, 0};

static const uint32_t OID_upsBatteryStatus[]         = {1, 3, 6, 1, 2, 1, 33, 1, 2, 1, 0};
static const uint32_t OID_upsSecondsOnBattery[]      = {1, 3, 6, 1, 2, 1, 33, 1, 2, 2, 0};
static const uint32_t OID_upsEstimatedMinutes[]      = {1, 3, 6, 1, 2, 1, 33, 1, 2, 3, 0};
static const uint32_t OID_upsEstimatedCharge[]       = {1, 3, 6, 1, 2, 1, 33, 1, 2, 4, 0};

static const uint32_t OID_upsInputNumLines[]         = {1, 3, 6, 1, 2, 1, 33, 1, 3, 2, 0};
static const uint32_t OID_upsInputVoltage[]          = {1, 3, 6, 1, 2, 1, 33, 1, 3, 3, 1, 3, 1};

static const uint32_t OID_upsOutputSource[]          = {1, 3, 6, 1, 2, 1, 33, 1, 4, 1, 0};
static const uint32_t OID_upsOutputVoltage[]         = {1, 3, 6, 1, 2, 1, 33, 1, 4, 2, 1, 2, 1};
static const uint32_t OID_upsOutputPercentLoad[]     = {1, 3, 6, 1, 2, 1, 33, 1, 4, 4, 1, 5, 1};

static const SnmpOidEntry SNMP_OIDS[] = {
  {OID_sysDescr, sizeof(OID_sysDescr) / sizeof(uint32_t)},
  {OID_sysUpTime, sizeof(OID_sysUpTime) / sizeof(uint32_t)},
  {OID_upsIdentManufacturer, sizeof(OID_upsIdentManufacturer) / sizeof(uint32_t)},
  {OID_upsIdentModel, sizeof(OID_upsIdentModel) / sizeof(uint32_t)},
  {OID_upsIdentSoftwareVersion, sizeof(OID_upsIdentSoftwareVersion) / sizeof(uint32_t)},
  {OID_upsBatteryStatus, sizeof(OID_upsBatteryStatus) / sizeof(uint32_t)},
  {OID_upsSecondsOnBattery, sizeof(OID_upsSecondsOnBattery) / sizeof(uint32_t)},
  {OID_upsEstimatedMinutes, sizeof(OID_upsEstimatedMinutes) / sizeof(uint32_t)},
  {OID_upsEstimatedCharge, sizeof(OID_upsEstimatedCharge) / sizeof(uint32_t)},
  {OID_upsInputNumLines, sizeof(OID_upsInputNumLines) / sizeof(uint32_t)},
  {OID_upsInputVoltage, sizeof(OID_upsInputVoltage) / sizeof(uint32_t)},
  {OID_upsOutputSource, sizeof(OID_upsOutputSource) / sizeof(uint32_t)},
  {OID_upsOutputVoltage, sizeof(OID_upsOutputVoltage) / sizeof(uint32_t)},
  {OID_upsOutputPercentLoad, sizeof(OID_upsOutputPercentLoad) / sizeof(uint32_t)},
};

// ============================================================
// HELPERS
// ============================================================
static void print_hex(const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    Serial.printf("%02X ", buf[i]);
  }
}

static bool is_on_battery_flags(uint32_t flags) {
  return flags == 0x002000;
}

static bool is_on_mains_flags(uint32_t flags) {
  return flags == 0x112000;
}

static bool is_switching_flags(uint32_t flags) {
  return flags == 0x102000 || flags == 0x012000;
}

static bool isUpsUsbConnected() {
  return g_dev_hdl != nullptr && g_dev_present;
}

static const char *power_state_text(uint32_t flags) {
  if (is_on_mains_flags(flags)) return "ON MAINS";
  if (is_on_battery_flags(flags)) return "ON BATTERY";
  if (is_switching_flags(flags)) return "SWITCHING";
  return "UNKNOWN";
}

static String shortPowerState() {
  if (!g_state.have_flags) return "WAIT";
  if (is_on_mains_flags(g_state.flags)) return "ON MAINS";
  if (is_on_battery_flags(g_state.flags)) return "ON BATTERY";
  if (is_switching_flags(g_state.flags)) return "SWITCHING";
  return "UNKNOWN";
}

static String nut_status() {
  if (!g_state.have_flags) return "WAIT";
  if (is_on_mains_flags(g_state.flags)) return "OL CHRG";
  if (is_on_battery_flags(g_state.flags)) return "OB DISCHRG";
  if (is_switching_flags(g_state.flags)) return "OB";
  return "WAIT";
}

static int estimated_load_percent() {
  return DEFAULT_LOAD_PERCENT;
}

static float estimated_output_voltage() {
  return DEFAULT_OUTPUT_VOLTAGE;
}

static float estimated_input_voltage() {
  if (is_on_mains_flags(g_state.flags)) return DEFAULT_OUTPUT_VOLTAGE;
  return 0.0f;
}

static int estimated_minutes_remaining() {
  if (!g_state.have_charge) return 0;
  int mins = (int)round(g_state.charge_percent * 0.53f);
  if (mins < 1) mins = 1;
  return mins;
}

static uint32_t seconds_on_battery() {
  if (!g_state.have_flags || !is_on_battery_flags(g_state.flags) || g_state.battery_start_ms == 0) {
    return 0;
  }
  return (millis() - g_state.battery_start_ms) / 1000UL;
}

static int snmp_battery_status() {
  if (!g_state.have_flags) return 1;
  if (is_on_battery_flags(g_state.flags) && g_state.have_charge && g_state.charge_percent <= 20) return 3;
  return 2;
}

static int snmp_output_source() {
  if (!g_state.have_flags) return 1;
  if (is_on_mains_flags(g_state.flags)) return 3;
  if (is_on_battery_flags(g_state.flags)) return 5;
  return 1;
}

static bool same_state(const UpsState &a, const UpsState &b) {
  return memcmp(&a, &b, sizeof(UpsState)) == 0;
}

static String statusColor() {
  if (!g_state.have_flags) return "#6b7280";
  if (is_on_mains_flags(g_state.flags)) return "#10b981";
  if (is_on_battery_flags(g_state.flags)) return "#f59e0b";
  if (is_switching_flags(g_state.flags)) return "#3b82f6";
  return "#ef4444";
}

static String htmlIconPower() {
  if (is_on_mains_flags(g_state.flags)) {
    return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M7 2v6'/><path d='M17 2v6'/><path d='M6 8h12v6a6 6 0 0 1-12 0V8z'/></svg>";
  }
  if (is_on_battery_flags(g_state.flags)) {
    return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><rect x='2' y='7' width='18' height='10' rx='2'/><path d='M22 10v4'/><path d='M6 10l3 4 2-3 2 3 3-4'/></svg>";
  }
  return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M13 2L4 14h6l-1 8 9-12h-6l1-8z'/></svg>";
}

static String htmlIconBattery() {
  return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><rect x='2' y='7' width='18' height='10' rx='2'/><path d='M22 10v4'/></svg>";
}

static String htmlIconLoad() {
  return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M4 18h16'/><path d='M7 18V9'/><path d='M12 18V5'/><path d='M17 18v-7'/></svg>";
}

static String htmlIconClock() {
  return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><circle cx='12' cy='12' r='9'/><path d='M12 7v5l3 3'/></svg>";
}

static String htmlIconWifi() {
  return "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M5 12.55a11 11 0 0 1 14.08 0'/><path d='M1.42 9a16 16 0 0 1 21.16 0'/><path d='M8.53 16.11a6 6 0 0 1 6.95 0'/><path d='M12 20h.01'/></svg>";
}

static void maybe_log_metric() {
  if (!g_state.have_metric) return;

  const unsigned long now = millis();
  const bool first = !g_have_last_metric_logged;
  const bool enough_time = (now - g_last_metric_log_ms) >= 5000;
  const bool enough_change = first || abs((int)g_state.battery_metric - (int)g_last_metric_logged) >= 5;

  if (enough_time || enough_change) {
    Serial.printf("[%10lu ms] BATTERY_METRIC UPDATE: %u (0x%04X, %.3f)\n",
                  now,
                  g_state.battery_metric,
                  g_state.battery_metric,
                  g_state.battery_metric / 1000.0f);

    g_last_metric_logged = g_state.battery_metric;
    g_have_last_metric_logged = true;
    g_last_metric_log_ms = now;
  }
}

static void print_summary(const char *reason) {
  UpsState tmp = g_state;
  tmp.last_flags_ms = 0;
  tmp.last_charge_ms = 0;
  tmp.last_metric_ms = 0;

  UpsState last = g_last_printed;
  last.last_flags_ms = 0;
  last.last_charge_ms = 0;
  last.last_metric_ms = 0;

  if (same_state(tmp, last)) return;
  g_last_printed = g_state;

  Serial.println();
  Serial.println("========================================");
  Serial.printf("[%10lu ms] UPS SUMMARY (%s)\n", millis(), reason);

  if (g_state.have_flags) {
    Serial.printf("PowerState     : %s\n", power_state_text(g_state.flags));
    Serial.printf("Flags          : 0x%06lX\n", (unsigned long)g_state.flags);
    Serial.printf("NUT status     : %s\n", nut_status().c_str());
  } else {
    Serial.println("PowerState     : unknown");
    Serial.println("Flags          : unknown");
    Serial.println("NUT status     : WAIT");
  }

  if (g_state.have_charge) {
    Serial.printf("ChargePercent  : %u %%\n", g_state.charge_percent);
  } else {
    Serial.println("ChargePercent  : unknown");
  }

  if (g_state.have_metric) {
    Serial.printf("BatteryMetric  : %u (0x%04X)\n", g_state.battery_metric, g_state.battery_metric);
    Serial.printf("Metric/1000    : %.3f\n", g_state.battery_metric / 1000.0f);
  } else {
    Serial.println("BatteryMetric  : unknown");
  }

  Serial.printf("LoadEstimate   : %d %%\n", estimated_load_percent());
  Serial.printf("OutputVoltage  : %.1f V\n", estimated_output_voltage());
  Serial.printf("InputVoltage   : %.1f V\n", estimated_input_voltage());
  Serial.printf("MinutesRemain  : %d\n", estimated_minutes_remaining());

  Serial.println("========================================");
}

// ============================================================
// OLED DRAWING
// ============================================================
static void drawPlugIcon(int x, int y) {
  display.drawRect(x + 6, y + 8, 16, 12, SSD1306_WHITE);
  display.drawLine(x + 10, y + 3, x + 10, y + 8, SSD1306_WHITE);
  display.drawLine(x + 18, y + 3, x + 18, y + 8, SSD1306_WHITE);
  display.drawLine(x + 14, y + 20, x + 14, y + 27, SSD1306_WHITE);
  display.drawLine(x + 14, y + 27, x + 10, y + 31, SSD1306_WHITE);
}

static void drawBatteryIcon(int x, int y, int pct) {
  display.drawRoundRect(x, y + 6, 24, 12, 2, SSD1306_WHITE);
  display.drawRect(x + 24, y + 9, 3, 6, SSD1306_WHITE);

  int fill = map(pct, 0, 100, 0, 20);
  if (fill < 0) fill = 0;
  if (fill > 20) fill = 20;
  if (fill > 0) {
    display.fillRect(x + 2, y + 8, fill, 8, SSD1306_WHITE);
  }
}

static void drawSwitchingIcon(int x, int y) {
  display.drawLine(x + 12, y + 0, x + 5, y + 13, SSD1306_WHITE);
  display.drawLine(x + 5, y + 13, x + 11, y + 13, SSD1306_WHITE);
  display.drawLine(x + 11, y + 13, x + 9, y + 26, SSD1306_WHITE);
  display.drawLine(x + 9, y + 26, x + 19, y + 10, SSD1306_WHITE);
  display.drawLine(x + 19, y + 10, x + 13, y + 10, SSD1306_WHITE);
}

static void drawUsbIcon(int x, int y) {
  display.drawCircle(x + 12, y + 4, 2, SSD1306_WHITE);
  display.drawLine(x + 12, y + 6, x + 12, y + 18, SSD1306_WHITE);
  display.drawLine(x + 12, y + 10, x + 7, y + 15, SSD1306_WHITE);
  display.drawLine(x + 12, y + 10, x + 17, y + 15, SSD1306_WHITE);
  display.drawTriangle(x + 10, y + 18, x + 14, y + 18, x + 12, y + 22, SSD1306_WHITE);
  display.drawRect(x + 5, y + 14, 4, 4, SSD1306_WHITE);
  display.drawCircle(x + 18, y + 16, 2, SSD1306_WHITE);
}

static void drawBatteryBar(int x, int y, int w, int h, int pct) {
  display.drawRoundRect(x, y, w, h, 3, SSD1306_WHITE);
  display.drawRect(x + w, y + (h / 2) - 3, 3, 6, SSD1306_WHITE);

  int innerW = w - 4;
  int fill = map(pct, 0, 100, 0, innerW);
  if (fill < 0) fill = 0;
  if (fill > innerW) fill = innerW;
  if (fill > 0) {
    display.fillRoundRect(x + 2, y + 2, fill, h - 4, 2, SSD1306_WHITE);
  }
}

static void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    oledReady = false;
    return;
  }

  oledReady = true;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("BlueWalker Online");
  display.setCursor(0, 16);
  display.println("Booting...");
  display.display();
}

static void updateDisplay() {
  if (!oledReady) return;
  if (millis() - lastDisplayUpdateMs < 500) return;
  lastDisplayUpdateMs = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BlueWalker Online");

  display.setCursor(0, 10);
  display.print("IP:");
  display.print(WiFi.localIP());

  if (!isUpsUsbConnected()) {
    drawUsbIcon(2, 26);

    display.setTextSize(1);
    display.setCursor(32, 28);
    display.println("USB CONNECTION");

    display.setCursor(32, 38);
    display.println("MISSING");

    display.setCursor(0, 56);
    display.print("Connect BlueWalker USB");
    display.display();
    return;
  }

  if (g_state.have_flags) {
    if (is_on_mains_flags(g_state.flags)) {
      drawPlugIcon(2, 24);
    } else if (is_on_battery_flags(g_state.flags)) {
      drawBatteryIcon(2, 24, g_state.have_charge ? g_state.charge_percent : 0);
    } else {
      drawSwitchingIcon(2, 24);
    }
  } else {
    drawSwitchingIcon(2, 24);
  }

  display.setTextSize(1);
  display.setCursor(32, 24);
  display.print(shortPowerState());

  display.setTextSize(2);
  display.setCursor(32, 36);
  if (g_state.have_charge) {
    display.print(g_state.charge_percent);
    display.print("%");
  } else {
    display.print("--%");
  }

  int pct = g_state.have_charge ? g_state.charge_percent : 0;
  drawBatteryBar(2, 52, 70, 10, pct);

  display.setTextSize(1);
  display.setCursor(78, 54);
  display.print(estimated_minutes_remaining());
  display.print(" min");

  display.display();
}

// ============================================================
// UNKNOWN TAG MEMORY
// ============================================================
static void maybe_log_unknown(uint8_t ep, const uint8_t *buf, uint8_t len) {
  LastUnknown *slot = (ep == EP1) ? &g_last_unknown_81 : &g_last_unknown_82;

  if (slot->valid &&
      slot->tag == buf[0] &&
      slot->len == len &&
      memcmp(slot->data, buf, len) == 0) {
    return;
  }

  slot->valid = true;
  slot->tag = buf[0];
  slot->len = len;
  memset(slot->data, 0, sizeof(slot->data));
  memcpy(slot->data, buf, len);

  Serial.printf("[%10lu ms] UNKNOWN TAG on EP 0x%02X len=%u data=",
                millis(), ep, len);
  print_hex(buf, len);
  Serial.println();
}

// ============================================================
// USB PACKET PARSING
// ============================================================
static void handle_packet(uint8_t ep, const uint8_t *buf, uint8_t len) {
  if (len < 2) return;

  switch (buf[0]) {
    case 0x32: {
      if (len >= 4) {
        uint32_t new_flags = (uint32_t)buf[1]
                           | ((uint32_t)buf[2] << 8)
                           | ((uint32_t)buf[3] << 16);

        if (!g_state.have_flags || g_state.flags != new_flags) {
          bool was_on_battery = g_state.have_flags && is_on_battery_flags(g_state.flags);
          bool now_on_battery = is_on_battery_flags(new_flags);

          g_state.flags = new_flags;
          g_state.have_flags = true;
          g_state.last_flags_ms = millis();

          if (!was_on_battery && now_on_battery) {
            g_state.battery_start_ms = millis();
          } else if (was_on_battery && !now_on_battery) {
            g_state.battery_start_ms = 0;
          }

          print_summary("flags changed");
          updateDisplay();
        }
      }
      break;
    }

    case 0x34: {
      if (len >= 2) {
        uint8_t new_charge = buf[1];
        if (!g_state.have_charge || g_state.charge_percent != new_charge) {
          g_state.charge_percent = new_charge;
          g_state.have_charge = true;
          g_state.last_charge_ms = millis();
          print_summary("charge changed");
          updateDisplay();
        }
      }
      break;
    }

    case 0x35: {
      if (len >= 3) {
        uint16_t new_metric = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
        g_state.battery_metric = new_metric;
        g_state.have_metric = true;
        g_state.last_metric_ms = millis();
        maybe_log_metric();
      }
      break;
    }

    default:
      maybe_log_unknown(ep, buf, len);
      break;
  }
}

static void transfer_cb(usb_transfer_t *transfer) {
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
      transfer->actual_num_bytes > 0) {
    handle_packet(transfer->bEndpointAddress, transfer->data_buffer, transfer->actual_num_bytes);
  } else if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    Serial.printf("[%10lu ms] EP 0x%02X transfer error: %d\n",
                  millis(), transfer->bEndpointAddress, transfer->status);
  }

  memset(transfer->data_buffer, 0, transfer->num_bytes);
  esp_err_t err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    Serial.printf("[%10lu ms] Resubmit failed on EP 0x%02X: %d\n",
                  millis(), transfer->bEndpointAddress, err);
  }
}

static esp_err_t start_interrupt_in(usb_transfer_t **xfer,
                                    usb_device_handle_t dev_hdl,
                                    uint8_t ep_addr,
                                    size_t size) {
  esp_err_t err = usb_host_transfer_alloc(size, 0, xfer);
  if (err != ESP_OK) {
    Serial.printf("usb_host_transfer_alloc failed for EP 0x%02X: %d\n", ep_addr, err);
    return err;
  }

  (*xfer)->device_handle = dev_hdl;
  (*xfer)->bEndpointAddress = ep_addr;
  (*xfer)->callback = transfer_cb;
  (*xfer)->context = nullptr;
  (*xfer)->num_bytes = size;
  memset((*xfer)->data_buffer, 0, size);

  err = usb_host_transfer_submit(*xfer);
  if (err != ESP_OK) {
    Serial.printf("usb_host_transfer_submit failed for EP 0x%02X: %d\n", ep_addr, err);
    return err;
  }

  Serial.printf("[%10lu ms] Started listening on EP 0x%02X\n", millis(), ep_addr);
  return ESP_OK;
}

static void free_transfers() {
  if (g_xfer81) {
    usb_host_transfer_free(g_xfer81);
    g_xfer81 = nullptr;
  }
  if (g_xfer82) {
    usb_host_transfer_free(g_xfer82);
    g_xfer82 = nullptr;
  }
}

static void close_device() {
  if (!g_dev_hdl) return;

  free_transfers();

  if (g_claimed) {
    usb_host_interface_release(g_client, g_dev_hdl, 0);
    usb_host_interface_release(g_client, g_dev_hdl, 1);
    g_claimed = false;
  }

  usb_host_device_close(g_client, g_dev_hdl);
  g_dev_hdl = nullptr;
}

static bool open_and_prepare_device(uint8_t dev_addr) {
  const usb_device_desc_t *desc = nullptr;
  esp_err_t err = usb_host_device_open(g_client, dev_addr, &g_dev_hdl);
  if (err != ESP_OK) {
    Serial.printf("usb_host_device_open failed: %d\n", err);
    g_dev_hdl = nullptr;
    return false;
  }

  err = usb_host_get_device_descriptor(g_dev_hdl, &desc);
  if (err != ESP_OK || !desc) {
    Serial.printf("usb_host_get_device_descriptor failed: %d\n", err);
    close_device();
    return false;
  }

  Serial.println();
  Serial.println("============== DEVICE OPENED ==============");
  Serial.printf("Address : %u\n", dev_addr);
  Serial.printf("VID     : 0x%04X\n", desc->idVendor);
  Serial.printf("PID     : 0x%04X\n", desc->idProduct);
  Serial.println("===========================================");

  if (desc->idVendor != UPS_VID || desc->idProduct != UPS_PID) {
    Serial.println("Not BlueWalker UPS, ignoring.");
    close_device();
    return false;
  }

  err = usb_host_interface_claim(g_client, g_dev_hdl, 0, 0);
  Serial.printf("Claim interface 0 => %d\n", err);
  if (err != ESP_OK) {
    close_device();
    return false;
  }

  err = usb_host_interface_claim(g_client, g_dev_hdl, 1, 0);
  Serial.printf("Claim interface 1 => %d\n", err);
  if (err != ESP_OK) {
    usb_host_interface_release(g_client, g_dev_hdl, 0);
    close_device();
    return false;
  }

  g_claimed = true;

  err = start_interrupt_in(&g_xfer81, g_dev_hdl, EP1, REPORT_SIZE);
  if (err != ESP_OK) {
    close_device();
    return false;
  }

  err = start_interrupt_in(&g_xfer82, g_dev_hdl, EP2, REPORT_SIZE);
  if (err != ESP_OK) {
    close_device();
    return false;
  }

  return true;
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
  (void)arg;

  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      g_dev_addr = event_msg->new_dev.address;
      g_dev_present = true;
      Serial.printf("[%10lu ms] NEW_DEV address=%u\n", millis(), g_dev_addr);
      updateDisplay();
      break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.printf("[%10lu ms] DEV_GONE\n", millis());
      g_dev_present = false;
      g_dev_addr = 0;
      close_device();
      updateDisplay();
      break;

    default:
      Serial.printf("[%10lu ms] Unhandled client event: %d\n", millis(), event_msg->event);
      break;
  }
}

void usb_lib_task(void *arg) {
  (void)arg;
  while (true) {
    uint32_t flags = 0;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK) {
      Serial.printf("[%10lu ms] usb_host_lib_handle_events err=%d\n", millis(), err);
    }
  }
}

void usb_client_task(void *arg) {
  (void)arg;

  usb_host_client_config_t config = {};
  config.is_synchronous = false;
  config.max_num_event_msg = 8;
  config.async.client_event_callback = client_event_cb;
  config.async.callback_arg = nullptr;

  esp_err_t err = usb_host_client_register(&config, &g_client);
  Serial.printf("usb_host_client_register => %d\n", err);
  if (err != ESP_OK) {
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    err = usb_host_client_handle_events(g_client, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("[%10lu ms] usb_host_client_handle_events err=%d\n", millis(), err);
    }

    if (g_dev_present && g_dev_addr != 0 && g_dev_hdl == nullptr) {
      open_and_prepare_device(g_dev_addr);
    }
  }
}

// ============================================================
// HTTP SERVER
// ============================================================
static String htmlPage() {
  String state = String(power_state_text(g_state.flags));
  String nut = nut_status();
  String charge = g_state.have_charge ? String(g_state.charge_percent) + "%" : "unknown";
  String metric = g_state.have_metric ? String(g_state.battery_metric) : "unknown";
  String inV = String(estimated_input_voltage(), 1) + " V";
  String outV = String(estimated_output_voltage(), 1) + " V";
  String load = String(estimated_load_percent()) + "%";
  String mins = String(estimated_minutes_remaining()) + " min";
  String ip = WiFi.localIP().toString();
  String badgeColor = statusColor();

  String html;
  html += "<!doctype html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>BlueWalker Online</title>";
  html += "<style>";
  html += ":root{--bg:#0b1220;--panel:#111827;--panel2:#0f172a;--text:#e5e7eb;--muted:#94a3b8;--border:#1f2937;}";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;background:radial-gradient(circle at top,#13213f,#0b1220 55%);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;}";
  html += ".wrap{max-width:1024px;margin:0 auto;padding:32px 20px 48px;}";
  html += ".hero{display:flex;justify-content:space-between;align-items:center;gap:16px;flex-wrap:wrap;margin-bottom:24px;}";
  html += ".hero-left{display:flex;gap:16px;align-items:center;}";
  html += ".hero-icon{width:56px;height:56px;border-radius:16px;background:linear-gradient(180deg,#1d4ed8,#1e40af);display:flex;align-items:center;justify-content:center;color:white;box-shadow:0 12px 30px rgba(37,99,235,.35)}";
  html += ".hero-icon svg{width:28px;height:28px}";
  html += ".title{font-size:34px;font-weight:800;letter-spacing:-0.03em;}";
  html += ".sub{color:var(--muted);font-size:14px;margin-top:6px;}";
  html += ".badge{display:inline-flex;align-items:center;gap:8px;padding:10px 14px;border-radius:999px;font-size:13px;font-weight:700;color:white;background:" + badgeColor + ";box-shadow:0 8px 24px rgba(0,0,0,.25)}";
  html += ".badge svg{width:16px;height:16px}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:16px;}";
  html += ".card{background:linear-gradient(180deg,var(--panel),var(--panel2));border:1px solid var(--border);border-radius:22px;padding:18px;box-shadow:0 10px 30px rgba(0,0,0,.25)}";
  html += ".card-top{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}";
  html += ".icon{width:34px;height:34px;border-radius:12px;background:#0b1220;border:1px solid #243041;display:flex;align-items:center;justify-content:center;color:#93c5fd}";
  html += ".icon svg{width:18px;height:18px}";
  html += ".label{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted)}";
  html += ".value{font-size:30px;font-weight:800;line-height:1.1}";
  html += ".small{font-size:14px;color:#cbd5e1;margin-top:8px}";
  html += ".footer{margin-top:26px;color:var(--muted);font-size:13px;display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap}";
  html += "a{color:#60a5fa;text-decoration:none}";
  html += "</style></head><body>";
  html += "<div class='wrap'>";

  html += "<div class='hero'>";
  html += "<div class='hero-left'>";
  html += "<div class='hero-icon'>" + htmlIconPower() + "</div>";
  html += "<div>";
  html += "<div class='title'>BlueWalker Online</div>";
  html += "<div class='sub'>ESP32 USB UPS gateway with HTTP, NUT and SNMP</div>";
  html += "</div></div>";
  html += "<div class='badge'>" + htmlIconPower() + state + "</div>";
  html += "</div>";

  html += "<div class='grid'>";
  html += "<div class='card'><div class='card-top'><div class='label'>UPS status</div><div class='icon'>" + htmlIconPower() + "</div></div><div class='value'>" + nut + "</div><div class='small'>Flags: 0x" + String((unsigned long)g_state.flags, HEX) + "</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Charge percent</div><div class='icon'>" + htmlIconBattery() + "</div></div><div class='value'>" + charge + "</div><div class='small'>Reported from USB tag 0x34</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Battery metric</div><div class='icon'>" + htmlIconBattery() + "</div></div><div class='value'>" + metric + "</div><div class='small'>Raw metric from USB tag 0x35</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Load estimate</div><div class='icon'>" + htmlIconLoad() + "</div></div><div class='value'>" + load + "</div><div class='small'>Current estimated UPS load</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Input voltage</div><div class='icon'>" + htmlIconPower() + "</div></div><div class='value'>" + inV + "</div><div class='small'>Estimated input voltage</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Output voltage</div><div class='icon'>" + htmlIconPower() + "</div></div><div class='value'>" + outV + "</div><div class='small'>Nominal output voltage</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Runtime estimate</div><div class='icon'>" + htmlIconClock() + "</div></div><div class='value'>" + mins + "</div><div class='small'>Approximate remaining runtime</div></div>";
  html += "<div class='card'><div class='card-top'><div class='label'>Network</div><div class='icon'>" + htmlIconWifi() + "</div></div><div class='value' style='font-size:22px'>" + ip + "</div><div class='small'><a href='/json'>Open JSON endpoint</a></div></div>";
  html += "</div>";

  html += "<div class='footer'>";
  html += "<div>Model: " + String(DEVICE_MODEL) + " · Manufacturer: " + String(DEVICE_MFR) + "</div>";
  html += "<div>Driver: " + String(DRIVER_NAME) + " " + String(DRIVER_VERSION) + "</div>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

static void handleHttpRoot() {
  httpServer.send(200, "text/html; charset=utf-8", htmlPage());
}

static void handleHttpJson() {
  String json = "{";
  json += "\"name\":\"" + String(DEVICE_MODEL) + "\",";
  json += "\"manufacturer\":\"" + String(DEVICE_MFR) + "\",";
  json += "\"power_state\":\"" + String(power_state_text(g_state.flags)) + "\",";
  json += "\"ups_status\":\"" + nut_status() + "\",";
  json += "\"charge_percent\":" + String(g_state.have_charge ? g_state.charge_percent : 0) + ",";
  json += "\"battery_metric\":" + String(g_state.have_metric ? g_state.battery_metric : 0) + ",";
  json += "\"input_voltage\":" + String(estimated_input_voltage(), 1) + ",";
  json += "\"output_voltage\":" + String(estimated_output_voltage(), 1) + ",";
  json += "\"load_estimate\":" + String(estimated_load_percent()) + ",";
  json += "\"minutes_remaining\":" + String(estimated_minutes_remaining()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  httpServer.send(200, "application/json", json);
}

static void setupHttp() {
  httpServer.on("/", handleHttpRoot);
  httpServer.on("/json", handleHttpJson);
  httpServer.begin();
  Serial.println("HTTP server started on port 80");
}

// ============================================================
// NUT SERVER
// ============================================================
static String nutGetVar(const String &var) {
  if (var == "device.mfr") return DEVICE_MFR;
  if (var == "device.model") return DEVICE_MODEL;
  if (var == "device.serial") return "unknown";
  if (var == "device.type") return "ups";
  if (var == "driver.name") return DRIVER_NAME;
  if (var == "driver.version") return DRIVER_VERSION;
  if (var == "ups.mfr") return DEVICE_MFR;
  if (var == "ups.model") return DEVICE_MODEL;
  if (var == "ups.status") return nut_status();
  if (var == "battery.charge") return String(g_state.have_charge ? g_state.charge_percent : 0);
  if (var == "battery.metric") return String(g_state.have_metric ? g_state.battery_metric : 0);
  if (var == "battery.runtime") return String(estimated_minutes_remaining() * 60);
  if (var == "input.voltage") return String(estimated_input_voltage(), 1);
  if (var == "output.voltage") return String(estimated_output_voltage(), 1);
  if (var == "ups.load") return String(estimated_load_percent());
  if (var == "ups.realpower.nominal") return "800";
  if (var == "ups.firmware") return "esp32";
  return "";
}

static void nutSendLine(WiFiClient &client, const String &line) {
  client.print(line);
  client.print("\n");
}

static void nutHandleCommand(WiFiClient &client, String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "VER" || line == "NETVER") {
    nutSendLine(client, "NETVER 1.0");
    return;
  }

  if (line == "HELP") {
    nutSendLine(client, "OK Commands: VER NETVER LIST UPS LIST VAR GET VAR");
    return;
  }

  if (line == "LIST UPS") {
    nutSendLine(client, "BEGIN LIST UPS");
    nutSendLine(client, "UPS " + String(NUT_UPS_NAME) + " \"" + String(DEVICE_MODEL) + "\"");
    nutSendLine(client, "END LIST UPS");
    return;
  }

  if (line == "LIST VAR ups") {
    nutSendLine(client, "BEGIN LIST VAR " + String(NUT_UPS_NAME));

    const char *vars[] = {
      "device.mfr", "device.model", "device.serial", "device.type",
      "driver.name", "driver.version", "ups.mfr", "ups.model",
      "ups.status", "battery.charge", "battery.metric", "battery.runtime",
      "input.voltage", "output.voltage", "ups.load", "ups.realpower.nominal",
      "ups.firmware"
    };

    for (auto v : vars) {
      String value = nutGetVar(v);
      nutSendLine(client, "VAR " + String(NUT_UPS_NAME) + " " + String(v) + " \"" + value + "\"");
    }

    nutSendLine(client, "END LIST VAR " + String(NUT_UPS_NAME));
    return;
  }

  if (line.startsWith("GET VAR ")) {
    int p1 = line.indexOf(' ');
    int p2 = line.indexOf(' ', p1 + 1);
    int p3 = line.indexOf(' ', p2 + 1);

    if (p3 < 0) {
      nutSendLine(client, "ERR INVALID-ARGUMENT");
      return;
    }

    String upsName = line.substring(p2 + 1, p3);
    String var = line.substring(p3 + 1);

    if (upsName != NUT_UPS_NAME) {
      nutSendLine(client, "ERR UNKNOWN-UPS");
      return;
    }

    String value = nutGetVar(var);
    if (value.length() == 0) {
      nutSendLine(client, "ERR VAR-NOT-SUPPORTED");
      return;
    }

    nutSendLine(client, "VAR " + upsName + " " + var + " \"" + value + "\"");
    return;
  }

  nutSendLine(client, "ERR UNKNOWN-COMMAND");
}

static void handleNutClients() {
  WiFiClient client = nutServer.available();
  if (!client) return;

  unsigned long start = millis();
  String line;

  while (client.connected() && millis() - start < 30000) {
    while (client.available()) {
      char c = (char)client.read();
      if (c == '\r') continue;
      if (c == '\n') {
        nutHandleCommand(client, line);
        line = "";
        start = millis();
      } else {
        line += c;
      }
    }
    delay(2);
  }

  client.stop();
}

// ============================================================
// SNMP
// ============================================================
static int oidCompare(const uint32_t *a, size_t aLen, const uint32_t *b, size_t bLen) {
  size_t minLen = (aLen < bLen) ? aLen : bLen;
  for (size_t i = 0; i < minLen; i++) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  if (aLen < bLen) return -1;
  if (aLen > bLen) return 1;
  return 0;
}

static const SnmpOidEntry *findExactOid(const uint32_t *oid, size_t oidLen) {
  for (const auto &entry : SNMP_OIDS) {
    if (oidCompare(entry.oid, entry.oidLen, oid, oidLen) == 0) return &entry;
  }
  return nullptr;
}

static const SnmpOidEntry *findNextOid(const uint32_t *oid, size_t oidLen) {
  const SnmpOidEntry *best = nullptr;
  for (const auto &entry : SNMP_OIDS) {
    if (oidCompare(entry.oid, entry.oidLen, oid, oidLen) > 0) {
      if (!best || oidCompare(entry.oid, entry.oidLen, best->oid, best->oidLen) < 0) {
        best = &entry;
      }
    }
  }
  return best;
}

static SnmpValue snmpValueForOid(const SnmpOidEntry *entry) {
  SnmpValue v{};
  if (!entry) {
    v.type = SVT_STRING;
    v.strValue = "";
    return v;
  }

  if (entry->oid == OID_sysDescr) {
    v.type = SVT_STRING;
    v.strValue = DEVICE_DESC;
    return v;
  }
  if (entry->oid == OID_sysUpTime) {
    v.type = SVT_TIMETICKS;
    v.intValue = millis() / 10;
    return v;
  }
  if (entry->oid == OID_upsIdentManufacturer) {
    v.type = SVT_STRING;
    v.strValue = DEVICE_MFR;
    return v;
  }
  if (entry->oid == OID_upsIdentModel) {
    v.type = SVT_STRING;
    v.strValue = DEVICE_MODEL;
    return v;
  }
  if (entry->oid == OID_upsIdentSoftwareVersion) {
    v.type = SVT_STRING;
    v.strValue = DRIVER_VERSION;
    return v;
  }
  if (entry->oid == OID_upsBatteryStatus) {
    v.type = SVT_INTEGER;
    v.intValue = snmp_battery_status();
    return v;
  }
  if (entry->oid == OID_upsSecondsOnBattery) {
    v.type = SVT_INTEGER;
    v.intValue = (int32_t)seconds_on_battery();
    return v;
  }
  if (entry->oid == OID_upsEstimatedMinutes) {
    v.type = SVT_INTEGER;
    v.intValue = estimated_minutes_remaining();
    return v;
  }
  if (entry->oid == OID_upsEstimatedCharge) {
    v.type = SVT_INTEGER;
    v.intValue = g_state.have_charge ? g_state.charge_percent : 0;
    return v;
  }
  if (entry->oid == OID_upsInputNumLines) {
    v.type = SVT_INTEGER;
    v.intValue = 1;
    return v;
  }
  if (entry->oid == OID_upsInputVoltage) {
    v.type = SVT_INTEGER;
    v.intValue = (int)round(estimated_input_voltage());
    return v;
  }
  if (entry->oid == OID_upsOutputSource) {
    v.type = SVT_INTEGER;
    v.intValue = snmp_output_source();
    return v;
  }
  if (entry->oid == OID_upsOutputVoltage) {
    v.type = SVT_INTEGER;
    v.intValue = (int)round(estimated_output_voltage());
    return v;
  }
  if (entry->oid == OID_upsOutputPercentLoad) {
    v.type = SVT_INTEGER;
    v.intValue = estimated_load_percent();
    return v;
  }

  v.type = SVT_STRING;
  v.strValue = "";
  return v;
}

static bool readLength(const uint8_t *in, int inLen, int &pos, int &len) {
  if (pos >= inLen) return false;
  uint8_t b = in[pos++];
  if ((b & 0x80) == 0) {
    len = b;
    return true;
  }
  int numBytes = b & 0x7F;
  if (numBytes < 1 || numBytes > 2 || pos + numBytes > inLen) return false;
  len = 0;
  for (int i = 0; i < numBytes; i++) {
    len = (len << 8) | in[pos++];
  }
  return true;
}

static bool expectTag(const uint8_t *in, int inLen, int &pos, uint8_t tag, int &len) {
  if (pos >= inLen || in[pos++] != tag) return false;
  return readLength(in, inLen, pos, len);
}

static bool parseInteger(const uint8_t *in, int inLen, int &pos, int32_t &value) {
  int len = 0;
  if (!expectTag(in, inLen, pos, 0x02, len)) return false;
  if (len < 1 || len > 4 || pos + len > inLen) return false;
  value = (int8_t)in[pos++];
  for (int i = 1; i < len; i++) value = (value << 8) | in[pos++];
  return true;
}

static bool parseOctetString(const uint8_t *in, int inLen, int &pos, String &s) {
  int len = 0;
  if (!expectTag(in, inLen, pos, 0x04, len)) return false;
  if (len < 0 || pos + len > inLen) return false;
  s = "";
  for (int i = 0; i < len; i++) s += (char)in[pos++];
  return true;
}

static bool parseOid(const uint8_t *in, int inLen, int &pos, uint32_t *oid, size_t &oidLen) {
  int len = 0;
  if (!expectTag(in, inLen, pos, 0x06, len)) return false;
  if (len < 1 || pos + len > inLen) return false;

  int end = pos + len;
  uint8_t first = in[pos++];
  oid[0] = first / 40;
  oid[1] = first % 40;
  oidLen = 2;

  uint32_t value = 0;
  while (pos < end) {
    uint8_t b = in[pos++];
    value = (value << 7) | (b & 0x7F);
    if ((b & 0x80) == 0) {
      if (oidLen < 32) oid[oidLen++] = value;
      value = 0;
    }
  }
  return true;
}

static bool skipTLV(const uint8_t *in, int inLen, int &pos) {
  if (pos >= inLen) return false;
  pos++;
  int len = 0;
  if (!readLength(in, inLen, pos, len)) return false;
  if (pos + len > inLen) return false;
  pos += len;
  return true;
}

static void appendLength(uint8_t *out, int &pos, int len) {
  if (len < 128) {
    out[pos++] = (uint8_t)len;
  } else {
    out[pos++] = 0x81;
    out[pos++] = (uint8_t)len;
  }
}

static void appendBytes(uint8_t *out, int &pos, const uint8_t *data, int len) {
  memcpy(out + pos, data, len);
  pos += len;
}

static void appendTLV(uint8_t *out, int &pos, uint8_t tag, const uint8_t *data, int len) {
  out[pos++] = tag;
  appendLength(out, pos, len);
  appendBytes(out, pos, data, len);
}

static void encodeIntegerValue(uint8_t *out, int &len, int32_t value) {
  uint8_t tmp[5];
  int pos = 0;

  if (value >= 0 && value <= 0x7F) {
    tmp[pos++] = (uint8_t)value;
  } else if (value >= 0 && value <= 0x7FFF) {
    tmp[pos++] = (uint8_t)((value >> 8) & 0xFF);
    tmp[pos++] = (uint8_t)(value & 0xFF);
    if (tmp[0] & 0x80) {
      memmove(tmp + 1, tmp, pos);
      tmp[0] = 0x00;
      pos++;
    }
  } else {
    tmp[pos++] = (uint8_t)((value >> 24) & 0xFF);
    tmp[pos++] = (uint8_t)((value >> 16) & 0xFF);
    tmp[pos++] = (uint8_t)((value >> 8) & 0xFF);
    tmp[pos++] = (uint8_t)(value & 0xFF);
    while (pos > 1 && tmp[0] == 0x00 && (tmp[1] & 0x80) == 0) {
      memmove(tmp, tmp + 1, --pos);
    }
  }

  memcpy(out, tmp, pos);
  len = pos;
}

static void encodeOidValue(uint8_t *out, int &len, const uint32_t *oid, size_t oidLen) {
  int pos = 0;
  out[pos++] = (uint8_t)(oid[0] * 40 + oid[1]);

  for (size_t i = 2; i < oidLen; i++) {
    uint32_t v = oid[i];
    uint8_t stack[5];
    int sp = 0;
    stack[sp++] = v & 0x7F;
    v >>= 7;
    while (v > 0) {
      stack[sp++] = 0x80 | (v & 0x7F);
      v >>= 7;
    }
    while (sp--) out[pos++] = stack[sp];
  }

  len = pos;
}

static void buildSnmpResponse(uint8_t version,
                              const String &community,
                              int32_t requestId,
                              const SnmpOidEntry *oidEntry,
                              const SnmpValue &value,
                              uint8_t *out,
                              int &outLen,
                              bool errorNoSuchName) {
  uint8_t varValue[128];
  int varValueLen = 0;
  uint8_t oidEnc[64];
  int oidEncLen = 0;
  encodeOidValue(oidEnc, oidEncLen,
                 oidEntry ? oidEntry->oid : OID_sysDescr,
                 oidEntry ? oidEntry->oidLen : (sizeof(OID_sysDescr) / sizeof(uint32_t)));

  if (!errorNoSuchName) {
    if (value.type == SVT_INTEGER || value.type == SVT_TIMETICKS) {
      encodeIntegerValue(varValue, varValueLen, value.intValue);
    } else {
      varValueLen = value.strValue.length();
      memcpy(varValue, value.strValue.c_str(), varValueLen);
    }
  }

  uint8_t vb[256];
  int vbPos = 0;

  appendTLV(vb, vbPos, 0x06, oidEnc, oidEncLen);

  if (errorNoSuchName) {
    uint8_t nullByte = 0;
    appendTLV(vb, vbPos, 0x05, &nullByte, 0);
  } else if (value.type == SVT_INTEGER) {
    appendTLV(vb, vbPos, 0x02, varValue, varValueLen);
  } else if (value.type == SVT_TIMETICKS) {
    appendTLV(vb, vbPos, 0x43, varValue, varValueLen);
  } else {
    appendTLV(vb, vbPos, 0x04, varValue, varValueLen);
  }

  uint8_t vbSeq[300];
  int vbSeqPos = 0;
  appendTLV(vbSeq, vbSeqPos, 0x30, vb, vbPos);

  uint8_t vbList[320];
  int vbListPos = 0;
  appendTLV(vbList, vbListPos, 0x30, vbSeq, vbSeqPos);

  uint8_t reqIdEnc[8];
  int reqIdLen = 0;
  encodeIntegerValue(reqIdEnc, reqIdLen, requestId);

  uint8_t errStatEnc[8];
  int errStatLen = 0;
  encodeIntegerValue(errStatEnc, errStatLen, errorNoSuchName ? 2 : 0);

  uint8_t errIdxEnc[8];
  int errIdxLen = 0;
  encodeIntegerValue(errIdxEnc, errIdxLen, errorNoSuchName ? 1 : 0);

  uint8_t pduBody[512];
  int pduBodyPos = 0;
  appendTLV(pduBody, pduBodyPos, 0x02, reqIdEnc, reqIdLen);
  appendTLV(pduBody, pduBodyPos, 0x02, errStatEnc, errStatLen);
  appendTLV(pduBody, pduBodyPos, 0x02, errIdxEnc, errIdxLen);
  appendBytes(pduBody, pduBodyPos, vbList, vbListPos);

  uint8_t pdu[540];
  int pduPos = 0;
  appendTLV(pdu, pduPos, 0xA2, pduBody, pduBodyPos);

  uint8_t verEnc[8];
  int verLen = 0;
  encodeIntegerValue(verEnc, verLen, version);

  uint8_t msgBody[640];
  int msgBodyPos = 0;
  appendTLV(msgBody, msgBodyPos, 0x02, verEnc, verLen);
  appendTLV(msgBody, msgBodyPos, 0x04, (const uint8_t *)community.c_str(), community.length());
  appendBytes(msgBody, msgBodyPos, pdu, pduPos);

  outLen = 0;
  appendTLV(out, outLen, 0x30, msgBody, msgBodyPos);
}

static void handleSnmpPacket() {
  int packetSize = snmpUdp.parsePacket();
  if (packetSize <= 0) return;

  uint8_t in[512];
  int len = snmpUdp.read(in, sizeof(in));
  if (len <= 0) return;

  int pos = 0;
  int seqLen = 0;
  if (!expectTag(in, len, pos, 0x30, seqLen)) return;

  int32_t version = 0;
  if (!parseInteger(in, len, pos, version)) return;

  String community;
  if (!parseOctetString(in, len, pos, community)) return;
  if (community != SNMP_COMMUNITY) return;

  if (pos >= len) return;
  uint8_t pduType = in[pos++];
  int pduLen = 0;
  if (!readLength(in, len, pos, pduLen)) return;

  if (pduType != 0xA0 && pduType != 0xA1) return;

  int32_t requestId = 0;
  int32_t errorStatus = 0;
  int32_t errorIndex = 0;
  if (!parseInteger(in, len, pos, requestId)) return;
  if (!parseInteger(in, len, pos, errorStatus)) return;
  if (!parseInteger(in, len, pos, errorIndex)) return;

  int vbListLen = 0;
  if (!expectTag(in, len, pos, 0x30, vbListLen)) return;

  int vbLen = 0;
  if (!expectTag(in, len, pos, 0x30, vbLen)) return;

  uint32_t reqOid[32];
  size_t reqOidLen = 0;
  if (!parseOid(in, len, pos, reqOid, reqOidLen)) return;

  if (!skipTLV(in, len, pos)) return;

  const SnmpOidEntry *target = nullptr;
  bool errorNoSuchName = false;

  if (pduType == 0xA0) {
    target = findExactOid(reqOid, reqOidLen);
    if (!target) errorNoSuchName = true;
  } else if (pduType == 0xA1) {
    target = findNextOid(reqOid, reqOidLen);
    if (!target) {
      target = findExactOid(OID_sysDescr, sizeof(OID_sysDescr) / sizeof(uint32_t));
      errorNoSuchName = true;
    }
  }

  SnmpValue value = snmpValueForOid(target);

  uint8_t out[768];
  int outLen = 0;
  buildSnmpResponse((uint8_t)version, community, requestId, target, value, out, outLen, errorNoSuchName);

  snmpUdp.beginPacket(snmpUdp.remoteIP(), snmpUdp.remotePort());
  snmpUdp.write(out, outLen);
  snmpUdp.endPacket();
}

// ============================================================
// WIFI
// ============================================================
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi SSID %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("BOOT");
  Serial.println("Starting BlueWalker Online monitor + HTTP + NUT + SNMP + OLED...");

  setupDisplay();

  connectWiFi();
  delay(300);
  updateDisplay();

  setupHttp();
  nutServer.begin();
  Serial.println("NUT server started on port 3493");

  snmpUdp.begin(161);
  Serial.println("SNMP server started on port 161");

  usb_host_config_t host_config = {};
  host_config.skip_phy_setup = false;
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;

  esp_err_t err = usb_host_install(&host_config);
  Serial.printf("usb_host_install => %d\n", err);
  if (err != ESP_OK) {
    Serial.println("USB host install failed");
    return;
  }

  xTaskCreate(usb_lib_task, "usb_lib_task", 4096, nullptr, 20, nullptr);
  xTaskCreate(usb_client_task, "usb_client_task", 8192, nullptr, 20, nullptr);

  Serial.println("Waiting for BlueWalker UPS...");
}

void loop() {
  httpServer.handleClient();
  handleNutClients();
  handleSnmpPacket();
  updateDisplay();
  delay(2);
}