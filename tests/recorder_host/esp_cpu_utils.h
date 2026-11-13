#include "platform.h"
static inline uint32_t esp_cpu_process_stack_pc(uint32_t pc) { if (pc & 0x80000000) pc=(pc & 0x3fffffff)|0x40000000; return pc-3; }
