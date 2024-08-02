#include "mgos.h"
#include "mgos_gpio.h"
#include "mgos_sys_config.h"
#include "mgos_mqtt.h"
#include "mgos_wifi.h"
#include "esp_wifi.h"
#include "frozen.h"
#include "mgos_rpc.h"
#include "mgos_dash.h"
#include "mgos_i2c.h"

// Global variables
static mgos_timer_id report_timer_id;
static mgos_timer_id status_check_timer_id;
static char rpc_topic_pub[100];
static char rpc_topic_sub[100];
static char confirmation_topic[100];
static char status_topic[100];

// Function to save total bag count and total gift count to JSON
static void save_counts_to_json() {
  const char *filename = mgos_sys_config_get_coin_count_file();
  double total_bag = mgos_sys_config_get_app_total_bag();
  double total_gift = mgos_sys_config_get_app_total_gift();
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
  char *json_str = json_asprintf("{\"total_bag\": %.2f, \"total_gift\": %.2f, \"time\": \"%s\"}", total_bag, total_gift, time_str);
  if (json_str != NULL) {
    FILE *f = fopen(filename, "w");
    if (f != NULL) {
      fwrite(json_str, 1, strlen(json_str), f);
      fclose(f);
      LOG(LL_INFO, ("Counts saved to JSON: bag=%.2f, gift=%.2f", total_bag, total_gift));
    } else {
      LOG(LL_ERROR, ("Failed to open file for writing"));
    }
    free(json_str);
  } else {
    LOG(LL_ERROR, ("Failed to allocate memory for JSON string"));
  }
}

// Function to load total bag count and total gift count from JSON
static void load_counts_from_json() {
  const char *filename = mgos_sys_config_get_coin_count_file();
  FILE *f = fopen(filename, "r");
  if (f != NULL) {
    char buffer[100];
    int len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[len] = '\0';
    fclose(f);
    double total_bag, total_gift;
    json_scanf(buffer, len, "{\"total_bag\": %lf, \"total_gift\": %lf}", &total_bag, &total_gift);
    mgos_sys_config_set_app_total_bag(total_bag);
    mgos_sys_config_set_app_total_gift(total_gift);
    LOG(LL_INFO, ("Counts loaded from JSON: bag=%.2f, gift=%.2f", total_bag, total_gift));
  } else {
    LOG(LL_ERROR, ("Failed to open file for reading, starting from initial value"));
  }
}

// ISR for coin insertion
static void coin_isr(int pin, void *arg) {
  double total_bag = mgos_sys_config_get_app_total_bag() + 1.0;
  mgos_sys_config_set_app_total_bag(total_bag);
  LOG(LL_INFO, ("Coin inserted! Total bag count: %.2f", total_bag));
  save_counts_to_json();
  (void) pin;
  (void) arg;
}

// ISR for gift insertion
static void gift_isr(int pin, void *arg) {
  double total_gift = mgos_sys_config_get_app_total_gift() + 1.0;
  mgos_sys_config_set_app_total_gift(total_gift);
  LOG(LL_INFO, ("Gift inserted! Total gift count: %.2f", total_gift));
  save_counts_to_json();
  (void) pin;
  (void) arg;
}

// Function to check the status of the machine and update machine_on
static void check_machine_status(void *arg) {
  int status_pin = mgos_sys_config_get_status_pin();
  bool current_status = mgos_gpio_read(status_pin);
  bool machine_on = mgos_sys_config_get_machine_on();
  if (current_status != machine_on) {
    mgos_sys_config_set_machine_on(current_status);
    char message[64];
    snprintf(message, sizeof(message), "{\"machine_on\": %s}", current_status ? "true" : "false");
    mgos_mqtt_pub(status_topic, message, strlen(message), 1, false);
    LOG(LL_INFO, ("Machine status changed: machine_on=%s", current_status ? "true" : "false"));
  }
  (void) arg;
}

// Timer callback to periodically save the counts
static void report_timer_cb(void *arg) {
  save_counts_to_json();
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
  char message[128];
  snprintf(message, sizeof(message), "{\"total_bag\": %.2f, \"total_gift\": %.2f, \"machine_on\": %s, \"time\": \"%s\"}", mgos_sys_config_get_app_total_bag(), mgos_sys_config_get_app_total_gift(), mgos_sys_config_get_machine_on() ? "true" : "false", time_str);
  mgos_mqtt_pub(rpc_topic_pub, message, strlen(message), 1, false);
  LOG(LL_INFO, ("Counts published: %s", message));
  (void) arg;
}

// RPC handler to set the total bag and total gift counts
static void rpc_set_counters_handler(struct mg_rpc_request_info *ri,
                                     const char *args,
                                     const char *src,
                                     void *cb_arg) {
  double total_bag, total_gift;
  bool bag_set = false, gift_set = false;
  if (json_scanf(args, strlen(args), "{\"total_bag\": %lf}", &total_bag) == 1) {
    mgos_sys_config_set_app_total_bag(total_bag);
    bag_set = true;
  }
  if (json_scanf(args, strlen(args), "{\"total_gift\": %lf}", &total_gift) == 1) {
    mgos_sys_config_set_app_total_gift(total_gift);
    gift_set = true;
  }
  if (bag_set || gift_set) {
    save_counts_to_json();
    mg_rpc_send_responsef(ri, "{\"id\": %d, \"result\": true}", ri->id);
    LOG(LL_INFO, ("Set counts via RPC: bag_set=%d, gift_set=%d", bag_set, gift_set));
    char confirmation_msg[128];
    snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Counts updated via RPC\", \"total_bag\": %.2f, \"total_gift\": %.2f}", mgos_sys_config_get_app_total_bag(), mgos_sys_config_get_app_total_gift());
    mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
  } else {
    mg_rpc_send_errorf(ri, 400, "Invalid parameters format");
    LOG(LL_ERROR, ("Invalid parameters format in RPC request"));
  }

  (void) cb_arg;
}

// MQTT message handler to change the configuration
static void mqtt_message_handler(struct mg_connection *nc, const char *topic,
                                 int topic_len, const char *msg, int msg_len,
                                 void *userdata) {
  LOG(LL_INFO, ("Received message on topic %.*s: %.*s", topic_len, topic, msg_len, msg));

  struct json_token method_token, params_token;
  if (json_scanf(msg, msg_len, "{\"method\": %T, \"params\": %T}", &method_token, &params_token) == 2) {
    if (strncmp(method_token.ptr, "Counters.Set", method_token.len) == 0) {
      double total_bag, total_gift;
      bool bag_set = false, gift_set = false;
      if (json_scanf(params_token.ptr, params_token.len, "{\"total_bag\": %lf}", &total_bag) == 1) {
        mgos_sys_config_set_app_total_bag(total_bag);
        bag_set = true;
      }
      if (json_scanf(params_token.ptr, params_token.len, "{\"total_gift\": %lf}", &total_gift) == 1) {
        mgos_sys_config_set_app_total_gift(total_gift);
        gift_set = true;
      }
      if (bag_set || gift_set) {
        save_counts_to_json();
        LOG(LL_INFO, ("Set counts via MQTT: bag_set=%d, gift_set=%d", bag_set, gift_set));
        char confirmation_msg[128];
        snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Counts updated via MQTT\", \"total_bag\": %.2f, \"total_gift\": %.2f}", mgos_sys_config_get_app_total_bag(), mgos_sys_config_get_app_total_gift());
        mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
      } else {
        LOG(LL_ERROR, ("Invalid JSON format for total_bag and total_gift parameters"));
      }
    } else if (strncmp(method_token.ptr, "App.SetPinMachine", method_token.len) == 0) {
      int pin_state;
      if (json_scanf(params_token.ptr, params_token.len, "{\"pin_machine\": %d}", &pin_state) == 1) {
        int pin = mgos_sys_config_get_pin_machine();
        mgos_gpio_write(pin, pin_state);
        LOG(LL_INFO, ("Set pin %d to state %d", pin, pin_state));
        char confirmation_msg[128];
        snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Pin state updated\", \"pin\": %d, \"state\": %d}", pin, pin_state);
        mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
      } else {
        LOG(LL_ERROR, ("Invalid JSON format for pin_machine"));
      }
    } else {
      LOG(LL_ERROR, ("Invalid method"));
    }
  } else {
    LOG(LL_ERROR, ("Invalid JSON format"));
  }

  (void) nc;
  (void) userdata;
}

enum mgos_app_init_result mgos_app_init(void) {
  load_counts_from_json(); // Load the initial counts from JSON

  int coin_pin = mgos_sys_config_get_coin_pin();
  mgos_gpio_set_mode(coin_pin, MGOS_GPIO_MODE_INPUT);
  mgos_gpio_set_pull(coin_pin, MGOS_GPIO_PULL_UP);
  mgos_gpio_set_int_handler(coin_pin, MGOS_GPIO_INT_EDGE_NEG, coin_isr, NULL);
  mgos_gpio_enable_int(coin_pin);

  int gift_pin = mgos_sys_config_get_gift_pin();
  mgos_gpio_set_mode(gift_pin, MGOS_GPIO_MODE_INPUT);
  mgos_gpio_set_pull(gift_pin, MGOS_GPIO_PULL_UP);
  mgos_gpio_set_int_handler(gift_pin, MGOS_GPIO_INT_EDGE_NEG, gift_isr, NULL);
  mgos_gpio_enable_int(gift_pin);

  int report_delay = mgos_sys_config_get_coin_report_delay();
  report_timer_id = mgos_set_timer(report_delay, MGOS_TIMER_REPEAT, report_timer_cb, NULL);

  int pin_machine = mgos_sys_config_get_pin_machine();
  mgos_gpio_set_mode(pin_machine, MGOS_GPIO_MODE_OUTPUT);

  int status_pin = mgos_sys_config_get_status_pin();
  mgos_gpio_set_mode(status_pin, MGOS_GPIO_MODE_INPUT);

  const char *machine_id = mgos_sys_config_get_app_machine_id();
  snprintf(rpc_topic_pub, sizeof(rpc_topic_pub), "machine/%s/in/report", machine_id);
  snprintf(rpc_topic_sub, sizeof(rpc_topic_sub), "machine/%s/out/set", machine_id);
  snprintf(confirmation_topic, sizeof(confirmation_topic), "machine/%s/confirmation", machine_id);
  snprintf(status_topic, sizeof(status_topic), "machine/%s/status", machine_id);

  // Register the RPC handlers
  mgos_rpc_add_handler("Counters.Set", rpc_set_counters_handler, NULL);
  LOG(LL_INFO, ("RPC handlers registered successfully"));

  // Subscribe to the MQTT topic
  mgos_mqtt_sub(rpc_topic_sub, mqtt_message_handler, NULL);

  // Set a timer to check the machine status periodically
  status_check_timer_id = mgos_set_timer(1000 /* 1 second */, MGOS_TIMER_REPEAT, check_machine_status, NULL);

  // WiFi configuration
  const char *ssid = mgos_sys_config_get_wifi_sta_ssid();
  const char *pass = mgos_sys_config_get_wifi_sta_pass();
  if (ssid && pass) {
    struct mgos_config_wifi_sta sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    sta_cfg.enable = true;
    sta_cfg.ssid = strdup(ssid);
    sta_cfg.pass = strdup(pass);
    mgos_wifi_setup_sta(&sta_cfg);
    free((void *) sta_cfg.ssid);
    free((void *) sta_cfg.pass);
  } else {
    LOG(LL_ERROR, ("WiFi SSID or password not set"));
    return MGOS_APP_INIT_ERROR;
  }

  return MGOS_APP_INIT_SUCCESS;
}
