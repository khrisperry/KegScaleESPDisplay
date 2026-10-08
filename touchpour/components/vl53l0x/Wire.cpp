#include "Wire.h"
TwoWire Wire;
bool TwoWire::begin(int sda, int scl) {
  i2c_master_bus_config_t c = {};
  c.i2c_port = I2C_NUM_0;
  c.sda_io_num = (gpio_num_t)sda;
  c.scl_io_num = (gpio_num_t)scl;
  c.clk_source = I2C_CLK_SRC_DEFAULT;
  c.glitch_ignore_cnt = 7;
  c.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&c, &bus) != ESP_OK)
    return false;
  i2c_device_config_t d = {};
  d.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  d.device_address = 0x29;
  d.scl_speed_hz = 100000;
  return i2c_master_bus_add_device(bus, &d, &dev) == ESP_OK;
}
uint8_t TwoWire::endTransmission() {
  bool ok = dev && i2c_master_transmit(dev, tx, used, 20) == ESP_OK;
  healthy &= ok;
  return ok ? 0 : 4;
}
size_t TwoWire::requestFrom(uint8_t, uint8_t n) {
  pos = 0;
  received = 0;
  bool ok =
      dev && n <= sizeof(rx) && i2c_master_receive(dev, rx, n, 20) == ESP_OK;
  healthy &= ok;
  if (ok)
    received = n;
  return received;
}
