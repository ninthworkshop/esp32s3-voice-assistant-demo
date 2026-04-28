/**
 * @file main.c
 * @brief ESP32-S3 Gemini Voice Assistant Demo
 * @author Ninth Workshop (https://github.com/ninthworkshop)
 * @version 1.0.0
 * @date 2026-04
 * * @copyright Copyright (c) 2026 Ninth Workshop. Licensed under the MIT License.
 * * --- Project Description ---
 * This is a high-performance, full-stack voice assistant implementation 
 * designed for the ESP32-S3 platform. It seamlessly integrates offline 
 * edge computing with modern cloud AI services.
 * * Core Features:
 * - Offline Wake-word: Espressif WakeNet (default: "Ni Hao Xiao Zhi")
 * - Smart Logic: Google Gemini 2.5 Flash API for advanced reasoning
 * - Natural Voice: Google Cloud Text-to-Speech (TTS) synthesis
 * - Audio Path: I2S Digital Interface (INMP441 Mic & MAX98357A Amp)
 * - Visual UI: Integrated WS2812B RGB LED status feedback
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "mbedtls/base64.h"
#include "esp_http_client.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "led_strip.h"

#include "secrets.h"

#define EXAMPLE_ESP_MAXIMUM_RETRY  5

/* --- Hardware Pin Mapping (Optimized for ESP32-S3) --- */
#define RGB_GPIO_PIN     38 
#define MIC_I2S_SCK      GPIO_NUM_4
#define MIC_I2S_WS       GPIO_NUM_5
#define MIC_I2S_SDIN     GPIO_NUM_6
#define SPK_I2S_BCLK     GPIO_NUM_16
#define SPK_I2S_LRC      GPIO_NUM_17
#define SPK_I2S_DOUT     GPIO_NUM_18

/* --- Audio Parameters --- */
#define SAMPLE_RATE         16000
#define RECORD_TIME_SEC     8 
#define RECORD_BUFFER_SIZE  (SAMPLE_RATE * RECORD_TIME_SEC) 
#define DEBUG_MODE          false

/* --- Global Handles & State Variables --- */
static const char *TAG = "NinthWorkshop";
static led_strip_handle_t led_strip;
static EventGroupHandle_t s_wifi_event_group;
static i2s_chan_handle_t rx_chan = NULL;
static i2s_chan_handle_t tx_chan = NULL;
static bool tx_enabled = false;
static int16_t *record_ptr = NULL;
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* --- WAV File Header Definition --- */
typedef struct {
    char chunk_id[4];        // "RIFF"
    uint32_t chunk_size;     // Total size - 8 bytes
    char format[4];          // "WAVE"
    char fmt_id[4];          // "fmt "
    uint32_t fmt_size;       // 16 for PCM
    uint16_t audio_format;   // 1 for PCM
    uint16_t num_channels;   // 1 for Mono
    uint32_t sample_rate;    // 16000
    uint32_t byte_rate;      // SampleRate * NumChannels * BitsPerSample/8
    uint16_t block_align;    // NumChannels * BitsPerSample/8
    uint16_t bits_per_sample;// 16
    char data_id[4];         // "data"
    uint32_t data_size;      // Raw PCM data size
} wav_header_t;

/* --- I2S Control Helpers --- */

/**
 * Enable I2S TX channel and flush the DMA buffer with silence 
 * to prevent residual audio "pops" from previous sessions.
 */
static void i2s_tx_enable() {
    if (tx_chan && !tx_enabled) {
        i2s_channel_enable(tx_chan);
        tx_enabled = true;

        size_t bytes_written = 0;
        int16_t silence[128] = {0};
        i2s_channel_write(tx_chan, silence, sizeof(silence), &bytes_written, 10);
    }
}

/**
 * Disable I2S TX channel with a slight delay to ensure the 
 * last frame of audio is fully played out.
 */
static void i2s_tx_disable() {
    if (tx_chan && tx_enabled) {
        vTaskDelay(pdMS_TO_TICKS(50));
        i2s_channel_disable(tx_chan);
        tx_enabled = false;
    }
}

/* --- System Event Handlers --- */

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* --- Hardware Initialization Functions --- */

/**
 * Sync system time via NTP for accurate timestamping in AI prompts.
 */
void init_time() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1); // Taiwan Timezone (UTC+8)
    tzset();
}

/**
 * Initialize the onboard WS2812B RGB LED.
 */
void init_rgb() {
    led_strip_config_t strip_config = { .strip_gpio_num = RGB_GPIO_PIN, .max_leds = 1 };
    led_strip_rmt_config_t rmt_config = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
}

void set_led_color(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip) {
        if (r == 0 && g == 0 && b == 0) led_strip_clear(led_strip);
        else {
            led_strip_set_pixel(led_strip, 0, r, g, b);
            led_strip_refresh(led_strip);
        }
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));
    wifi_config_t wifi_config = { .sta = { .ssid = EXAMPLE_ESP_WIFI_SSID, .password = EXAMPLE_ESP_WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

/**
 * Configure I2S for Microphone input (32-bit shifted to 16-bit PCM).
 */
void init_i2s_mic() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = GPIO_NUM_NC, .bclk = MIC_I2S_SCK, .ws = MIC_I2S_WS, .din = MIC_I2S_SDIN },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}

/**
 * Configure I2S for Speaker output (MAX98357A I2S DAC).
 */
void init_i2s_speaker() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = GPIO_NUM_NC, .bclk = SPK_I2S_BCLK, .ws = SPK_I2S_LRC, .dout = SPK_I2S_DOUT },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
}

/* --- Audio Utilities --- */

/**
 * Generates a simple sine wave beep for system feedback.
 */
void play_beep(uint32_t freq, uint32_t duration_ms) {
    size_t num_samples = (SAMPLE_RATE * duration_ms) / 1000;
    int16_t *beep_buf = heap_caps_malloc(num_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (beep_buf) {
        for (int i = 0; i < num_samples; i++) {
            beep_buf[i] = (int16_t)(10000 * sin(2.0 * M_PI * freq * i / SAMPLE_RATE));
        }
        size_t bytes_written = 0;
        i2s_tx_enable();
        i2s_channel_write(tx_chan, beep_buf, num_samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        i2s_tx_disable();
        free(beep_buf);
    }
}

/**
 * Wraps raw PCM data into a WAV header and encodes to Base64 for API transmission.
 */
char* pack_wav_and_base64(int16_t *pcm_data, size_t pcm_len, size_t *out_b64_len) {
    uint32_t data_payload_size = pcm_len * sizeof(int16_t);
    uint32_t total_wav_size = sizeof(wav_header_t) + data_payload_size;
    uint8_t *wav_buffer = (uint8_t *)heap_caps_malloc(total_wav_size, MALLOC_CAP_SPIRAM);
    if (!wav_buffer) return NULL;

    wav_header_t header = {
        .chunk_id = {'R', 'I', 'F', 'F'},
        .chunk_size = total_wav_size - 8,
        .format = {'W', 'A', 'V', 'E'},
        .fmt_id = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = 1,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = SAMPLE_RATE * 2,
        .block_align = 2,
        .bits_per_sample = 16,
        .data_id = {'d', 'a', 't', 'a'},
        .data_size = data_payload_size
    };

    memcpy(wav_buffer, &header, sizeof(wav_header_t));
    memcpy(wav_buffer + sizeof(wav_header_t), pcm_data, data_payload_size);

    size_t b64_max_len = (total_wav_size * 4 / 3) + 10;
    char *b64_output = (char *)heap_caps_malloc(b64_max_len, MALLOC_CAP_SPIRAM);
    if (b64_output) {
        size_t actual_out_len;
        mbedtls_base64_encode((unsigned char *)b64_output, b64_max_len, &actual_out_len, wav_buffer, total_wav_size);
        *out_b64_len = actual_out_len;
        b64_output[actual_out_len] = '\0';
    }
    free(wav_buffer);
    return b64_output;
}

/* --- Cloud Service Integration --- */

/**
 * Sends text to Google TTS API and plays the returned audio content.
 */
esp_err_t text_to_speech_and_play(const char *text) {
    if (text == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting Text-to-Speech synthesis...");

    char *post_data = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    snprintf(post_data, 2048,
        "{\"input\":{\"text\":\"%s\"},"
        "\"voice\":{\"languageCode\":\"zh-TW\",\"ssmlGender\":\"FEMALE\"},"
        "\"audioConfig\":{\"audioEncoding\":\"LINEAR16\",\"sampleRateHertz\":16000}}", text);

    char url[256];
    snprintf(url, sizeof(url), "https://texttospeech.googleapis.com/v1/text:synthesize?key=%s", GOOGLE_TTS_API_KEY);

    esp_http_client_config_t config = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 15000, .buffer_size = 4096, .skip_cert_common_name_check = true };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK) { free(post_data); esp_http_client_cleanup(client); return err; }

    esp_http_client_write(client, post_data, strlen(post_data));
    esp_http_client_fetch_headers(client);
    
    int status_code = esp_http_client_get_status_code(client);

    if (status_code == 200) {
        ESP_LOGI(TAG, "TTS Response Status: %d", status_code);
        int alloc_len = 1024 * 1024 * 4; // Large buffer for long audio in PSRAM
        char *resp_buf = (char *)heap_caps_malloc(alloc_len, MALLOC_CAP_SPIRAM);
        if (resp_buf) {
            int total_read_len = 0;
            int read_len = 0;

            // Handle Chunked Transfer Encoding by reading until completion
            while (1) {
                read_len = esp_http_client_read(client, resp_buf + total_read_len, 4096);
                if (read_len <= 0) break; 
                total_read_len += read_len;
                
                if (total_read_len >= alloc_len - 4096) {
                    ESP_LOGW(TAG, "Buffer full, stopping read");
                    break;
                }
            }
            resp_buf[total_read_len] = '\0';
            ESP_LOGD(TAG, "Read %d bytes from TTS API", total_read_len);

            if (total_read_len > 0) {
                cJSON *root = cJSON_Parse(resp_buf);
                if (root) {
                    cJSON *audio_item = cJSON_GetObjectItem(root, "audioContent");
                    if (audio_item && audio_item->valuestring) {
                        const char *b64_str = audio_item->valuestring;
                        size_t b64_len = strlen(b64_str);
                        size_t pcm_max_len = b64_len * 3 / 4 + 10;
                        
                        unsigned char *pcm_data = (unsigned char *)heap_caps_malloc(pcm_max_len, MALLOC_CAP_SPIRAM);
                        if (pcm_data) {
                            size_t pcm_actual_len = 0;
                            if (mbedtls_base64_decode(pcm_data, pcm_max_len, &pcm_actual_len, (unsigned char *)b64_str, b64_len) == 0) {
                                ESP_LOGI(TAG, "Playing TTS Audio (%zu bytes)...", pcm_actual_len);
                                size_t bytes_written = 0;
                                i2s_tx_enable();
                                i2s_channel_write(tx_chan, pcm_data, pcm_actual_len, &bytes_written, portMAX_DELAY);
                                i2s_tx_disable();
                            }
                            free(pcm_data);
                        }                        
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "JSON Parse Fail for TTS response.");
                }
            }
            free(resp_buf);
        }
    } else {
        ESP_LOGW(TAG, "TTS API Error Status: %d", status_code);
    }
    free(post_data);
    esp_http_client_cleanup(client);
    return err;
}

/**
 * Sends audio recording to Gemini API and processes the AI response.
 */
esp_err_t send_voice_to_gemini(int16_t *pcm_data, size_t pcm_len) {
    size_t b64_len = 0;
    char *final_b64 = pack_wav_and_base64(pcm_data, pcm_len, &b64_len);
    if (!final_b64) return ESP_ERR_NO_MEM;

    char *post_data = (char *)heap_caps_malloc(b64_len + 2048, MALLOC_CAP_SPIRAM);
    if (!post_data) { free(final_b64); return ESP_ERR_NO_MEM; }

    // Fetch current time for contextual prompts
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char str_ftime[64];
    strftime(str_ftime, sizeof(str_ftime), "%Y-%m-%d %H:%M", &timeinfo);

    char dynamic_prompt[512];
    snprintf(dynamic_prompt, sizeof(dynamic_prompt),
        "你是一位專業語音助手。現在時間是 %s，地點在台灣台北市。請回答問題重點不要超過180個字。", 
        str_ftime);

    sprintf(post_data, 
        "{\"contents\": [{\"parts\": ["
        "{\"text\": \"%s\"},"
        "{\"inline_data\": {\"mime_type\": \"audio/wav\", \"data\": \"%s\"}}"
        "]}]}", dynamic_prompt, final_b64);

    char url[256];
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=%s", GEMINI_API_KEY);

    esp_http_client_config_t config = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 30000, .buffer_size_tx = 4096, .skip_cert_common_name_check = true };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err == ESP_OK) {
        esp_http_client_write(client, post_data, strlen(post_data));
        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            int max_len = 32768; 
            char *response_buffer = (char *)heap_caps_malloc(max_len, MALLOC_CAP_SPIRAM);
            if (response_buffer) {
                int read_len = esp_http_client_read_response(client, response_buffer, max_len - 1);
                if (read_len > 0) {
                    response_buffer[read_len] = '\0';
                    cJSON *root = cJSON_Parse(response_buffer);
                    if (root) {
                        cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
                        if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
                            cJSON *content = cJSON_GetObjectItem(cJSON_GetArrayItem(candidates, 0), "content");
                            cJSON *parts = cJSON_GetObjectItem(content, "parts");
                            if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                                cJSON *text_item = cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "text");
                                if (text_item && text_item->valuestring) {
                                    ESP_LOGI(TAG, "Gemini Response: %s", text_item->valuestring);
                                    text_to_speech_and_play(text_item->valuestring);
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                free(response_buffer);
            }
        } else {
            ESP_LOGW(TAG, "Gemini API Error Status: %d", status_code);
        }
    }
    free(final_b64);
    free(post_data);
    esp_http_client_cleanup(client);
    return err;
}

/* --- Main Application Task --- */

void voice_assistant_task(void *pvParameters) {
    init_rgb();
    init_i2s_mic();
    init_i2s_speaker();

    // NVS storage required for WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    init_time();

    record_ptr = (int16_t *)heap_caps_malloc(RECORD_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    
    // Initialize Wake-word detection (WakeNet)
    srmodel_list_t *models = esp_srmodel_init("model");
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi"); 
    esp_wn_iface_t *wakenet = (esp_wn_iface_t*)esp_wn_handle_from_name(model_name);
    model_iface_data_t *model_data = wakenet->create(model_name, DET_MODE_95);

    int chunk_num = wakenet->get_samp_chunksize(model_data);
    int32_t *i2s_raw_buffer = (int32_t *)malloc(chunk_num * sizeof(int32_t));
    int16_t *wn_buffer = (int16_t *)malloc(chunk_num * sizeof(int16_t));

    ESP_LOGI(TAG, "Voice System Ready. Wake-word: 'nihaoxiaozhi'");
    
    int record_idx = 0;
    int silence_count = 0;
    bool is_recording = false;

    while (1) {
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_chan, i2s_raw_buffer, chunk_num * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK) {
            // Pre-process: 32-bit to 16-bit PCM shift
            for (int i = 0; i < chunk_num; i++) wn_buffer[i] = (int16_t)(i2s_raw_buffer[i] >> 14);

            wakenet_state_t state = wakenet->detect(model_data, wn_buffer);
            if (state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "Wake-word Detected!");
                play_beep(800, 100);
                set_led_color(0, 0, 255); // Blue for active state
                is_recording = true; 
                record_idx = 0; 
                silence_count = 0;
            }

            if (is_recording) {
                if (record_idx + chunk_num < RECORD_BUFFER_SIZE) {
                    memcpy(&record_ptr[record_idx], wn_buffer, chunk_num * sizeof(int16_t));
                    record_idx += chunk_num;
                }

                // Simplified Voice Activity Detection (VAD) via energy measurement
                int32_t energy = 0;
                for(int i = 0; i < chunk_num; i++) energy += abs(wn_buffer[i]);
                if ((energy / chunk_num) < 500) silence_count++; else silence_count = 0;

                // Stop recording if buffer is full or silence persists for ~1 second
                if (record_idx + chunk_num >= RECORD_BUFFER_SIZE || silence_count > 60) { 
                    is_recording = false;
                    set_led_color(0, 0, 0); // Clear LED
                    ESP_LOGI(TAG, "Recording finished. Processing via Gemini...");
                    send_voice_to_gemini(record_ptr, record_idx);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void app_main(void) {
    // Dedicated task for the voice assistant with large stack for JSON parsing
    xTaskCreatePinnedToCore(voice_assistant_task, "voice_assistant_task", 1024 * 48, NULL, 5, NULL, 0);
}
