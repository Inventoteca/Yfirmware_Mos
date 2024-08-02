#include "mgos.h"
#include "mgos_gpio.h"
#include "mgos_sys_config.h"
#include "frozen.h"
#include "mgos_vfs.h"
#include "mgos_mqtt.h"

static int coin_count = 0;
static int gift_count = 0;
static int prev_coin_count = -1;
static int prev_gift_count = -1;
static mgos_timer_id report_timer_id;
static const char *filename = "counts.json";

static void save_counts_to_json(int coin_count, int gift_count) {
  char *json_str = json_asprintf("{total_bag: %d, total_gift: %d}", coin_count, gift_count);
  if (json_str != NULL) {
    FILE *f = fopen(filename, "w");
    if (f != NULL) {
      fwrite(json_str, 1, strlen(json_str), f);
      fclose(f);
      LOG(LL_INFO, ("Counts saved to JSON: coins=%d, gifts=%d", coin_count, gift_count));
    } else {
      LOG(LL_ERROR, ("Failed to open file for writing"));
    }
    free(json_str);
  } else {
    LOG(LL_ERROR, ("Failed to allocate memory for JSON string"));
  }
}

static void load_counts_from_json() {
  FILE *f = fopen(filename, "r");
  if (f != NULL) {
    char buffer[100];
    int len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[len] = '\0';
    fclose(f);
    json_scanf(buffer, len, "{total_bag: %d, total_gift: %d}", &coin_count, &gift_count);
    LOG(LL_INFO, ("Counts loaded from JSON: coins=%d, gifts=%d", coin_count, gift_count));
  } else {
    LOG(LL_ERROR, ("Failed to open file for reading"));
  }
}

static void coin_isr(int pin_coin, void *arg) {
  coin_count++;
  save_counts_to_json(coin_count, gift_count);
  (void) pin_coin;
  (void) arg;
}

static void gift_isr(int pin_gift, void *arg) {
  gift_count++;
  save_counts_to_json(coin_count, gift_count);
  (void) pin_gift;
  (void) arg;
}

static void report_timer_cb(void *arg) {
  if (coin_count != prev_coin_count || gift_count != prev_gift_count) {
    char *machine_id = (char *) mgos_sys_config_get_app_machine_id();
    char topic[100];
    snprintf(topic, sizeof(topic), "machines/%s/out/report", machine_id);

    char *json_str = json_asprintf("{\"coins\": %d, \"gifts\": %d}", coin_count, gift_count);
    if (json_str != NULL) {
      bool res = mgos_mqtt_pub(topic, json_str, strlen(json_str), 1, false);
      LOG(LL_INFO, ("Reporting counts: coins=%d, gifts=%d, MQTT publish: %s", coin_count, gift_count, res ? "success" : "failed"));
      free(json_str);
    }

    prev_coin_count = coin_count;
    prev_gift_count = gift_count;
  } else {
    LOG(LL_INFO, ("Counts unchanged. No MQTT publish."));
  }
  (void) arg;
}

enum mgos_app_init_result mgos_app_init(void) {
  load_counts_from_json(); // Cargar las posiciones desde el archivo al iniciar

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

  return MGOS_APP_INIT_SUCCESS;
}
