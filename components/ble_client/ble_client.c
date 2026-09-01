#include "ble_client.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"

static const char *TAG = "ble_client";

#define HOST_SYNC_BIT BIT0
#define GATT_TIMEOUT_MS 10000

static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(
        0x01, 0xc0, 0x71, 0x5b,
        0x2f, 0x6d, 0xb8, 0xa2,
        0x61, 0x4c, 0x7b, 0x3f,
        0x01, 0x00, 0x7a, 0x8f);

static const ble_uuid128_t s_snapshot_uuid =
    BLE_UUID128_INIT(
        0x01, 0xc0, 0x71, 0x5b,
        0x2f, 0x6d, 0xb8, 0xa2,
        0x61, 0x4c, 0x7b, 0x3f,
        0x02, 0x00, 0x7a, 0x8f);

static const ble_uuid128_t s_keg_name_uuid =
    BLE_UUID128_INIT(
        0x01, 0xc0, 0x71, 0x5b,
        0x2f, 0x6d, 0xb8, 0xa2,
        0x61, 0x4c, 0x7b, 0x3f,
        0x03, 0x00, 0x7a, 0x8f);

static const ble_uuid128_t s_device_info_uuid =
    BLE_UUID128_INIT(
        0x01, 0xc0, 0x71, 0x5b,
        0x2f, 0x6d, 0xb8, 0xa2,
        0x61, 0x4c, 0x7b, 0x3f,
        0x04, 0x00, 0x7a, 0x8f);

typedef struct __attribute__((packed)) {
    uint8_t protocol_version;
    uint8_t flags;
    uint16_t sequence;
    uint16_t remaining_percent_x100;
    uint16_t remaining_servings;
    uint16_t remaining_gallons_x100;
    int32_t total_weight_milli_lb;
    int32_t beverage_weight_milli_lb;
    int8_t wifi_rssi_dbm;
    uint8_t profile_revision;
} wire_snapshot_t;

_Static_assert(sizeof(wire_snapshot_t) == 20, "Scale BLE snapshot layout changed");

typedef struct {
    ble_client_peer_t *items;
    size_t capacity;
    size_t count;
    SemaphoreHandle_t done;
} scan_context_t;

typedef struct {
    SemaphoreHandle_t done;
    int status;
    uint16_t conn_handle;
} connect_context_t;

typedef struct {
    SemaphoreHandle_t done;
    int status;
    uint16_t start_handle;
    uint16_t end_handle;
} service_context_t;

typedef struct {
    SemaphoreHandle_t done;
    int status;
    uint16_t snapshot_handle;
    uint16_t keg_name_handle;
    uint16_t device_info_handle;
} characteristic_context_t;

typedef struct {
    SemaphoreHandle_t done;
    int status;
    uint8_t *buffer;
    size_t capacity;
    size_t length;
} read_context_t;

static EventGroupHandle_t s_host_events;
static uint8_t s_own_addr_type;
static bool s_initialized;

static bool same_address(
    const ble_client_peer_t *candidate,
    const ble_addr_t *address)
{
    return candidate->address_type == address->type &&
        memcmp(candidate->address, address->val, sizeof(candidate->address)) == 0;
}

static bool advertised_service_matches(const struct ble_hs_adv_fields *fields)
{
    for (int i = 0; i < fields->num_uuids128; ++i) {
        if (ble_uuid_cmp(
                &fields->uuids128[i].u,
                &s_service_uuid.u) == 0) {
            return true;
        }
    }

    return false;
}

static ble_client_peer_t *get_scan_candidate(
    scan_context_t *context,
    const ble_addr_t *address)
{
    for (size_t i = 0; i < context->count; ++i) {
        if (same_address(&context->items[i], address)) {
            return &context->items[i];
        }
    }

    if (context->count >= context->capacity) {
        return NULL;
    }

    ble_client_peer_t *candidate =
        &context->items[context->count++];

    memset(candidate, 0, sizeof(*candidate));
    candidate->address_type = address->type;
    memcpy(candidate->address, address->val, sizeof(candidate->address));
    candidate->rssi = -127;

    return candidate;
}

static int scan_gap_event(struct ble_gap_event *event, void *arg)
{
    scan_context_t *context = arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields = {0};

            if (ble_hs_adv_parse_fields(
                    &fields,
                    event->disc.data,
                    event->disc.length_data) != 0) {
                break;
            }

            const bool has_service =
                advertised_service_matches(&fields);

            bool has_scale_name = false;

            if (fields.name != NULL &&
                fields.name_len >= 9) {
                static const char prefix[] =
                    "kegscale-";

                has_scale_name = true;

                for (size_t i = 0;
                     i < sizeof(prefix) - 1;
                     ++i) {
                    if ((char)tolower(
                            (unsigned char)fields.name[i]) !=
                        prefix[i]) {
                        has_scale_name = false;
                        break;
                    }
                }
            }

            if (!has_service && !has_scale_name) {
                break;
            }

            ble_client_peer_t *candidate =
                get_scan_candidate(
                    context,
                    &event->disc.addr);

            if (candidate == NULL) {
                break;
            }

            candidate->rssi = event->disc.rssi;

            if (has_service) {
                candidate->service_seen = true;
            }

            if (has_scale_name) {
                size_t name_len = fields.name_len;

                if (name_len > BLE_CLIENT_SCALE_ID_MAX) {
                    name_len = BLE_CLIENT_SCALE_ID_MAX;
                }

                memcpy(candidate->scale_id, fields.name, name_len);
                candidate->scale_id[name_len] = '\0';
            }

            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            xSemaphoreGive(context->done);
            break;

        default:
            break;
    }

    return 0;
}

static int connect_gap_event(struct ble_gap_event *event, void *arg)
{
    connect_context_t *context = arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            context->status = event->connect.status;

            if (event->connect.status == 0) {
                context->conn_handle =
                    event->connect.conn_handle;
            }

            xSemaphoreGive(context->done);
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            if (context->conn_handle ==
                event->disconnect.conn.conn_handle) {
                ESP_LOGI(
                    TAG,
                    "Scale disconnected; reason=%d",
                    event->disconnect.reason);
            }
            break;

        default:
            break;
    }

    return 0;
}

static int service_disc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service,
    void *arg)
{
    (void)conn_handle;
    service_context_t *context = arg;

    if (error->status == 0 && service != NULL) {
        context->start_handle = service->start_handle;
        context->end_handle = service->end_handle;
        return 0;
    }

    context->status = error->status;

    if (error->status == BLE_HS_EDONE &&
        context->start_handle != 0) {
        context->status = 0;
    }

    xSemaphoreGive(context->done);
    return 0;
}

static int characteristic_disc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic,
    void *arg)
{
    (void)conn_handle;
    characteristic_context_t *context = arg;

    if (error->status == 0 &&
        characteristic != NULL) {
        if (ble_uuid_cmp(
                &characteristic->uuid.u,
                &s_snapshot_uuid.u) == 0) {
            context->snapshot_handle =
                characteristic->val_handle;
        } else if (ble_uuid_cmp(
                       &characteristic->uuid.u,
                       &s_keg_name_uuid.u) == 0) {
            context->keg_name_handle =
                characteristic->val_handle;
        } else if (ble_uuid_cmp(
                       &characteristic->uuid.u,
                       &s_device_info_uuid.u) == 0) {
            context->device_info_handle =
                characteristic->val_handle;
        }

        return 0;
    }

    context->status = error->status;

    if (error->status == BLE_HS_EDONE &&
        context->snapshot_handle != 0 &&
        context->keg_name_handle != 0 &&
        context->device_info_handle != 0) {
        context->status = 0;
    }

    xSemaphoreGive(context->done);
    return 0;
}

static int read_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg)
{
    (void)conn_handle;
    read_context_t *context = arg;

    context->status = error->status;

    if (error->status == 0 &&
        attr != NULL &&
        attr->om != NULL) {
        size_t packet_len =
            OS_MBUF_PKTLEN(attr->om);

        if (packet_len > context->capacity) {
            packet_len = context->capacity;
        }

        if (os_mbuf_copydata(
                attr->om,
                0,
                packet_len,
                context->buffer) == 0) {
            context->length = packet_len;
        } else {
            context->status = BLE_HS_EAPP;
        }
    }

    xSemaphoreGive(context->done);
    return 0;
}

static int read_long_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg)
{
    (void)conn_handle;
    read_context_t *context = arg;

    if (error->status == 0 &&
        attr != NULL &&
        attr->om != NULL) {
        size_t packet_len =
            OS_MBUF_PKTLEN(attr->om);

        if (context->length + packet_len >
            context->capacity) {
            context->status = BLE_HS_EMSGSIZE;
            xSemaphoreGive(context->done);
            return BLE_HS_EMSGSIZE;
        }

        if (os_mbuf_copydata(
                attr->om,
                0,
                packet_len,
                context->buffer +
                    context->length) != 0) {
            context->status = BLE_HS_EAPP;
            xSemaphoreGive(context->done);
            return BLE_HS_EAPP;
        }

        context->length += packet_len;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        context->status = 0;
        xSemaphoreGive(context->done);
        return 0;
    }

    context->status = error->status;
    xSemaphoreGive(context->done);
    return 0;
}

static esp_err_t wait_sem(SemaphoreHandle_t sem)
{
    return xSemaphoreTake(
               sem,
               pdMS_TO_TICKS(GATT_TIMEOUT_MS)) == pdTRUE ?
        ESP_OK :
        ESP_ERR_TIMEOUT;
}

static esp_err_t read_value(
    uint16_t conn_handle,
    uint16_t value_handle,
    uint8_t *buffer,
    size_t capacity,
    size_t *length)
{
    SemaphoreHandle_t done =
        xSemaphoreCreateBinary();

    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    read_context_t context = {
        .done = done,
        .status = BLE_HS_EAPP,
        .buffer = buffer,
        .capacity = capacity,
    };

    int rc =
        ble_gattc_read(
            conn_handle,
            value_handle,
            read_cb,
            &context);

    if (rc != 0) {
        vSemaphoreDelete(done);
        return ESP_FAIL;
    }

    esp_err_t err = wait_sem(done);
    vSemaphoreDelete(done);

    if (err != ESP_OK) {
        return err;
    }

    if (context.status != 0) {
        return ESP_FAIL;
    }

    *length = context.length;
    return ESP_OK;
}

static esp_err_t read_long_value(
    uint16_t conn_handle,
    uint16_t value_handle,
    uint8_t *buffer,
    size_t capacity,
    size_t *length)
{
    SemaphoreHandle_t done =
        xSemaphoreCreateBinary();

    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    read_context_t context = {
        .done = done,
        .status = BLE_HS_EAPP,
        .buffer = buffer,
        .capacity = capacity,
    };

    int rc =
        ble_gattc_read_long(
            conn_handle,
            value_handle,
            0,
            read_long_cb,
            &context);

    if (rc != 0) {
        vSemaphoreDelete(done);
        return ESP_FAIL;
    }

    esp_err_t err = wait_sem(done);
    vSemaphoreDelete(done);

    if (err != ESP_OK) {
        return err;
    }

    if (context.status != 0) {
        return ESP_FAIL;
    }

    *length = context.length;
    return ESP_OK;
}

static void host_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);

    if (s_host_events != NULL) {
        xEventGroupClearBits(
            s_host_events,
            HOST_SYNC_BIT);
    }
}

static void host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);

    if (rc == 0) {
        rc = ble_hs_id_infer_auto(
            0,
            &s_own_addr_type);
    }

    if (rc != 0) {
        ESP_LOGE(
            TAG,
            "Could not configure BLE identity; rc=%d",
            rc);
        return;
    }

    xEventGroupSetBits(
        s_host_events,
        HOST_SYNC_BIT);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_host_events = xEventGroupCreate();

    if (s_host_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();

    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;

    nimble_port_freertos_init(host_task);

    EventBits_t bits =
        xEventGroupWaitBits(
            s_host_events,
            HOST_SYNC_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(GATT_TIMEOUT_MS));

    if ((bits & HOST_SYNC_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "BLE client ready");

    return ESP_OK;
}

esp_err_t ble_client_scan(
    ble_client_peer_t *candidates,
    size_t capacity,
    size_t *count,
    uint32_t duration_ms)
{
    if (!s_initialized ||
        candidates == NULL ||
        capacity == 0 ||
        count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        candidates,
        0,
        capacity * sizeof(*candidates));

    SemaphoreHandle_t done =
        xSemaphoreCreateBinary();

    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    scan_context_t context = {
        .items = candidates,
        .capacity = capacity,
        .done = done,
    };

    struct ble_gap_disc_params params = {0};
    params.passive = 0;
    params.filter_duplicates = 0;

    int rc =
        ble_gap_disc(
            s_own_addr_type,
            (int32_t)duration_ms,
            &params,
            scan_gap_event,
            &context);

    if (rc != 0) {
        vSemaphoreDelete(done);
        return ESP_FAIL;
    }

    TickType_t timeout =
        pdMS_TO_TICKS(duration_ms + 2000U);

    if (xSemaphoreTake(done, timeout) != pdTRUE) {
        ble_gap_disc_cancel();
        vSemaphoreDelete(done);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done);

    size_t out = 0;

    for (size_t i = 0; i < context.count; ++i) {
        /*
         * Some BLE stacks / legacy advertisers split the device name and
         * 128-bit service UUID across advertisement and scan-response packets
         * in ways that are not always surfaced identically by the scanner.
         * Keep any KegScale-named candidate here and validate the actual
         * service + protocol after connecting before it can be paired.
         */
        if (context.items[i].scale_id[0] == '\0') {
            continue;
        }

        if (out != i) {
            context.items[out] =
                context.items[i];
        }

        ++out;
    }

    *count = out;

    ESP_LOGI(
        TAG,
        "Found %u compatible Keg Scale device(s)",
        (unsigned)out);

    return ESP_OK;
}

static esp_err_t connect_peer(
    const ble_client_peer_t *peer,
    uint16_t *conn_handle,
    connect_context_t *connect_context)
{
    SemaphoreHandle_t done =
        xSemaphoreCreateBinary();

    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *connect_context = (connect_context_t) {
        .done = done,
        .status = BLE_HS_EAPP,
        .conn_handle = BLE_HS_CONN_HANDLE_NONE,
    };

    ble_addr_t address = {
        .type = peer->address_type,
    };

    memcpy(address.val, peer->address, sizeof(address.val));

    int rc =
        ble_gap_connect(
            s_own_addr_type,
            &address,
            GATT_TIMEOUT_MS,
            NULL,
            connect_gap_event,
            connect_context);

    if (rc != 0) {
        vSemaphoreDelete(done);
        return ESP_FAIL;
    }

    esp_err_t err = wait_sem(done);
    vSemaphoreDelete(done);
    connect_context->done = NULL;

    if (err != ESP_OK) {
        return err;
    }

    if (connect_context->status != 0 ||
        connect_context->conn_handle ==
            BLE_HS_CONN_HANDLE_NONE) {
        return ESP_FAIL;
    }

    *conn_handle = connect_context->conn_handle;
    return ESP_OK;
}

static esp_err_t discover_handles(
    uint16_t conn_handle,
    uint16_t *snapshot_handle,
    uint16_t *keg_name_handle,
    uint16_t *device_info_handle)
{
    SemaphoreHandle_t service_done =
        xSemaphoreCreateBinary();

    if (service_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    service_context_t service_context = {
        .done = service_done,
        .status = BLE_HS_EAPP,
    };

    int rc =
        ble_gattc_disc_svc_by_uuid(
            conn_handle,
            &s_service_uuid.u,
            service_disc_cb,
            &service_context);

    if (rc != 0) {
        vSemaphoreDelete(service_done);
        return ESP_FAIL;
    }

    esp_err_t err = wait_sem(service_done);
    vSemaphoreDelete(service_done);

    if (err != ESP_OK ||
        service_context.status != 0 ||
        service_context.start_handle == 0) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    SemaphoreHandle_t chr_done =
        xSemaphoreCreateBinary();

    if (chr_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    characteristic_context_t chr_context = {
        .done = chr_done,
        .status = BLE_HS_EAPP,
    };

    rc =
        ble_gattc_disc_all_chrs(
            conn_handle,
            service_context.start_handle,
            service_context.end_handle,
            characteristic_disc_cb,
            &chr_context);

    if (rc != 0) {
        vSemaphoreDelete(chr_done);
        return ESP_FAIL;
    }

    err = wait_sem(chr_done);
    vSemaphoreDelete(chr_done);

    if (err != ESP_OK ||
        chr_context.status != 0) {
        return err != ESP_OK ? err : ESP_FAIL;
    }

    *snapshot_handle = chr_context.snapshot_handle;
    *keg_name_handle = chr_context.keg_name_handle;
    *device_info_handle = chr_context.device_info_handle;

    return ESP_OK;
}

esp_err_t ble_client_fetch(
    const ble_client_peer_t *peer,
    ble_client_scale_state_t *state)
{
    if (!s_initialized ||
        peer == NULL ||
        state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));

    uint16_t conn_handle =
        BLE_HS_CONN_HANDLE_NONE;

    connect_context_t connection = {0};

    esp_err_t err =
        connect_peer(
            peer,
            &conn_handle,
            &connection);

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not connect to %s",
            peer->scale_id);
        return err;
    }

    uint16_t snapshot_handle = 0;
    uint16_t keg_name_handle = 0;
    uint16_t device_info_handle = 0;

    err =
        discover_handles(
            conn_handle,
            &snapshot_handle,
            &keg_name_handle,
            &device_info_handle);

    uint8_t raw_snapshot[sizeof(wire_snapshot_t)] = {0};
    size_t raw_snapshot_len = 0;

    if (err == ESP_OK) {
        err =
            read_value(
                conn_handle,
                snapshot_handle,
                raw_snapshot,
                sizeof(raw_snapshot),
                &raw_snapshot_len);
    }

    size_t text_len = 0;

    if (err == ESP_OK) {
        err =
            read_long_value(
                conn_handle,
                keg_name_handle,
                (uint8_t *)state->keg_name,
                BLE_CLIENT_KEG_NAME_MAX,
                &text_len);

        state->keg_name[text_len] = '\0';
    }

    text_len = 0;

    if (err == ESP_OK) {
        err =
            read_long_value(
                conn_handle,
                device_info_handle,
                (uint8_t *)state->device_info,
                BLE_CLIENT_DEVICE_INFO_MAX,
                &text_len);

        state->device_info[text_len] = '\0';
    }

    ble_gap_terminate(
        conn_handle,
        BLE_ERR_REM_USER_CONN_TERM);

    if (err != ESP_OK) {
        return err;
    }

    if (raw_snapshot_len !=
        sizeof(wire_snapshot_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    wire_snapshot_t wire;
    memcpy(&wire, raw_snapshot, sizeof(wire));

    state->protocol_version =
        wire.protocol_version;
    state->flags =
        wire.flags;
    state->sequence =
        wire.sequence;
    state->remaining_percent =
        (float)wire.remaining_percent_x100 /
        100.0f;
    state->remaining_servings =
        wire.remaining_servings;
    state->remaining_gallons =
        (float)wire.remaining_gallons_x100 /
        100.0f;
    state->total_weight_lbs =
        (float)wire.total_weight_milli_lb /
        1000.0f;
    state->beverage_weight_lbs =
        (float)wire.beverage_weight_milli_lb /
        1000.0f;
    state->wifi_rssi_dbm =
        wire.wifi_rssi_dbm;
    state->profile_revision =
        wire.profile_revision;

    ESP_LOGI(
        TAG,
        "%s: seq=%u servings=%u remaining=%.2f%% weight=%.3f lb",
        peer->scale_id,
        (unsigned)state->sequence,
        (unsigned)state->remaining_servings,
        (double)state->remaining_percent,
        (double)state->total_weight_lbs);

    ESP_LOGI(
        TAG,
        "Scale identity: %s",
        state->device_info);

    return ESP_OK;
}

bool ble_client_state_is_compatible(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state)
{
    if (peer == NULL ||
        state == NULL ||
        state->protocol_version !=
            BLE_CLIENT_PROTOCOL_VERSION) {
        return false;
    }

    char expected[48];
    snprintf(
        expected,
        sizeof(expected),
        "protocol=%u;id=%s;",
        BLE_CLIENT_PROTOCOL_VERSION,
        peer->scale_id);

    return strncmp(
               state->device_info,
               expected,
               strlen(expected)) == 0;
}

void ble_client_format_address(
    const ble_client_peer_t *peer,
    char *buffer,
    size_t buffer_size)
{
    if (peer == NULL ||
        buffer == NULL ||
        buffer_size == 0) {
        return;
    }

    snprintf(
        buffer,
        buffer_size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        peer->address[5],
        peer->address[4],
        peer->address[3],
        peer->address[2],
        peer->address[1],
        peer->address[0]);
}
