
#include "esp_log.h"
#include "esp_err.h"
#include "lwip/sockets.h"
#include "esp_transport_tcp.h"
#include "audio_mem.h"
#include "snapclient_stream.h"
#include "snapcast.h"

static const char *TAG = "SNAPCLIENT_STREAM";
#define CONNECT_TIMEOUT_MS        100

typedef struct snapclient_stream {
    esp_transport_handle_t        t;
    audio_stream_type_t           type;
    int                           sock;
    int                           port;
    char                          *host;
    bool                          is_open;
    int                           timeout_ms;
    snapclient_stream_event_handle_cb    hook;
    void                          *ctx;
	// snapclient structures
	/*
	codec_header_message_t codec_header_message;
	wire_chunk_message_t wire_chunk_message;
	server_settings_message_t server_settings_message;
	*/
} snapclient_stream_t;

static int _get_socket_error_code_reason(char *str, int sockfd)
{
    uint32_t optlen = sizeof(int);
    int result;
    int err;

    err = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &result, &optlen);
    if (err == -1) {
        ESP_LOGE(TAG, "%s, getsockopt failed: ret=%d", str, err);
        return -1;
    }
    if (result != 0) {
        ESP_LOGW(TAG, "%s error, error code: %d, reason: %s", str, err, strerror(result));
    }
    return result;
}

static esp_err_t _dispatch_event(audio_element_handle_t el, snapclient_stream_t *snapclient, void *data, int len, snapclient_stream_status_t state)
{
    if (el && snapclient && snapclient->hook) {
        snapclient_stream_event_msg_t msg = { 0 };
        msg.data = data;
        msg.data_len = len;
        msg.sock_fd = snapclient->t;
        msg.source = el;
        return snapclient->hook(&msg, state, snapclient->ctx);
    }
    return ESP_FAIL;
}


static esp_err_t _snapclient_open(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);
	int result;
    struct timeval now;

    snapclient_stream_t *snapclient = (snapclient_stream_t *)audio_element_getdata(self);
    if (snapclient->is_open) {
        ESP_LOGE(TAG, "Already opened");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Host is %s, port is %d\n", snapclient->host, snapclient->port);

    esp_transport_handle_t t = esp_transport_tcp_init();
    AUDIO_NULL_CHECK(TAG, t, return ESP_FAIL);
    snapclient->sock = esp_transport_connect(t, snapclient->host, snapclient->port, CONNECT_TIMEOUT_MS);
    if (snapclient->sock < 0) {
        _get_socket_error_code_reason("TCP create",  snapclient->sock);
        return ESP_FAIL;
    }

    snapclient->is_open = true;
    snapclient->t = t;

	char mac_address[18];
    uint8_t base_mac[6];
    // Get MAC address for WiFi station
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    sprintf(mac_address,
			"%02X:%02X:%02X:%02X:%02X:%02X",
			base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

	codec_header_message_t codec_header_message;
	wire_chunk_message_t wire_chunk_message;
	server_settings_message_t server_settings_message;

	result = gettimeofday(&now, NULL);
	if (result) {
		ESP_LOGI(TAG, "Failed to gettimeofday\r\n");
		return ESP_FAIL;
	}

	bool received_header = false;
	base_message_t base_message = {
		SNAPCAST_MESSAGE_HELLO,      // type
		0x0,                         // id
		0x0,                         // refersTo
		{ now.tv_sec, now.tv_usec }, // sent
		{ 0x0, 0x0 },                // received
		0x0,                         // size
	};

	hello_message_t hello_message = {
		mac_address,
		SNAPCLIENT_STREAM_CLIENT_NAME,  // hostname
		"0.0.2",               // client version
		"libsnapcast",         // client name
		"esp32",               // os name
		"xtensa",              // arch
		1,                     // instance
		mac_address,           // id
		2,                     // protocol version
	};

    char base_message_serialized[BASE_MESSAGE_SIZE];
    char *hello_message_serialized;

	hello_message_serialized = hello_message_serialize(
		&hello_message, (size_t*) &(base_message.size));
	if (!hello_message_serialized) {
		ESP_LOGI(TAG, "Failed to serialize hello message\r\b");
		return ESP_FAIL;
	}

	result = base_message_serialize(
		&base_message,
		base_message_serialized,
		BASE_MESSAGE_SIZE);

	if (result) {
		ESP_LOGI(TAG, "Failed to serialize base message\r\n");
        return ESP_FAIL;
	}

	result = esp_transport_write(snapclient->t,
								 base_message_serialized, BASE_MESSAGE_SIZE,
								 snapclient->timeout_ms);
    if (result < 0) {
        _get_socket_error_code_reason("TCP write", snapclient->sock);
        goto _snapclient_open_exit;
    }
	result = esp_transport_write(snapclient->t,
								 hello_message_serialized, base_message.size,
								 snapclient->timeout_ms);
    if (result < 0) {
        _get_socket_error_code_reason("TCP write", snapclient->sock);
        goto _snapclient_open_exit;
    }
	free(hello_message_serialized);

    _dispatch_event(self, snapclient, NULL, 0, SNAPCLIENT_STREAM_STATE_CONNECTED);

    return ESP_OK;

_snapclient_open_exit:
    free(hello_message_serialized);
    return ESP_FAIL;
}

static esp_err_t _snapclient_close(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    snapclient_stream_t *snapclient = (snapclient_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, snapclient, return ESP_FAIL);
    if (!snapclient->is_open) {
        ESP_LOGE(TAG, "Already closed");
        return ESP_FAIL;
    }
    if (-1 == esp_transport_close(snapclient->t)) {
        ESP_LOGE(TAG, "Snapclient stream close failed");
        return ESP_FAIL;
    }
    snapclient->is_open = false;
    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        audio_element_set_byte_pos(self, 0);
    }
    return ESP_OK;
}

static esp_err_t _snapclient_read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    snapclient_stream_t *snapclient = (snapclient_stream_t *)audio_element_getdata(self);
    int rlen = esp_transport_read(snapclient->t, buffer, len, snapclient->timeout_ms);
    ESP_LOGD(TAG, "read len=%d, rlen=%d", len, rlen);
    if (rlen < 0) {
        _get_socket_error_code_reason("TCP read", snapclient->sock);
        return ESP_FAIL;
    } else if (rlen == 0) {
        ESP_LOGI(TAG, "Get end of the file");
    } else {
        audio_element_update_byte_pos(self, rlen);
    }
    return rlen;
}

static esp_err_t _snapclient_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    int w_size = 0;
    if (r_size > 0) {
        w_size = audio_element_output(self, in_buffer, r_size);
        if (w_size > 0) {
            audio_element_update_byte_pos(self, r_size);
        }
    } else {
        w_size = r_size;
    }
    return w_size;
}

static esp_err_t _snapclient_destroy(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    snapclient_stream_t *snapclient = (snapclient_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, snapclient, return ESP_FAIL);
    if (snapclient->t) {
        esp_transport_destroy(snapclient->t);
        snapclient->t = NULL;
    }
    audio_free(snapclient);
    return ESP_OK;
}

audio_element_handle_t snapclient_stream_init(snapclient_stream_cfg_t *config)
{
	AUDIO_NULL_CHECK(TAG, config, return NULL);

	audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
	audio_element_handle_t el;
    cfg.open = _snapclient_open;
    cfg.close = _snapclient_close;
    cfg.process = _snapclient_process;
    cfg.destroy = _snapclient_destroy;

    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.stack_in_ext = config->ext_stack;
    cfg.tag = "snapclient_client";

    if (cfg.buffer_len == 0) {
        cfg.buffer_len = SNAPCLIENT_STREAM_BUF_SIZE;
    }

	snapclient_stream_t *snapclient = audio_calloc(1, sizeof(snapclient_stream_t));
    AUDIO_MEM_CHECK(TAG, snapclient, return NULL);

	if (config->type == AUDIO_STREAM_READER) {
        cfg.read = _snapclient_read;
    } else if (config->type == AUDIO_STREAM_WRITER) {
        ESP_LOGE(TAG, "No writer for snapclient stream");
        goto _snapclient_init_exit;
    }

    snapclient->port = config->port;
    snapclient->host = config->host;
    snapclient->timeout_ms = config->timeout_ms;

    if (config->event_handler) {
        snapclient->hook = config->event_handler;
        if (config->event_ctx) {
            snapclient->ctx = config->event_ctx;
        }
    }

    el = audio_element_init(&cfg);
    AUDIO_MEM_CHECK(TAG, el, goto _snapclient_init_exit);
    audio_element_setdata(el, snapclient);

    return el;

_snapclient_init_exit:
    audio_free(snapclient);
    return NULL;

}
