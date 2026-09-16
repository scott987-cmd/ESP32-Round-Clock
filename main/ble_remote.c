#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "ble_remote.h"

#define HID_REPORT_ID_KEYBOARD 1
#define HID_REPORT_ID_AUDIO 2
#define HID_REPORT_ID_AUDIO_ACK 3
#define HID_REPORT_ID_PHOTO 4
#define HID_REPORT_ID_PHOTO_ACK 5
#define HID_MODIFIER_LEFT_GUI 0x08
#define HID_MODIFIER_RIGHT_ALT 0x40
#define HID_KEY_RETURN 0x28
#define HID_KEY_V 0x19
#define HID_KEY_Z 0x1D

static const char *TAG = "vibe_remote";
static esp_hidd_dev_t *hid_device;
static bool initialized;
static bool hid_service_started;
static bool advertising;
static bool advertising_data_ready;
static bool pairing_window;
static bool bond_cleanup_pending;
static int pending_bond_removals;
static bool authenticated;
static TimerHandle_t pairing_timer;
static char remote_status[64] = "BLUETOOTH OFF";
static vibe_remote_audio_ack_cb_t audio_ack_callback;
static vibe_remote_photo_packet_cb_t photo_packet_callback;

static void start_advertising(void);

static esp_err_t refresh_pairing_passkey(void)
{
    /* A fresh code prevents a leaked development passkey from remaining useful. */
    uint32_t passkey = 100000U + (esp_random() % 900000U);
    return esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey,
                                          sizeof(passkey));
}

static const uint8_t keyboard_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, HID_REPORT_ID_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08,
    0x81, 0x03,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

static esp_hid_raw_report_map_t report_maps[] = {
    { .data = keyboard_report_map, .len = sizeof(keyboard_report_map) },
};

static const esp_hid_device_config_t hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x175C,
    .version = 0x0100,
    .device_name = "Round Clock",
    .manufacturer_name = "Waveshare",
    .serial_number = "175C-CLOCK",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static const uint8_t hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t advertising_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = ESP_HID_APPEARANCE_KEYBOARD,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = (uint8_t *)hidd_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t advertising_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void set_remote_status(const char *status)
{
    strlcpy(remote_status, status, sizeof(remote_status));
    ESP_LOGI(TAG, "%s", remote_status);
}

static void start_advertising(void)
{
    if (!pairing_window || bond_cleanup_pending || advertising ||
        !advertising_data_ready || !hid_service_started) {
        if (pairing_window && !advertising) {
            snprintf(remote_status, sizeof(remote_status), "ADV WAIT H%u D%u",
                     (unsigned)hid_service_started, (unsigned)advertising_data_ready);
        }
        return;
    }
    esp_err_t result = esp_ble_gap_start_advertising(&advertising_params);
    if (result == ESP_OK) {
        advertising = true;
        set_remote_status("PAIRING OPEN 5 MINUTES");
    } else {
        snprintf(remote_status, sizeof(remote_status), "BLE START ERR 0x%X", (unsigned)result);
        ESP_LOGE(TAG, "Advertising start failed: %s", esp_err_to_name(result));
    }
}

static void stop_pairing_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    pairing_window = false;
    if (advertising) {
        esp_ble_gap_stop_advertising();
        advertising = false;
    }
    if (!authenticated) {
        set_remote_status("PAIRING CLOSED");
    }
}

static void hid_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    if (id == ESP_HIDD_START_EVENT) {
        hid_service_started = true;
        start_advertising();
    } else if (id == ESP_HIDD_CONNECT_EVENT) {
        advertising = false;
        authenticated = false;
        set_remote_status("DEVICE CONNECTED - VERIFYING");
    } else if (id == ESP_HIDD_DISCONNECT_EVENT) {
        advertising = false;
        authenticated = false;
        if (pairing_window) {
            set_remote_status("PAIRING OPEN 5 MINUTES");
            start_advertising();
        } else {
            set_remote_status("DEVICE DISCONNECTED");
        }
    } else if (id == ESP_HIDD_OUTPUT_EVENT && event_data != NULL) {
        esp_hidd_event_data_t *event = event_data;
        if (authenticated && event->output.report_id == HID_REPORT_ID_AUDIO_ACK &&
            event->output.length >= 4 && event->output.data[0] == 1 &&
            audio_ack_callback != NULL) {
            uint16_t session = (uint16_t)event->output.data[2] |
                               ((uint16_t)event->output.data[3] << 8);
            audio_ack_callback(session, event->output.data[1]);
        } else if (authenticated && event->output.report_id == HID_REPORT_ID_PHOTO &&
                   event->output.length == 180 && photo_packet_callback != NULL) {
            photo_packet_callback(event->output.data, event->output.length);
        }
    }
}

static void gap_event_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        advertising_data_ready = true;
        start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            advertising = false;
            size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            snprintf(remote_status, sizeof(remote_status), "ADV NOMEM I%u/%uK",
                     (unsigned)(internal_free / 1024), (unsigned)(internal_largest / 1024));
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        advertising = false;
        break;
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
        if (pending_bond_removals > 0) {
            pending_bond_removals--;
        }
        if (pending_bond_removals == 0) {
            bond_cleanup_pending = false;
            start_advertising();
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, pairing_window);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        snprintf(remote_status, sizeof(remote_status), "PAIR CODE %06lu",
                 (unsigned long)param->ble_security.key_notif.passkey);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        authenticated = param->ble_security.auth_cmpl.success;
        if (authenticated) {
            pairing_window = false;
            set_remote_status("DEVICE CONNECTED - SECURED");
        } else {
            set_remote_status("PAIRING FAILED");
            ESP_LOGW(TAG, "BLE authentication failed: 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

static esp_err_t initialize_remote(void)
{
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT), TAG,
                        "BT classic memory release failed");
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&controller_config), TAG, "BT controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "BT controller enable failed");
    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_config), TAG,
                        "Bluedroid init failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "Bluedroid enable failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatt_set_local_mtu(185), TAG, "BLE MTU setup failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_cb), TAG, "BLE GAP callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler), TAG,
                        "GATTS callback failed");

    /*
     * Match ESP-IDF's high-level BLE HID keyboard example. A keyboard must use
     * authenticated pairing on macOS; DISPLAY_ONLY makes the host ask for the
     * passkey shown by ESP_GAP_BLE_PASSKEY_NOTIF_EVT.
     */
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t io_capability = ESP_IO_CAP_OUT;
    uint8_t init_keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t response_keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)),
                        TAG, "BLE security mode failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_capability,
                                                       sizeof(io_capability)),
                        TAG, "BLE IO capability failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_keys,
                                                       sizeof(init_keys)),
                        TAG, "BLE initiator keys failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &response_keys,
                                                       sizeof(response_keys)),
                        TAG, "BLE responder keys failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size,
                                                       sizeof(key_size)),
                        TAG, "BLE key size failed");
    ESP_RETURN_ON_ERROR(refresh_pairing_passkey(),
                        TAG, "BLE passkey failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(hid_config.device_name), TAG, "BLE name failed");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hid_event_cb,
                                          &hid_device), TAG, "HID init failed");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_battery_set(hid_device, 100), TAG, "HID battery failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&advertising_data), TAG,
                        "BLE advertising data failed");
    initialized = true;
    return ESP_OK;
}

esp_err_t vibe_remote_prepare(void)
{
    esp_err_t result = initialize_remote();
    if (result == ESP_OK) {
        set_remote_status("BLUETOOTH READY");
    } else {
        set_remote_status("BLUETOOTH UNAVAILABLE");
    }
    return result;
}

esp_err_t vibe_remote_start_pairing(void)
{
    bool was_initialized = initialized;
    pairing_window = true;
    bond_cleanup_pending = false;
    pending_bond_removals = 0;
    esp_err_t result = initialize_remote();
    if (result != ESP_OK) {
        pairing_window = false;
        bond_cleanup_pending = false;
        set_remote_status("BLUETOOTH UNAVAILABLE");
        return result;
    }
    if (was_initialized) {
        result = refresh_pairing_passkey();
        if (result != ESP_OK) {
            pairing_window = false;
            set_remote_status("BLUETOOTH UNAVAILABLE");
            return result;
        }
    }

    if (pairing_timer == NULL) {
        pairing_timer = xTimerCreate("ble_pair", pdMS_TO_TICKS(300000), pdFALSE, NULL,
                                     stop_pairing_timer_cb);
    }
    if (pairing_timer == NULL) {
        pairing_window = false;
        set_remote_status("BLUETOOTH UNAVAILABLE");
        return ESP_ERR_NO_MEM;
    }
    xTimerReset(pairing_timer, 0);
    start_advertising();
    return ESP_OK;
}

bool vibe_remote_is_ready(void)
{
    return authenticated && hid_device != NULL && esp_hidd_dev_connected(hid_device);
}

const char *vibe_remote_status(void)
{
    return remote_status;
}

void vibe_remote_set_status(const char *status)
{
    if (status != NULL) {
        set_remote_status(status);
    }
}

void vibe_remote_set_audio_ack_callback(vibe_remote_audio_ack_cb_t callback)
{
    audio_ack_callback = callback;
}

void vibe_remote_set_photo_packet_callback(vibe_remote_photo_packet_cb_t callback)
{
    photo_packet_callback = callback;
}

static esp_err_t send_key(uint8_t modifier, uint8_t key)
{
    if (!vibe_remote_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t report[8] = { modifier, 0, key, 0, 0, 0, 0, 0 };
    ESP_RETURN_ON_ERROR(esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_KEYBOARD,
                                                report, sizeof(report)), TAG, "Key press failed");
    vTaskDelay(pdMS_TO_TICKS(45));
    memset(report, 0, sizeof(report));
    return esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_KEYBOARD, report, sizeof(report));
}

esp_err_t vibe_remote_send_voice(void)
{
    return send_key(HID_MODIFIER_RIGHT_ALT, 0);
}

esp_err_t vibe_remote_send_confirm(void)
{
    return send_key(0, HID_KEY_RETURN);
}

esp_err_t vibe_remote_send_undo(void)
{
    return send_key(HID_MODIFIER_LEFT_GUI, HID_KEY_Z);
}

esp_err_t vibe_remote_send_paste(void)
{
    return send_key(HID_MODIFIER_LEFT_GUI, HID_KEY_V);
}

esp_err_t vibe_remote_send_audio_packet(const uint8_t *packet, size_t length)
{
    if (!vibe_remote_is_ready() || packet == NULL || length != 180) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_AUDIO,
                                  (uint8_t *)packet, length);
}

esp_err_t vibe_remote_send_photo_ack(uint16_t session, uint8_t status, uint8_t slot)
{
    if (!vibe_remote_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t report[8] = {
        1, status, (uint8_t)(session & 0xFF), (uint8_t)(session >> 8), slot, 0, 0, 0,
    };
    return esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_PHOTO_ACK,
                                  report, sizeof(report));
}
