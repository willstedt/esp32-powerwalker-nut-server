#include <Arduino.h>
#include <string.h>
#include <WiFi.h>
#include <WebServer.h>

#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

// =========================
// WiFi
// =========================
const char* WIFI_SSID = "willstedt";
const char* WIFI_PASSWORD = "will13734";

// =========================
// UPS identity
// =========================
static const uint16_t UPS_VID = 0x0665;
static const uint16_t UPS_PID = 0x5161;
static const char* UPS_NAME = "ups";
static const char* UPS_DESC = "ESP32 BlueWalker UPS";
static const char* DRIVER_NAME = "esp32-usb-ups";
static const char* DRIVER_VERSION = "0.1";

// =========================
// USB endpoints
// =========================
static const uint8_t EP1 = 0x81;
static const uint8_t EP2 = 0x82;
static const size_t REPORT_SIZE = 8;

// =========================
// Globals
// =========================
WebServer httpServer(80);
WiFiServer nutServer(3493);

usb_host_client_handle_t g_client = nullptr;
usb_device_handle_t g_dev_hdl = nullptr;
usb_transfer_t *g_xfer81 = nullptr;
usb_transfer_t *g_xfer82 = nullptr;

volatile uint8_t g_dev_addr = 0;
volatile bool g_dev_present = false;
bool g_claimed = false;

struct UpsState {
  bool have_flags = false;
  bool have_charge = false;
  bool have_metric = false;

  uint32_t flags = 0;
  uint8_t charge_percent = 0;     // from tag 0x34
  uint16_t battery_metric = 0;    // from tag 0x35

  unsigned long last_flags_ms = 0;
  unsigned long last_charge_ms = 0;
  unsigned long last_metric_ms = 0;
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

// =========================
// Utility
// =========================
static void print_hex(const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    Serial.printf("%02X ", buf[i]);
  }
}

static const char *power_state_text(uint32_t flags) {
  if (flags == 0x112000) return "ON MAINS";
  if (flags == 0x002000) return "ON BATTERY";
  if (flags == 0x102000) return "SWITCHING";
  if (flags == 0x012000) return "SWITCHING";
  return "UNKNOWN";
}

static String nut_status() {
  if (!g_state.have_flags) return "WAIT";

  if (g_state.flags == 0x112000) return "OL CHRG";
  if (g_state.flags == 0x002000) return "OB DISCHRG";
  if (g_state.flags == 0x102000 || g_state.flags == 0x012000) return "OB";
  return "WAIT";
}

// Grov heuristik: mappa BatteryMetric till ungefärlig inspänning.
// Din UPS visade ~230 V när metric låg kring ~2920.
// Senare såg vi ~2660 osv under batteridrift.
// Detta är bara en approximation för att ge ett vettigt fält.
static float estimated_input_voltage() {
  if (!g_state.have_metric) return 0.0f;
  return g_state.battery_metric * (230.0f / 2920.0f);
}

// Du nämnde att displayen visar LOAD 10-11%.
// Vi har inte säkert dekodat load från USB, så här lämnar vi
// ett placeholdervärde baserat på senaste kända observation.
// Byt gärna senare när vi hittar den riktiga taggen.
static int estimated_load_percent() {
  return 11;
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

static bool same_state(const UpsState &a, const UpsState &b) {
  return memcmp(&a, &b, sizeof(UpsState)) == 0;
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
    Serial.printf("EstInputVolt   : %.1f V\n", estimated_input_voltage());
  } else {
    Serial.println("BatteryMetric  : unknown");
  }

  Serial.printf("EstLoad        : %d %%\n", estimated_load_percent());

  Serial.println("========================================");
}

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

// =========================
// USB parsing
// =========================
static void handle_packet(uint8_t ep, const uint8_t *buf, uint8_t len) {
  if (len < 2) return;

  switch (buf[0]) {
    case 0x32: {
      if (len >= 4) {
        uint32_t new_flags = (uint32_t)buf[1]
                           | ((uint32_t)buf[2] << 8)
                           | ((uint32_t)buf[3] << 16);

        if (!g_state.have_flags || g_state.flags != new_flags) {
          g_state.flags = new_flags;
          g_state.have_flags = true;
          g_state.last_flags_ms = millis();
          print_summary("flags changed");
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
    handle_packet(transfer->bEndpointAddress,
                  transfer->data_buffer,
                  transfer->actual_num_bytes);
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
  esp_err_t err;

  err = usb_host_device_open(g_client, dev_addr, &g_dev_hdl);
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
      break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.printf("[%10lu ms] DEV_GONE\n", millis());
      g_dev_present = false;
      g_dev_addr = 0;
      close_device();
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

  usb_host_client_config_t config = {
    .is_synchronous = false,
    .max_num_event_msg = 8,
    .async = {
      .client_event_callback = client_event_cb,
      .callback_arg = nullptr,
    }
  };

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

// =========================
// HTTP
// =========================
static void handleHttpRoot() {
  String html;
  html += "<html><body><h1>ESP32 UPS</h1>";
  html += "<p>PowerState: " + String(power_state_text(g_state.flags)) + "</p>";
  html += "<p>NUT Status: " + nut_status() + "</p>";
  html += "<p>ChargePercent: " + String(g_state.have_charge ? g_state.charge_percent : 0) + "</p>";
  html += "<p>BatteryMetric: " + String(g_state.have_metric ? g_state.battery_metric : 0) + "</p>";
  html += "<p>InputVoltageEstimate: " + String(estimated_input_voltage(), 1) + "</p>";
  html += "<p>LoadEstimate: " + String(estimated_load_percent()) + "</p>";
  html += "</body></html>";
  httpServer.send(200, "text/html", html);
}

static void handleHttpJson() {
  String json = "{";
  json += "\"power_state\":\"" + String(power_state_text(g_state.flags)) + "\",";
  json += "\"ups_status\":\"" + nut_status() + "\",";
  json += "\"charge_percent\":" + String(g_state.have_charge ? g_state.charge_percent : 0) + ",";
  json += "\"battery_metric\":" + String(g_state.have_metric ? g_state.battery_metric : 0) + ",";
  json += "\"input_voltage_estimate\":" + String(estimated_input_voltage(), 1) + ",";
  json += "\"load_estimate\":" + String(estimated_load_percent());
  json += "}";
  httpServer.send(200, "application/json", json);
}

static void setupHttp() {
  httpServer.on("/", handleHttpRoot);
  httpServer.on("/json", handleHttpJson);
  httpServer.begin();
  Serial.println("HTTP server started on port 80");
}

// =========================
// Minimal NUT server
// =========================
static String nutGetVar(const String &var) {
  if (var == "device.mfr") return "BlueWalker";
  if (var == "device.model") return "VI 800 SCL";
  if (var == "device.serial") return "unknown";
  if (var == "device.type") return "ups";
  if (var == "driver.name") return DRIVER_NAME;
  if (var == "driver.version") return DRIVER_VERSION;
  if (var == "ups.mfr") return "BlueWalker";
  if (var == "ups.model") return "VI 800 SCL";
  if (var == "ups.status") return nut_status();
  if (var == "battery.charge") return String(g_state.have_charge ? g_state.charge_percent : 0);
  if (var == "battery.metric") return String(g_state.have_metric ? g_state.battery_metric : 0);
  if (var == "input.voltage") return String(estimated_input_voltage(), 1);
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

  Serial.printf("NUT CMD: %s\n", line.c_str());

  if (line == "VER") {
    nutSendLine(client, "NETVER 1.0");
    return;
  }

  if (line == "NETVER") {
    nutSendLine(client, "NETVER 1.0");
    return;
  }

  if (line == "HELP") {
    nutSendLine(client, "OK Commands: VER NETVER LIST UPS LIST VAR GET VAR");
    return;
  }

  if (line == "LIST UPS") {
    nutSendLine(client, "BEGIN LIST UPS");
    nutSendLine(client, "UPS " + String(UPS_NAME) + " \"" + String(UPS_DESC) + "\"");
    nutSendLine(client, "END LIST UPS");
    return;
  }

  if (line == "LIST VAR ups") {
    nutSendLine(client, "BEGIN LIST VAR " + String(UPS_NAME));

    const char* vars[] = {
      "device.mfr",
      "device.model",
      "device.serial",
      "device.type",
      "driver.name",
      "driver.version",
      "ups.mfr",
      "ups.model",
      "ups.status",
      "battery.charge",
      "battery.metric",
      "input.voltage",
      "ups.load",
      "ups.realpower.nominal",
      "ups.firmware"
    };

    for (auto v : vars) {
      String value = nutGetVar(v);
      nutSendLine(client, "VAR " + String(UPS_NAME) + " " + String(v) + " \"" + value + "\"");
    }

    nutSendLine(client, "END LIST VAR " + String(UPS_NAME));
    return;
  }

  if (line.startsWith("GET VAR ")) {
    // GET VAR ups battery.charge
    int p1 = line.indexOf(' ');
    int p2 = line.indexOf(' ', p1 + 1);
    int p3 = line.indexOf(' ', p2 + 1);

    if (p3 < 0) {
      nutSendLine(client, "ERR INVALID-ARGUMENT");
      return;
    }

    String upsName = line.substring(p2 + 1, p3);
    String var = line.substring(p3 + 1);

    if (upsName != UPS_NAME) {
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

  Serial.println("NUT client connected");
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
  Serial.println("NUT client disconnected");
}

// =========================
// WiFi
// =========================
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

// =========================
// Setup / loop
// =========================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("BOOT");
  Serial.println("Starting ESP32 BlueWalker UPS monitor + HTTP + NUT...");

  connectWiFi();
  setupHttp();
  nutServer.begin();
  Serial.println("NUT server started on port 3493");

  usb_host_config_t host_config = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };

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
  delay(2);
}