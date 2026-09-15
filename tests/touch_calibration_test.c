/* Host-side regression of the production calibration routine with a fake sensor.
 * No physical touch behavior is claimed by these tests. */
#include <assert.h>
#include <stdio.h>
#include "../components/touch_wake/touch_wake.c"

static TickType_t ticks;
static unsigned scans, reads, deleted, starts;
static int scenario, phase;
static uint32_t sample;

TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t n) { ticks += n; }
esp_err_t touch_sensor_new_controller(const touch_sensor_config_t *c, touch_sensor_handle_t *s)
{ (void)c; *s = &ticks; return ESP_OK; }
esp_err_t touch_sensor_new_channel(touch_sensor_handle_t s, int n, const touch_channel_config_t *c, touch_channel_handle_t *h)
{ (void)s; (void)n; (void)c; *h = &ticks; return ESP_OK; }
esp_err_t touch_sensor_get_channel_info(touch_channel_handle_t h, touch_chan_info_t *i)
{ (void)h; i->chan_gpio = 12; return ESP_OK; }
esp_err_t touch_sensor_config_filter(touch_sensor_handle_t h, const touch_sensor_filter_config_t *c)
{ (void)h; (void)c; return ESP_OK; }
esp_err_t touch_sensor_del_controller(touch_sensor_handle_t h) { (void)h; ++deleted; return ESP_OK; }
esp_err_t touch_sensor_del_channel(touch_channel_handle_t h) { (void)h; return ESP_OK; }
esp_err_t touch_sensor_enable(touch_sensor_handle_t h) { (void)h; return ESP_OK; }
esp_err_t touch_sensor_disable(touch_sensor_handle_t h) { (void)h; return ESP_OK; }
esp_err_t touch_sensor_start_continuous_scanning(touch_sensor_handle_t h) { (void)h; ++starts; return ESP_OK; }
esp_err_t touch_sensor_stop_continuous_scanning(touch_sensor_handle_t h) { (void)h; return ESP_OK; }
esp_err_t touch_sensor_trigger_oneshot_scanning(touch_sensor_handle_t h, int timeout)
{
    (void)h; assert(timeout == 2000); ++scans;
    if (scenario == 1 && scans == 10) return ESP_ERR_TIMEOUT;
    sample = scenario == 2 ? 0 : ((phase == 1) ? 8000 : 10000);
    return ESP_OK;
}
esp_err_t touch_channel_read_data(touch_channel_handle_t h, int type, uint32_t *v)
{ (void)h; assert(type == TOUCH_CHAN_DATA_TYPE_SMOOTH); assert(scans == reads + 4); ++reads; *v = sample; return ESP_OK; }
esp_err_t touch_sensor_reconfig_channel(touch_channel_handle_t h, const touch_channel_config_t *c)
{ (void)h; (void)c; return ESP_OK; }
esp_err_t touch_sensor_config_sleep_wakeup(touch_sensor_handle_t h, const touch_sleep_config_t *c)
{ (void)h; (void)c; return ESP_OK; }

static esp_err_t progress(touch_calibration_stage_t stage, uint8_t done, uint8_t total, void *ctx)
{
    (void)ctx;
    assert(total == 3);
    if (stage == TOUCH_CALIBRATION_STAGE_BASELINE) phase = 0;
    if (stage == TOUCH_CALIBRATION_STAGE_TOUCH_AND_HOLD) phase = scenario == 3 ? 0 : 1;
    if (stage == TOUCH_CALIBRATION_STAGE_RELEASE) phase = 0;
    if (stage == TOUCH_CALIBRATION_STAGE_VERIFY) {
        assert(done < total);
        phase = 1;
        if (scenario == 5) return ESP_ERR_INVALID_ARG;
    }
    if (stage == TOUCH_CALIBRATION_STAGE_VERIFY_RELEASE) {
        assert(done < total);
        phase = scenario == 4 ? 1 : 0;
    }
    return ESP_OK;
}

int main(void)
{
    for (scenario = 0; scenario < 6; ++scenario) {
        ticks = scans = reads = deleted = starts = 0;
        touch_calibration_result_t result;
        esp_err_t err = touch_wake_calibrate(progress, NULL, &result);
        assert(deleted == 1);
        assert(starts == 0); /* Calibration must not read a background cache. */
        if (scenario == 0) {
            assert(err == ESP_OK);
            assert(result.verified_touches == 3);
            assert(result.threshold_percent == 8);
        } else if (scenario == 1) {
            assert(err == ESP_ERR_TIMEOUT);
            assert(result.failure == TOUCH_CALIBRATION_FAILURE_HARDWARE);
        } else if (scenario == 2) {
            assert(err == ESP_ERR_INVALID_STATE);
        } else if (scenario == 5) {
            assert(err == ESP_ERR_INVALID_ARG);
            assert(result.failure == TOUCH_CALIBRATION_FAILURE_HARDWARE);
            assert(result.verified_touches == 0);
        } else {
            assert(result.verified_touches == 0);
            assert(err == ESP_ERR_TIMEOUT);
            assert(result.failure == TOUCH_CALIBRATION_FAILURE_TIMEOUT);
        }
    }
    puts("touch calibration tests passed");
}
