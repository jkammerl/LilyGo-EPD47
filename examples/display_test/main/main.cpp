/* Simple firmware for a ESP32 displaying a static image on an EPaper Screen.
 *
 * Write an image into a header file using a 3...2...1...0 format per pixel,
 * for 4 bits color (16 colors - well, greys.) MSB first.  At 80 MHz, screen
 * clears execute in 1.075 seconds and images are drawn in 1.531 seconds.
 */

// Now include the third-party library header
#include <WiFiUdp.h>  // before lwip RE: https://github.com/espressif/arduino-esp32/issues/4405
#include <lwip/sockets.h>
// others
#include <epdiy.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "firasans_12.h"
#include "firasans_20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "img_beach.h"
#include "img_board.h"
#include "img_zebra.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

EpdDrawMode DISPLAY_MODE = MODE_GL16;

#ifdef ARDUINO_ARCH_ESP32
// Arduino
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#else
// ESP-IDF
void idf_setup();
void idf_loop();

void delay(uint32_t millis) { vTaskDelay(millis / portTICK_PERIOD_MS); }

extern "C" void app_main() {
  idf_setup();

  while (1) {
    idf_loop();
  };
}
#endif

#define DEEP_SLEEP_DURATION_SECONDS 21600

#define WAVEFORM EPD_BUILTIN_WAVEFORM

// choose the default demo board depending on the architecture
#ifdef CONFIG_IDF_TARGET_ESP32S3
#define DEMO_BOARD epd_board_v7
#endif

EpdiyHighlevelState hl;

#include "esp_http_client.h"

// --- Configuration ---
// Replace with your network credentials
#define WIFI_SSID "SSID"
#define WIFI_PASS "PASS"
#define MAX_RETRIES 10  // Maximum connection retries

// --- Globals ---
static const char* TAG = "HTTP_CLIENT_EXAMPLE";
static int s_retry_num = 0;

// --- FIXED ---
// Make both buffer and its length counter file-static so they can be reset
// together.
static char* downloaded_text_buffer = NULL;
static int output_len = 0;

// Enum for application states to provide on-screen feedback
typedef enum {
  STATE_INIT,
  STATE_CONNECTING,
  STATE_CONNECTED,
  STATE_DOWNLOADING,
  STATE_DOWNLOAD_ERROR,
  STATE_DISPLAY_CONTENT
} app_state_t;

static volatile app_state_t app_state = STATE_INIT;

// Event group to signal when we are connected
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// --- HTTP Client Event Handler ---
/**
 * @brief Event handler for HTTP client events.
 *
 * This function is called by the HTTP client library to report its status.
 * It now correctly handles both chunked and non-chunked responses by
 * dynamically allocating and resizing the buffer.
 *
 * @param evt Pointer to the event data.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t _http_event_handler(esp_http_client_event_t* evt) {
  switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
      app_state = STATE_DOWNLOAD_ERROR;
      break;

    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      // --- REMOVED ---
      // The output_len is now reliably reset in idf_loop, so we don't do it
      // here. This avoids issues with HTTP keep-alive where this event may not
      // fire on subsequent requests.
      break;

    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;

    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
               evt->header_value);
      break;

    case HTTP_EVENT_ON_DATA: {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);

      // This logic now handles both chunked and non-chunked responses.
      // Use a temporary pointer for realloc for safety. If realloc fails,
      // it returns NULL, and you would lose the original pointer.
      char* new_buffer = (char*)realloc(downloaded_text_buffer,
                                        output_len + evt->data_len + 1);
      if (new_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to reallocate memory");
        if (downloaded_text_buffer) {
          free(downloaded_text_buffer);  // Free the original buffer
        }
        downloaded_text_buffer = NULL;
        output_len = 0;  // Also reset length
        app_state = STATE_DOWNLOAD_ERROR;
        return ESP_FAIL;  // Abort the process
      }

      // Point to the new, larger buffer
      downloaded_text_buffer = new_buffer;

      // Copy the incoming data chunk to the end of the buffer
      memcpy(downloaded_text_buffer + output_len, evt->data, evt->data_len);
      output_len += evt->data_len;
    } break;

    case HTTP_EVENT_ON_FINISH: {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      if (downloaded_text_buffer != NULL) {
        // Null-terminate the completed buffer to make it a valid C-string
        downloaded_text_buffer[output_len] = '\0';
        ESP_LOGI(TAG, "HTTP Response Payload (Length: %d):", output_len);
        printf("%.*s\n", output_len, downloaded_text_buffer);
        app_state = STATE_DISPLAY_CONTENT;
      } else {
        // Handle cases where the response was empty (e.g., HTTP 204 No Content)
        ESP_LOGI(TAG, "HTTP response payload was empty.");
        // Ensure buffer is not NULL to avoid issues in the main loop
        downloaded_text_buffer = (char*)malloc(1);
        if (downloaded_text_buffer) downloaded_text_buffer[0] = '\0';
        output_len = 0;
        app_state = STATE_DISPLAY_CONTENT;
      }
    } break;

    case HTTP_EVENT_DISCONNECTED: {
      ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
      // The buffer is now freed reliably by the main idf_loop(),
      // so we don't need to free it here. This simplifies ownership
      // and prevents potential double-free errors.
    } break;
  }
  return ESP_OK;
}

// --- HTTP GET Request Task ---
/**
 * @brief Performs the HTTP GET request.
 *
 * This function is spawned as a task by the main loop.
 * It configures and executes the HTTP GET request.
 *
 * @param pvParameters Unused.
 */
static void http_get_request_task(void* pvParameters) {
  ESP_LOGI(TAG, "Starting HTTP GET request task...");

  esp_http_client_config_t config = {
      .url = "add here",
      .event_handler = _http_event_handler,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  // Perform the GET request
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %lld",
             (int)esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    app_state = STATE_DOWNLOAD_ERROR;
  }

  // Clean up
  esp_http_client_cleanup(client);
  vTaskDelete(NULL);  // Delete this task once done
}

// --- Wi-Fi Event Handler ---
/**
 * @brief Event handler for Wi-Fi and IP events.
 *
 * This function is called by the ESP event loop to handle system-level
 * network events.
 *
 * @param arg Unused.
 * @param event_base The base ID of the event.
 * @param event_id The ID of the event.
 * @param event_data Data associated with the event.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    app_state = STATE_CONNECTING;
    esp_wifi_connect();
    ESP_LOGI(TAG, "Wi-Fi station started, connecting to AP...");
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < MAX_RETRIES) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "Retry connecting to the AP (%d/%d)", s_retry_num,
               MAX_RETRIES);
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      ESP_LOGE(TAG, "Failed to connect to the AP after max retries.");
      app_state =
          STATE_DOWNLOAD_ERROR;  // Use the same state for general errors
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    app_state = STATE_CONNECTED;
    // NOTE: The HTTP request task is no longer started here.
    // It is now managed by the main idf_loop.
  }
}

// --- Wi-Fi Initialization ---
/**
 * @brief Initializes and connects to Wi-Fi.
 */
void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  // Initialize TCP/IP stack
  ESP_ERROR_CHECK(esp_netif_init());

  // Create the default event loop
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Create default Wi-Fi station
  esp_netif_create_default_wifi_sta();

  // Initialize Wi-Fi with default configuration
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Register event handlers
  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  // Configure Wi-Fi using a more robust initialization method
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
  strcpy((char*)wifi_config.sta.password, WIFI_PASS);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  // Wait until either the connection is established (WIFI_CONNECTED_BIT) or
  // connection failed for the maximum number of retries (WIFI_FAIL_BIT).
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
}

void idf_setup() {
  Wire.begin(39, 40);

  epd_init(&DEMO_BOARD, &ED047TC1, EPD_LUT_64K);
  // Set VCOM for boards that allow to set this in software (in mV).
  // This will print an error if unsupported. In this case,
  // set VCOM using the hardware potentiometer and delete this line.
  epd_set_vcom(1560);

  hl = epd_hl_init(WAVEFORM);

  epd_set_rotation(EPD_ROT_LANDSCAPE);

  printf("Dimensions after rotation, width: %d height: %d\n\n",
         epd_rotated_display_width(), epd_rotated_display_height());

  // Initial screen update to show we're initializing
  uint8_t* fb = epd_hl_get_framebuffer(&hl);
  epd_poweron();
  epd_clear();
  EpdFontProperties font_props = epd_font_properties_default();
  font_props.flags = EPD_DRAW_ALIGN_CENTER;
  int cursor_x = epd_rotated_display_width() / 2;
  int cursor_y = epd_rotated_display_height() / 2;
  epd_write_string(&FiraSans_20, "Initializing...", &cursor_x, &cursor_y, fb,
                   &font_props);
  epd_hl_update_screen(&hl, DISPLAY_MODE, epd_ambient_temperature());
  epd_poweroff();

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Start the Wi-Fi connection process
  wifi_init_sta();

  heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
  heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
}

static inline void checkError(enum EpdDrawError err) {
  if (err != EPD_DRAW_SUCCESS) {
    ESP_LOGE("demo", "draw error: %X", err);
  }
}
void PrintMessage(const char* text_to_display) {
  // Get framebuffer and clear it to white (0xFF pattern for 4bpp)
  uint8_t* fb = epd_hl_get_framebuffer(&hl);
  memset(fb, 0xFF,
         epd_rotated_display_width() * epd_rotated_display_height() / 2);

  int temperature = epd_ambient_temperature();
  int cursor_x = 20;
  int cursor_y = 45;  // Start with some top margin

  const EpdFont* font = &FiraSans_20;
  EpdFontProperties font_props = epd_font_properties_default();
  font_props.flags = EPD_DRAW_ALIGN_LEFT;

  if (text_to_display != NULL) {
    // --- REVISED TEXT WRAPPING LOGIC ---
    char current_line_buffer[256] = {0};
    const char* p = text_to_display;

    while (*p != '\0') {
      // Find the end of the next word. Delimiters are space and newline.
      const char* word_end = strpbrk(p, " \n");
      size_t word_len;
      bool is_newline = false;
      const char* next_p;

      if (word_end != NULL) {
        word_len = word_end - p;
        if (*word_end == '\n') {
          is_newline = true;
        }
        // Advance pointer past the delimiter for the next iteration.
        next_p = word_end + 1;
      } else {
        // This is the last word in the text.
        word_len = strlen(p);
        next_p = p + word_len;  // Point to the null terminator to end the loop.
      }

      // Process the word if it's not empty (handles multiple spaces/newlines).
      if (word_len > 0) {
        // Create a temporary line with the new word appended.
        char temp_line[256];
        strcpy(temp_line, current_line_buffer);
        if (current_line_buffer[0] != '\0') {
          strcat(temp_line, " ");  // Add space separator.
        }
        strncat(temp_line, p, word_len);

        // Get the dimensions of the temporary line.
        int x1, y1, w, h;
        epd_get_text_bounds(font, temp_line, &cursor_x, &cursor_y, &x1, &y1, &w,
                            &h, &font_props);

        // Check if the new word causes the line to overflow.
        if (w > epd_rotated_display_width() - 40 &&
            current_line_buffer[0] != '\0') {
          // Line is too long, so write the previous line without the new word.
          epd_write_string(font, current_line_buffer, &cursor_x, &cursor_y, fb,
                           &font_props);

          // Start a new line on the display.
          cursor_x = 20;
          cursor_y += 0;  // Move down by font height plus some padding.

          // The new line starts with the current word.
          strncpy(current_line_buffer, p, word_len);
          current_line_buffer[word_len] = '\0';
        } else {
          // The new word fits, so append it to the current line buffer.
          strcpy(current_line_buffer, temp_line);
        }
      }

      // If the delimiter was a newline, force a line break on the display.
      if (is_newline) {
        // Write whatever is in the buffer to the screen.
        epd_write_string(font, current_line_buffer, &cursor_x, &cursor_y, fb,
                         &font_props);

        // Reset for the next line.
        cursor_x = 20;
        int x1, y1, w, h;
        // Get line height to correctly advance the cursor.
        epd_get_text_bounds(font, "A", &cursor_x, &cursor_y, &x1, &y1, &w, &h,
                            &font_props);
        cursor_y += 10;
        memset(current_line_buffer, 0, sizeof(current_line_buffer));
      }

      p = next_p;  // Move to the start of the next word.
    }

    // After the loop, write any remaining text in the buffer.
    if (current_line_buffer[0] != '\0') {
      epd_write_string(font, current_line_buffer, &cursor_x, &cursor_y, fb,
                       &font_props);
    }
  }

  // Update the physical display.
  epd_poweron();
  checkError(epd_hl_update_screen(&hl, DISPLAY_MODE, temperature));
  epd_poweroff();
}

/**
 * @brief The main application loop.
 * This function now handles the entire download-display-delay cycle.
 */
void idf_loop() {
  // --- FIXED ---
  // Free buffer AND reset the length counter from the previous loop.
  // This ensures a clean state for every download cycle.
  if (downloaded_text_buffer != NULL) {
    free(downloaded_text_buffer);
    downloaded_text_buffer = NULL;
  }
  output_len = 0;

  // --- Start Download ---
  ESP_LOGI(TAG, "Starting new download cycle.");
  app_state = STATE_DOWNLOADING;

  // Update screen to show download status
  epd_poweron();
  epd_clear();
  // We write directly to the framebuffer here for a status message
  uint8_t* fb = epd_hl_get_framebuffer(&hl);
  memset(fb, 0xFF,
         epd_rotated_display_width() * epd_rotated_display_height() / 2);
  EpdFontProperties font_props = epd_font_properties_default();
  font_props.flags = EPD_DRAW_ALIGN_CENTER;
  int cursor_x = epd_rotated_display_width() / 2;
  int cursor_y = epd_rotated_display_height() / 2;
  epd_write_string(&FiraSans_20, "Downloading...", &cursor_x, &cursor_y, fb,
                   &font_props);
  epd_hl_update_screen(&hl, DISPLAY_MODE, epd_ambient_temperature());
  epd_poweroff();

  // Spawn the task that performs the HTTP GET request
  xTaskCreate(&http_get_request_task, "http_get_request_task", 8192, NULL, 5,
              NULL);

  // --- Wait for Download to Complete ---
  // The app_state is updated by the HTTP event handler
  while (app_state != STATE_DISPLAY_CONTENT &&
         app_state != STATE_DOWNLOAD_ERROR) {
    delay(200);  // Poll state every 200ms
  }

  // --- Display Result ---
  if (app_state == STATE_DISPLAY_CONTENT) {
    ESP_LOGI(TAG, "Download complete. Displaying text.");
    PrintMessage(downloaded_text_buffer);
  } else {  // app_state == STATE_DOWNLOAD_ERROR
    ESP_LOGE(TAG, "Download failed.");
    PrintMessage("Download failed.\nRetrying soon...");
  }
  // Note: PrintMessage handles its own poweron/off cycle for the update

  ESP_LOGI(TAG, "Cycle finished. Entering deep sleep for %d seconds.",
           DEEP_SLEEP_DURATION_SECONDS);
  // Enter deep sleep to save power. The device will reset and start from
  // app_main() after the timer expires.
  esp_deep_sleep(DEEP_SLEEP_DURATION_SECONDS * 1000000ULL);
}
