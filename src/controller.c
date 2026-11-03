/*
 * Controller driver registry.
 */

#include "../include/controller.h"
#include "../include/controllers/switch_pro.h"
#include "../include/controllers/xbox360.h"
#include "../include/controllers/ds4.h"

#include <stddef.h>

/* --- Driver Registry --- */

#define MAX_DRIVERS 16

static const ControllerDriver *driver_registry[MAX_DRIVERS];
static int driver_count = 0;

void controller_driver_register(const ControllerDriver *drv)
{
    if (driver_count < MAX_DRIVERS)
        driver_registry[driver_count++] = drv;
}

const ControllerDriver *controller_driver_find(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < driver_count; i++) {
        if (driver_registry[i]->vendor_id == vid &&
            driver_registry[i]->product_id == pid)
            return driver_registry[i];
    }
    return NULL;
}

const ControllerDriver *controller_driver_find_by_type(ControllerType type)
{
    for (int i = 0; i < driver_count; i++) {
        if (driver_registry[i]->type == type)
            return driver_registry[i];
    }
    return NULL;
}

void controller_drivers_init(void)
{
    driver_count = 0;
    controller_driver_register(switch_pro_get_driver());
    controller_driver_register(xbox360_get_driver());
    controller_driver_register(ds4_get_driver(0));
    controller_driver_register(ds4_get_driver(1));
}
