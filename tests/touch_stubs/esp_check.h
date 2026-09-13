#pragma once
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { int e_ = (expr); if (e_) return e_; } while (0)
