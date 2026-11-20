#pragma once
#include "driver/i2c_master.h"
#include <stddef.h>
#include <stdint.h>
class TwoWire {
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_dev_handle_t dev = nullptr;
  uint8_t tx[128]{}, rx[128]{};
  size_t used = 0, pos = 0, received = 0;

public:
  bool healthy = true;
  bool begin(int sda, int scl);
  void beginTransmission(uint8_t) { used = 0; }
  size_t write(uint8_t b) {
    if (used >= sizeof(tx)) {
      healthy = false;
      return 0;
    }
    tx[used++] = b;
    return 1;
  }
  uint8_t endTransmission();
  size_t requestFrom(uint8_t, uint8_t n);
  int read() { return pos < received ? rx[pos++] : 255; }
};
extern TwoWire Wire;
