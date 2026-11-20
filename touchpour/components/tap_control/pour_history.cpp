#include "pour_history.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <cstring>
#include <type_traits>

namespace tap {
namespace {
constexpr size_t slot_count = 6, slot_size = 0x4000, max_points = 242;
constexpr uint32_t magic = 0x504f5552, format = 1;
struct Header {
  uint32_t magic_word, format_version, count, data_crc, header_crc;
  PourSummary summary;
};
static_assert(std::is_trivially_copyable<PourPoint>::value, "History rows must be POD");
static_assert(sizeof(Header) + max_points * sizeof(PourPoint) < slot_size, "History slot capacity");
const esp_partition_t *partition;
SemaphoreHandle_t mutex;
TaskHandle_t writer;
Header headers[slot_count]{};
PourPoint points[max_points]{};
Header pending{};
bool recording = false, pending_button = false;
uint32_t started = 0, last_at = 0, next_id = 1;
uint32_t crc(const void *data, size_t bytes, uint32_t value = 0xffffffff) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (bytes--) {
    value ^= *p++;
    for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return value;
}
bool valid_header(const Header &source) {
  Header h; memcpy(&h, &source, sizeof(h));
  uint32_t stored = h.header_crc; h.header_crc = 0;
  return h.magic_word == magic && h.format_version == format && h.count > 0 &&
         h.count <= max_points && h.summary.samples == h.count && h.summary.id > 0 &&
         crc(&h, sizeof(h)) == stored;
}
void load_slots() {
  for (size_t slot = 0; slot < slot_count; ++slot) {
    Header h{};
    if (esp_partition_read(partition, slot * slot_size, &h, sizeof(h)) != ESP_OK || !valid_header(h)) continue;
    uint8_t block[512]; uint32_t check = 0xffffffff;
    size_t bytes = h.count * sizeof(PourPoint), offset = slot * slot_size + sizeof(Header);
    bool ok = true;
    while (bytes) {
      size_t n = std::min(bytes, sizeof(block));
      if (esp_partition_read(partition, offset, block, n) != ESP_OK) { ok = false; break; }
      check = crc(block, n, check); offset += n; bytes -= n;
    }
    if (ok && check == h.data_crc) {
      headers[slot] = h;
      next_id = std::max(next_id, h.summary.id + 1);
    }
  }
}
void writer_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(mutex, portMAX_DELAY);
    // Six slots retain five pours: replace an invalid/oldest slot only after
    // payload and header are written. Commit magic last for torn-write recovery.
    size_t slot = 0;
    for (size_t i = 1; i < slot_count; ++i)
      if (headers[i].summary.id < headers[slot].summary.id) slot = i;
    pending.summary.id = next_id++;
    pending.magic_word = magic; pending.format_version = format;
    pending.summary.samples = pending.count;
    pending.data_crc = crc(points, pending.count * sizeof(PourPoint));
    pending.header_crc = 0; pending.header_crc = crc(&pending, sizeof(pending));
    size_t offset = slot * slot_size;
    esp_err_t e = esp_partition_erase_range(partition, offset, slot_size);
    if (e == ESP_OK) {
      headers[slot] = {};
      e = esp_partition_write(partition, offset + sizeof(Header), points, pending.count * sizeof(PourPoint));
    }
    if (e == ESP_OK)
      e = esp_partition_write(partition, offset + sizeof(uint32_t), reinterpret_cast<const uint8_t *>(&pending) + sizeof(uint32_t), sizeof(Header) - sizeof(uint32_t));
    if (e == ESP_OK) e = esp_partition_write(partition, offset, &pending.magic_word, sizeof(uint32_t));
    if (e == ESP_OK) headers[slot] = pending;
    uint32_t id = pending.summary.id, count = pending.count;
    xSemaphoreGive(mutex);
    history_saving = false;
    if (e == ESP_OK) ESP_LOGI("tap_history", "Saved pour %lu: %lu readings, retained last five", (unsigned long)id, (unsigned long)count);
    else ESP_LOGE("tap_history", "Pour save failed: %s", esp_err_to_name(e));
  }
}
void add_point(const Sample &s, uint32_t now, int servo, bool hold) {
  PourPoint row{now - started, s.sequence, now - s.at, s.mm, (int16_t)s.status,
                (int16_t)servo, (uint8_t)s.valid, (uint8_t)hold,
                (uint8_t)pending_button, 0};
  if (pending.count < max_points) points[pending.count++] = row;
  else points[max_points - 1] = row;
  pending_button = false; last_at = now;
}
}
std::atomic<bool> history_saving{false};
void history_init() {
  mutex = xSemaphoreCreateMutex(); configASSERT(mutex);
  partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "pour_history");
  if (!partition) {
    // The released OTA layout leaves this tail unused. Registration checks
    // flash bounds and overlap; this also supports updates without a USB migration.
    auto e = esp_partition_register_external(nullptr, 0xc20000, slot_count * slot_size,
        "pour_history", ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, &partition);
    if (e != ESP_OK) { ESP_LOGE("tap_history", "History unavailable: %s", esp_err_to_name(e)); return; }
  }
  if (partition->address != 0xc20000 || partition->size < slot_count * slot_size) {
    partition = nullptr; ESP_LOGE("tap_history", "Unexpected history partition layout"); return;
  }
  load_slots();
  configASSERT(xTaskCreate(writer_task, "pour_history", 4096, nullptr, 2, &writer) == pdPASS);
  ESP_LOGI("tap_history", "Persistent last-five-pour storage ready");
}
bool history_available() { return partition != nullptr; }
void history_observe(const Sample &s, const Settings &c, uint32_t now, int servo,
                     bool pouring, bool hold, bool button, const char *reason) {
  if (!partition || history_saving) return;
  if (pouring && !recording) {
    pending = {}; pending.summary.settings = c; pending.summary.manual_hold = hold;
    std::strncpy(pending.summary.firmware, esp_app_get_description()->version, sizeof(pending.summary.firmware) - 1);
    started = now; last_at = now; pending_button = button; recording = true;
    add_point(s, now, servo, hold);
  } else if (recording) {
    pending_button |= button;
    if (!pouring || now - last_at >= 500) add_point(s, now, servo, hold);
    if (!pouring) {
      pending.summary.duration_ms = now - started;
      std::strncpy(pending.summary.reason, reason, sizeof(pending.summary.reason) - 1);
      recording = false; history_saving = true;
      xTaskNotifyGive(writer);
    }
  }
}
size_t history_list(PourSummary out[5]) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  PourSummary sorted[slot_count]{}; size_t count = 0;
  for (const auto &h : headers) if (h.summary.id) sorted[count++] = h.summary;
  std::sort(sorted, sorted + count, [](const auto &a, const auto &b) { return a.id > b.id; });
  count = std::min(count, size_t(5));
  std::copy(sorted, sorted + count, out);
  xSemaphoreGive(mutex); return count;
}
bool history_get(uint32_t id, PourSummary &out) {
  PourSummary saved[5]; size_t n = history_list(saved);
  for (size_t i = 0; i < n; ++i) if (saved[i].id == id) { out = saved[i]; return true; }
  return false;
}
bool history_point(uint32_t id, size_t index, PourPoint &out) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = false;
  for (size_t slot = 0; slot < slot_count; ++slot) {
    if (headers[slot].summary.id != id || index >= headers[slot].count) continue;
    ok = esp_partition_read(partition, slot * slot_size + sizeof(Header) + index * sizeof(PourPoint), &out, sizeof(out)) == ESP_OK;
    break;
  }
  xSemaphoreGive(mutex); return ok;
}


} // namespace tap
