/**
 * Read energy from an ODROID Smart Power USB device.
 *
 * @author Connor Imes
 */
#ifndef _HB_ENERGY_ODROID_SMART_POWER_H_
#define _HB_ENERGY_ODROID_SMART_POWER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "hb-energy.h"
#include <inttypes.h>

int hb_energy_init_osp(void);

double hb_energy_read_total_osp(int64_t last_hb_time, int64_t curr_hb_time);

int hb_energy_finish_osp(void);

char* hb_energy_get_source_osp(void);

hb_energy_impl* hb_energy_impl_alloc_osp(void);

#ifdef __cplusplus
}
#endif

#endif
