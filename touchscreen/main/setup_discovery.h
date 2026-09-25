#pragma once

#include "mdns.h"
#include <cstddef>
#include <cstdint>

unsigned setup_discovery_build_scale_options(mdns_result_t *found,
                                             char *options,
                                             size_t options_size);

bool setup_discovery_start_wifi_scan(uint32_t ui_generation);
bool setup_discovery_start_scale_scan(uint32_t ui_generation);
