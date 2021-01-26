/* Snapcast client


   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "lwip/apps/sntp.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "snapclient_stream.h"
#include "opus_decoder.h"
#include "filter_resample.h"

#include "nvs_flash.h"

#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"

static const char *TAG = "SNAPCAST";
/*
   To embed it in the app binary, the mp3 file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/

static void wait_for_sntp(void)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;

    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "europe.pool.ntp.org");
    sntp_init();

    const int retry_count = 20;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}


void app_main(void)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, opus_decoder, snapclient_stream;

	// setup logging
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

	// flash init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
	// for IDF >= 4.1.0
	// ESP_ERROR_CHECK(esp_netif_init()); does not work, fall back to bw compat libs
	tcpip_adapter_init();

	// now setip the audio pipeline
    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
	audio_hal_set_volume(board_handle->audio_hal, 30);

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline, add all elements to pipeline, and subscribe pipeline event");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.0] Create snapclient source stream");
    snapclient_stream_cfg_t snapclient_cfg = SNAPCLIENT_STREAM_CFG_DEFAULT();
	snapclient_cfg.port = CONFIG_SNAPSERVER_PORT;
	snapclient_cfg.host = CONFIG_SNAPSERVER_HOST;
	// TODO buff len & client name
    snapclient_stream = snapclient_stream_init(&snapclient_cfg);

    ESP_LOGI(TAG, "[2.1] Create opus decoder");
    opus_decoder_cfg_t opus_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    opus_decoder = decoder_opus_init(&opus_cfg);

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.i2s_config.sample_rate = 48000;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, snapclient_stream, "snapclient");
    audio_pipeline_register(pipeline, opus_decoder, "opus");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.4] Link it together");

    const char *link_tag[3] = {"snapclient", "opus", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[ 3 ] Start and wait for Wi-Fi network");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    periph_wifi_cfg_t wifi_cfg = {
        .ssid = CONFIG_ESP_WIFI_SSID,
        .password = CONFIG_ESP_WIFI_PASSWORD,
    };

    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    ESP_LOGI(TAG, "[3.1] Start the Wi-Fi network");
    esp_periph_start(set, wifi_handle);
    ESP_LOGI(TAG, "[3.2] wait for connection");

	while (1) {
		esp_err_t result = periph_wifi_wait_for_connected(wifi_handle, 2000 / portTICK_PERIOD_MS);
		if (result == ESP_OK)
			break;
		ESP_LOGW(TAG, "[3.2] still waiting for connection");
	}

	// actually we don't really want to get not too big timestamps
	// wait_for_sntp();

	struct timeval now;
	if (gettimeofday(&now, NULL)) {
		ESP_LOGW(TAG, "Failed to gettimeofday");
	} else {
		ESP_LOGI(TAG, "Current timestamp is %ld.%ld", now.tv_sec, now.tv_usec);
	}

	// XXX set up SNTP

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    while (1) {
		/*
        AEL_MSG_CMD_NONE                = 0,
        AEL_MSG_CMD_ERROR               = 1,
        AEL_MSG_CMD_FINISH              = 2,
        AEL_MSG_CMD_STOP                = 3,
        AEL_MSG_CMD_PAUSE               = 4,
        AEL_MSG_CMD_RESUME              = 5,
        AEL_MSG_CMD_DESTROY             = 6,
        // AEL_MSG_CMD_CHANGE_STATE        = 7,
        AEL_MSG_CMD_REPORT_STATUS       = 8,
        AEL_MSG_CMD_REPORT_MUSIC_INFO   = 9,
        AEL_MSG_CMD_REPORT_CODEC_FMT    = 10,
        AEL_MSG_CMD_REPORT_POSITION     = 11,
		*/
		char source[20];

        audio_event_iface_msg_t msg;
		ESP_LOGI(TAG, "[ X ] Waiting for a new message");
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
		if (msg.source == (void *) opus_decoder)
			sprintf(source, "%s", "opus");
		else if (msg.source == (void *) snapclient_stream)
			sprintf(source, "%s", "snapclient");
		else if (msg.source == (void *) i2s_stream_writer)
			sprintf(source, "%s", "i2s");
		else
			sprintf(source, "%s", "unknown");


		ESP_LOGI(TAG, "[ X ] Event message %d:%d from %s", msg.source_type, msg.cmd, source);

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
			&& msg.source == (void *) snapclient_stream
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
			ESP_LOGI(TAG, "[ X ] report music info ");
            audio_element_info_t music_info = {0};
            audio_element_getinfo(snapclient_stream, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from snapclient decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(opus_decoder, &music_info);
            audio_element_setinfo(i2s_stream_writer, &music_info);

            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates , music_info.bits, music_info.channels);
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) opus_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
			ESP_LOGI(TAG, "[ X ] opus message ");
            audio_element_info_t music_info = {0};
            audio_element_getinfo(opus_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from opus decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);

            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates , music_info.bits, music_info.channels);
            continue;
        }
        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
			ESP_LOGI(TAG, "[ X ] i2s wants to stop!");

            //break;
        }
    }

    ESP_LOGI(TAG, "[ 5 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

	audio_pipeline_unregister(pipeline, snapclient_stream);
    audio_pipeline_unregister(pipeline, opus_decoder);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Make sure audio_pipeline_remove_listener is called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
	/* D: still neeeded?
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, opus_decoder);
	*/
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(opus_decoder);
    audio_element_deinit(snapclient_stream);
}
