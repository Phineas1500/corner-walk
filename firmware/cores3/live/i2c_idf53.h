// Arduino 3.1.3 used the LL spelling. IDF 5.3.4 moved this initializer into HAL.
#include "hal/i2c_hal.h"
static inline void i2c_ll_slave_init(i2c_dev_t *dev) {
    i2c_hal_context_t hal = {.dev = dev};
    i2c_hal_slave_init(&hal);
}
