#include "mgos.h"
#include "mgos_gpio.h"
#include "mgos_sys_config.h"
#include "mgos_mqtt.h"
#include "mgos_wifi.h"
#include "mgos_i2c.h"
#include "mgos_ds3231.h"
#include "frozen.h"
#include "mgos_rpc.h"
#include "mgos_dash.h"

// Global variables
static mgos_timer_id report_timer_id;
static mgos_timer_id status_check_timer_id;
static mgos_timer_id auto_control_timer_id;
static char rpc_topic_pub[100];
static char rpc_topic_sub[100];
static char confirmation_topic[100];
static char status_topic[100];
int pin_machine;
char time_str[9];
char date_str[12];

// Declaración del manejador del DS3231
struct mgos_ds3231 *rtc = NULL;
time_t now;
struct tm *t;

// Function to save total bag count, total gift count, enable_auto, on_hour, and off_hour to JSON
static void save_counts_to_json() {
  const char *filename = mgos_sys_config_get_coin_count_file();
  double total_bag = mgos_sys_config_get_app_total_bag();
  double total_gift = mgos_sys_config_get_app_total_gift();
  bool enable_auto = mgos_sys_config_get_app_enable_auto();
  int on_hour = mgos_sys_config_get_app_on_hour();
  int off_hour = mgos_sys_config_get_app_off_hour();
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
  char *json_str = json_asprintf("{total_bag: %.2f, total_gift: %.2f, enable_auto: %B, on_hour: %d, off_hour: %d, time: \"%s\"}", total_bag, total_gift, enable_auto, on_hour, off_hour, time_str);
  if (json_str != NULL) {
    FILE *f = fopen(filename, "w");
    if (f != NULL) {
      fwrite(json_str, 1, strlen(json_str), f);
      fclose(f);
      LOG(LL_INFO, ("Counts saved to JSON: bag=%.2f, gift=%.2f, enable_auto=%d, on_hour=%d, off_hour=%d", total_bag, total_gift, enable_auto, on_hour, off_hour));
    } else {
      LOG(LL_ERROR, ("Failed to open file for writing"));
    }
    free(json_str);
  } else {
    LOG(LL_ERROR, ("Failed to allocate memory for JSON string"));
  }
}

// Function to load total bag count, total gift count, enable_auto, on_hour, and off_hour from JSON
static bool load_counts_from_json(double *total_bag, double *total_gift, bool *enable_auto, int *on_hour, int *off_hour) {
  const char *filename = mgos_sys_config_get_coin_count_file();
  FILE *f = fopen(filename, "r");
  if (f != NULL) {
    char buffer[200];
    int len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[len] = '\0';
    fclose(f);
    if (json_scanf(buffer, len, "{total_bag: %lf, total_gift: %lf, enable_auto: %B, on_hour: %d, off_hour: %d}", total_bag, total_gift, enable_auto, on_hour, off_hour) == 5) {
      return true;
    } else {
      LOG(LL_ERROR, ("Failed to parse JSON"));
    }
  } else {
    LOG(LL_ERROR, ("Failed to open file for reading"));
  }
  return false;
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

// Timer callback to control the machine based on the schedule
static void auto_control_cb(void *arg) {
  double total_bag, total_gift;
  bool enable_auto;
  int on_hour, off_hour;
  load_counts_from_json(); // Reload the counts from JSON to get the latest on_hour and off_hour
  bool enable_auto = mgos_sys_config_get_app_enable_auto();
  int on_hour = mgos_sys_config_get_app_on_hour();
  int off_hour = mgos_sys_config_get_app_off_hour();

  if (!load_counts_from_json(&total_bag, &total_gift, &enable_auto, &on_hour, &off_hour)) {
    LOG(LL_ERROR, ("Failed to load counts from JSON"));
    return;
  }

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int current_hour = t->tm_hour;

  if (enable_auto) {
    if (current_hour >= on_hour && current_hour < off_hour) {
      mgos_gpio_write(pin_machine, 0 );  // Turn on the machine
      LOG(LL_INFO, ("Machine turned on automatically at %d:00", current_hour));
    } else {
      mgos_gpio_write(pin_machine, 1);  // Turn off the machine
      LOG(LL_INFO, ("Machine turned off automatically at %d:00", current_hour));
    if (current_hour == on_hour) {
      mgos_gpio_write(pin_machine, 0);  // Turn on the machine
      LOG(LL_INFO, ("Machine turned on automatically at %d:00", on_hour));
    } else if (current_hour == off_hour) {
      mgos_gpio_write(pin_machine, 1);  // Turn off the machine
      LOG(LL_INFO, ("Machine turned off automatically at %d:00", off_hour));
    } else if (current_hour > on_hour && current_hour < off_hour && !mgos_gpio_read(pin_machine)) {
      mgos_gpio_write(pin_machine, 0);  // Ensure the machine stays on between on_hour and off_hour
      LOG(LL_INFO, ("Machine re-enabled during operational hours"));
    } else if ((current_hour < on_hour || current_hour > off_hour) && mgos_gpio_read(pin_machine)) {
      mgos_gpio_write(pin_machine, 1);  // Ensure the machine stays off outside operational hours
      LOG(LL_INFO, ("Machine disabled outside operational hours"));
    }
  } else {
    // Allow manual control if enable_auto is false
    bool machine_on = mgos_sys_config_get_machine_on();
    mgos_gpio_write(pin_machine, machine_on ? 1 : 0);
    LOG(LL_INFO, ("Machine manually controlled: %s", machine_on ? "on" : "off"));
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
  char message[256];
  now = time(NULL);
  t = localtime(&now);

  snprintf(time_str, sizeof(time_str), "%02d:%02d", t->tm_hour, t->tm_min);
  snprintf(message, sizeof(message), "{total_bag: %.2f, total_gift: %.2f, machine_on: %s, enable_auto: %s, on_hour: %d, off_hour: %d, time: \"%s\"}",
           mgos_sys_config_get_app_total_bag(),
           mgos_sys_config_get_app_total_gift(),
           mgos_sys_config_get_machine_on() ? "true" : "false",
           mgos_sys_config_get_app_enable_auto() ? "true" : "false",
           mgos_sys_config_get_app_on_hour(),
           mgos_sys_config_get_app_off_hour(),
           time_str);
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
  if (json_scanf(args, strlen(args), "{total_bag: %lf}", &total_bag) == 1) {
    mgos_sys_config_set_app_total_bag(total_bag);
    bag_set = true;
  }
  if (json_scanf(args, strlen(args), "{total_gift: %lf}", &total_gift) == 1) {
    mgos_sys_config_set_app_total_gift(total_gift);
    gift_set = true;
  }
  if (bag_set || gift_set) {
    save_counts_to_json();
    mg_rpc_send_responsef(ri, "{id: %d, result: true}", ri->id);
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

// RPC handler to set enable_auto
static void rpc_set_enable_auto_handler(struct mg_rpc_request_info *ri,
                                        const char *args,
                                        const char *src,
                                        void *cb_arg) {
  bool enable_auto;
  if (json_scanf(args, strlen(args), "{enable_auto: %B}", &enable_auto) == 1) {
    mgos_sys_config_set_app_enable_auto(enable_auto);
    save_counts_to_json();
    mg_rpc_send_responsef(ri, "{id: %d, result: true}", ri->id);
    LOG(LL_INFO, ("Set enable_auto via RPC: enable_auto=%d", enable_auto));
    char confirmation_msg[128];
    snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Enable_auto updated via RPC\", \"enable_auto\": %s}", enable_auto ? "true" : "false");
    mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
  } else {
    mg_rpc_send_errorf(ri, 400, "Invalid parameters format");
    LOG(LL_ERROR, ("Invalid parameters format in RPC request"));
  }

  (void) cb_arg;
}

// RPC handler to set on_hour
static void rpc_set_on_hour_handler(struct mg_rpc_request_info *ri,
                                    const char *args,
                                    const char *src,
                                    void *cb_arg) {
  int on_hour;
  if (json_scanf(args, strlen(args), "{on_hour: %d}", &on_hour) == 1) {
    mgos_sys_config_set_app_on_hour(on_hour);
    save_counts_to_json();
    mg_rpc_send_responsef(ri, "{id: %d, result: true}", ri->id);
    LOG(LL_INFO, ("Set on_hour via RPC: on_hour=%d", on_hour));
    char confirmation_msg[128];
    snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"On_hour updated via RPC\", \"on_hour\": %d}", on_hour);
    mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
  } else {
    mg_rpc_send_errorf(ri, 400, "Invalid parameters format");
    LOG(LL_ERROR, ("Invalid parameters format in RPC request"));
  }

  (void) cb_arg;
}

// RPC handler to set off_hour
static void rpc_set_off_hour_handler(struct mg_rpc_request_info *ri,
                                     const char *args,
                                     const char *src,
                                     void *cb_arg) {
  int off_hour;
  if (json_scanf(args, strlen(args), "{off_hour: %d}", &off_hour) == 1) {
    mgos_sys_config_set_app_off_hour(off_hour);
    save_counts_to_json();
    mg_rpc_send_responsef(ri, "{id: %d, result: true}", ri->id);
    LOG(LL_INFO, ("Set off_hour via RPC: off_hour=%d", off_hour));
    char confirmation_msg[128];
    snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Off_hour updated via RPC\", \"off_hour\": %d}", off_hour);
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
  if (json_scanf(msg, msg_len, "{method: %T, params: %T}", &method_token, &params_token) == 2) {
    if (strncmp(method_token.ptr, "Counters.Set", method_token.len) == 0) {
      double total_bag, total_gift;
      bool bag_set = false, gift_set = false;
      if (json_scanf(params_token.ptr, params_token.len, "{total_bag: %lf}", &total_bag) == 1) {
        mgos_sys_config_set_app_total_bag(total_bag);
        bag_set = true;
      }
      if (json_scanf(params_token.ptr, params_token.len, "{total_gift: %lf}", &total_gift) == 1) {
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
    } else if (strncmp(method_token.ptr, "App.SetEnableAuto", method_token.len) == 0) {
      bool enable_auto;
      if (json_scanf(params_token.ptr, params_token.len, "{enable_auto: %B}", &enable_auto) == 1) {
        mgos_sys_config_set_app_enable_auto(enable_auto);
        save_counts_to_json();
        LOG(LL_INFO, ("Set enable_auto via MQTT: enable_auto=%d", enable_auto));
        char confirmation_msg[128];
        snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Enable_auto updated via MQTT\", \"enable_auto\": %s}", enable_auto ? "true" : "false");
        mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
      } else {
        LOG(LL_ERROR, ("Invalid JSON format for enable_auto"));
      }
    } else if (strncmp(method_token.ptr, "App.SetOnHour", method_token.len) == 0) {
      int on_hour;
      if (json_scanf(params_token.ptr, params_token.len, "{on_hour: %d}", &on_hour) == 1) {
        mgos_sys_config_set_app_on_hour(on_hour);
        save_counts_to_json();
        LOG(LL_INFO, ("Set on_hour via MQTT: on_hour=%d", on_hour));
        char confirmation_msg[128];
        snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"On_hour updated via MQTT\", \"on_hour\": %d}", on_hour);
        mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
      } else {
        LOG(LL_ERROR, ("Invalid JSON format for on_hour"));
      }
    } else if (strncmp(method_token.ptr, "App.SetOffHour", method_token.len) == 0) {
      int off_hour;
      if (json_scanf(params_token.ptr, params_token.len, "{off_hour: %d}", &off_hour) == 1) {
        mgos_sys_config_set_app_off_hour(off_hour);
        save_counts_to_json();
        LOG(LL_INFO, ("Set off_hour via MQTT: off_hour=%d", off_hour));
        char confirmation_msg[128];
        snprintf(confirmation_msg, sizeof(confirmation_msg), "{\"confirmation\": \"Off_hour updated via MQTT\", \"off_hour\": %d}", off_hour);
        mgos_mqtt_pub(confirmation_topic, confirmation_msg, strlen(confirmation_msg), 1, false);
      } else {
        LOG(LL_ERROR, ("Invalid JSON format for off_hour"));
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

// Inicialización del DS3231
// Inicialización del DS3231
bool ds3231_init(void) {
  rtc = mgos_ds3231_create(104);
  if (rtc == NULL) {
    LOG(LL_ERROR, ("Failed to initialize DS3231"));
    return false;
  } else {
  } else {
    LOG(LL_INFO, ("RTC INIT OK"));
    mgos_ds3231_settimeofday(rtc);
    return true;
  }
}

enum mgos_app_init_result mgos_app_init(void) {
  double total_bag, total_gift;
  bool enable_auto;
  int on_hour, off_hour;

  // Load the initial counts from JSON
  if (!load_counts_from_json(&total_bag, &total_gift, &enable_auto, &on_hour, &off_hour)) {
    LOG(LL_ERROR, ("Failed to load initial counts from JSON"));
    return MGOS_APP_INIT_ERROR;
  }

  if (!ds3231_init()) {
    LOG(LL_ERROR, ("Failed to initialize DS3231"));
    return MGOS_APP_INIT_ERROR;
  }

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

  pin_machine = mgos_sys_config_get_pin_machine();
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
  mgos_rpc_add_handler("App.SetEnableAuto", rpc_set_enable_auto_handler, NULL);
  mgos_rpc_add_handler("App.SetOnHour", rpc_set_on_hour_handler, NULL);
  mgos_rpc_add_handler("App.SetOffHour", rpc_set_off_hour_handler, NULL);
  LOG(LL_INFO, ("RPC handlers registered successfully"));

  // Subscribe to the MQTT topic
  mgos_mqtt_sub(rpc_topic_sub, mqtt_message_handler, NULL);

  // Set a timer to check the machine status periodically
  status_check_timer_id = mgos_set_timer(1000 /* 1 second */, MGOS_TIMER_REPEAT, check_machine_status, NULL);

  // Set a timer to control the machine automatically based on the schedule
  auto_control_timer_id = mgos_set_timer(10000 /* 1 minute */, MGOS_TIMER_REPEAT, auto_control_cb, NULL);
  auto_control_timer_id = mgos_set_timer(20000 /* 1 minute */, MGOS_TIMER_REPEAT, auto_control_cb, NULL);

  return MGOS_APP_INIT_SUCCESS;
}
