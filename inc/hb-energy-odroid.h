/**
 * Energy reading for an ODROID with INA231 power sensors.
 *
 * @author Connor Imes
 * @date 2014-06-30
 */
#ifndef _HB_ENERGY_ODROID_H_
#define _HB_ENERGY_ODROID_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "hb-energy.h"
#include <inttypes.h>

int hb_energy_init_odroid(void);

double hb_energy_read_total_odroid(int64_t last_hb_time, int64_t curr_hb_time);

int hb_energy_finish_odroid(void);

char* hb_energy_get_source_odroid(void);

hb_energy_impl* hb_energy_impl_alloc_odroid(void);

#ifdef __cplusplus
}
#endif

#endif
