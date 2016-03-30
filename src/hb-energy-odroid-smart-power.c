/**
 * Read energy from an ODROID Smart Power USB device.
 * Uses the HID API.
 * The default implementation just fetches an energy reading when requested.
 * To enable polling of power readings instead, set:
 *   HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
 *
 * @author Connor Imes
 * @date 2015-01-27
 */

#include "hb-energy.h"
#include "hb-energy-odroid-smart-power.h"
#include <hidapi/hidapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>

#define OSP_MAX_STR 65
#define OSP_VENDOR_ID           0x04d8
#define OSP_PRODUCT_ID          0x003f
#define OSP_REQUEST_DATA        0x37
#define OSP_REQUEST_STARTSTOP   0x80
#define OSP_REQUEST_STATUS      0x81

// how long to sleep for (in microseconds) after certain operations
#ifndef HB_ENERGY_ODROID_SMART_POWER_SLEEP_TIME_US
  #define HB_ENERGY_ODROID_SMART_POWER_SLEEP_TIME_US 200000
#endif

/* Shared variables */
static hid_device* device = NULL;
static unsigned char buf[OSP_MAX_STR];

#ifdef HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
// sensor polling interval in microseconds
#ifndef HB_ENERGY_ODROID_SMART_POWER_POLL_DELAY_US
  // default value determined experimentally
  #define HB_ENERGY_ODROID_SMART_POWER_POLL_DELAY_US 200000
#endif

static int64_t osp_start_time;
static double osp_total_energy;

// thread variables
static double osp_hb_pwr_avg;
static double osp_hb_pwr_avg_last;
static int osp_hb_pwr_avg_count;
static pthread_mutex_t osp_polling_mutex;
static pthread_t osp_polling_thread;
static int osp_do_polling;
#else
#define JOULES_PER_WATTHOUR     3600
#endif

#ifdef HB_ENERGY_IMPL
int hb_energy_init(void) {
  return hb_energy_init_osp();
}

double hb_energy_read_total(int64_t last_hb_time, int64_t curr_hb_time) {
  return hb_energy_read_total_osp(last_hb_time, curr_hb_time);
}

int hb_energy_finish(void) {
  return hb_energy_finish_osp();
}

char* hb_energy_get_source(void) {
  return hb_energy_get_source_osp();
}

hb_energy_impl* hb_energy_impl_alloc(void) {
  return hb_energy_impl_alloc_osp();
}
#endif

static inline int hb_energy_osp_request_status() {
  buf[0] = 0x00;
  memset((void*) &buf[2], 0x00, sizeof(buf) - 2);
  buf[1] = OSP_REQUEST_STATUS;
  if (hid_write(device, buf, sizeof(buf)) == -1 ||
      hid_read(device, buf, sizeof(buf)) == -1) {
    fprintf(stderr,
            "hb_energy_osp_request_status: Failed to request/read status\n");
    return -1;
  }
  return 0;
}

static inline int hb_energy_osp_request_start_stop(int started) {
  if(started == 0) {
    buf[1] = OSP_REQUEST_STARTSTOP;
    if (hid_write(device, buf, sizeof(buf)) == -1) {
      fprintf(stderr, "hb_energy_osp_request_start_stop: Request failed\n");
      return -1;
    }
  }

  buf[1] = OSP_REQUEST_STARTSTOP;
  if (hid_write(device, buf, sizeof(buf)) == -1) {
    fprintf(stderr, "hb_energy_osp_request_start_stop: Request failed\n");
    return -1;
  }

  // let meter reset
  usleep(HB_ENERGY_ODROID_SMART_POWER_SLEEP_TIME_US);
  return 0;
}

static inline int hb_energy_osp_request_data() {
  buf[0] = 0x00;
  memset((void*) &buf[2], 0x00, sizeof(buf) - 2);
  buf[1] = OSP_REQUEST_DATA;
  if (hid_write(device, buf, sizeof(buf)) == -1 ||
      hid_read(device, buf, sizeof(buf)) == -1) {
    fprintf(stderr, "Failed to request data from ODROID Smart Power\n");
    return -1;
  }
  return 0;
}

#ifdef HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
/**
 * pthread function to poll the device at regular intervals and
 * keep a running average of power between heartbeats.
 */
static void* osp_poll_device(void* args) {
  double watts;
  while(osp_do_polling > 0 && device != NULL) {
    char w[8] = {'\0'};
    if(hb_energy_osp_request_data()) {
      fprintf(stderr, "osp_poll_device: Data request failed\n");
    } else if(buf[0] == OSP_REQUEST_DATA) {
      strncpy(w, (char*) &buf[17], 6);
      watts = atof(w);
      // keep running average between heartbeats
      pthread_mutex_lock(&osp_polling_mutex);
      osp_hb_pwr_avg = (watts + osp_hb_pwr_avg_count * osp_hb_pwr_avg) / (osp_hb_pwr_avg_count + 1);
      // printf("osp_poll_device: %f %f\n", watts, osp_hb_pwr_avg);
      osp_hb_pwr_avg_count++;
      pthread_mutex_unlock(&osp_polling_mutex);
    } else {
      fprintf(stderr, "osp_poll_device: Did not get data\n");
    }
    // sleep for the polling delay
    usleep(HB_ENERGY_ODROID_SMART_POWER_POLL_DELAY_US);
  }
  return (void*) NULL;
}
#endif

int hb_energy_init_osp(void) {
  int started;
  buf[0] = 0x00;
  memset((void*) &buf[2], 0x00, sizeof(buf) - 2);

  // initialize
  if(hid_init()) {
    fprintf(stderr, "Failed to initialize ODROID Smart Power\n");
    return -1;
  }

  // get the device
  device = hid_open(OSP_VENDOR_ID, OSP_PRODUCT_ID, NULL);
  if (device == NULL) {
    fprintf(stderr, "Failed to open ODROID Smart Power\n");
    return -1;
  }

  // set nonblocking
  hid_set_nonblocking(device, 1);

  // get the status
  if(hb_energy_osp_request_status()) {
    hb_energy_finish_osp();
    return -1;
  }

  // TODO: This doesn't seem to be accurate
  started = (buf[1] == 0x01) ? 1 : 0;
  if (hb_energy_osp_request_start_stop(started)) {
    hb_energy_finish_osp();
    return -1;
  }

  // do an initial couple of reads
  if (hb_energy_osp_request_data() || hb_energy_osp_request_data()) {
    fprintf(stderr, "Failed initial write/read of ODROID Smart Power\n");
    hb_energy_finish_osp();
    return -1;
  }

#ifdef HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
  // track start time
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  osp_start_time = to_nanosec(&now);

  // start device polling thread
  osp_total_energy = 0;
  osp_do_polling = 1;
  osp_hb_pwr_avg = 0;
  osp_hb_pwr_avg_last = 0;
  osp_hb_pwr_avg_count = 0;
  if(pthread_mutex_init(&osp_polling_mutex, NULL)) {
    fprintf(stderr, "Failed to create ODROID Smart Power mutex.\n");
    hb_energy_finish_osp();
    return -1;
  }
  if (pthread_create(&osp_polling_thread, NULL, osp_poll_device, NULL)) {
    fprintf(stderr, "Failed to start ODROID Smart Power thread.\n");
    hb_energy_finish_osp();
    return -1;
  }
#endif

  return 0;
}

double hb_energy_read_total_osp(int64_t last_hb_time, int64_t curr_hb_time) {
  double joules = 0;

  if (device == NULL) {
    fprintf(stderr, "hb_energy_read_total_osp: Not initialized!\n");
    return -1;
  }

#ifdef HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
  last_hb_time = last_hb_time < 0 ? osp_start_time : last_hb_time;
  // it's also assumed that curr_hb_time >= last_hb_time

  pthread_mutex_lock(&osp_polling_mutex);
  // convert from power to energy using timestamps
  osp_hb_pwr_avg_last = osp_hb_pwr_avg > 0 ? osp_hb_pwr_avg : osp_hb_pwr_avg_last;
  osp_total_energy += osp_hb_pwr_avg_last * diff_sec(last_hb_time, curr_hb_time);
  // printf("foo: %f %f\n", osp_total_energy, diff_sec(last_hb_time, curr_hb_time));
  joules = osp_total_energy;
  // reset running power average
  osp_hb_pwr_avg = 0;
  osp_hb_pwr_avg_count = 0;
  pthread_mutex_unlock(&osp_polling_mutex);
#else
  char wh[7] = {'\0'};

  if(hb_energy_osp_request_data()) {
    fprintf(stderr, "hb_energy_read_total_osp: Data request failed\n");
  } else if(buf[0] == OSP_REQUEST_DATA) {
		strncpy(wh, (char*) &buf[26], 5);
    joules = atof(wh) * JOULES_PER_WATTHOUR;
    // printf("hb_energy_read_total_osp: %s Watt-Hours = %f Joules\n", wh, joules);
  } else {
    fprintf(stderr, "hb_energy_read_total_osp: Did not get data\n");
  }
#endif

  return joules;
}

int hb_energy_finish_osp(void) {
  if (device == NULL) {
    // nothing to do
    return 0;
  }

#ifdef HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
  // stop sensors polling thread and cleanup
  osp_do_polling = 0;
  if(pthread_join(osp_polling_thread, NULL)) {
    fprintf(stderr, "Error joining ODROID Smart Power polling thread.\n");
  }
  if(pthread_mutex_destroy(&osp_polling_mutex)) {
    fprintf(stderr,"Error destroying pthread mutex for ODROID Smart Power polling thread.\n");
  }
#endif

  buf[0] = 0x00;
  memset((void*) &buf[2], 0x00, sizeof(buf) - 2);
#ifdef HB_ENERGY_ODROID_SMART_POWER_STOP_ON_FINISH
  buf[1] = OSP_REQUEST_STARTSTOP;
  if (hid_write(device, buf, sizeof(buf)) == -1) {
    fprintf(stderr, "hb_energy_finish_osp: Failed to request start/stop\n");
  }
  usleep(HB_ENERGY_ODROID_SMART_POWER_SLEEP_TIME_US);
#endif
  hid_close(device);
  device = NULL;
  if(hid_exit()) {
    fprintf(stderr, "hb_energy_finish_osp: Failed to exit\n");
  }
  return 0;
}

char* hb_energy_get_source_osp(void) {
#ifdef HB_ENERGY_ODROID_SMART_POWER_USE_POLLING
  return "ODROID Smart Power with Polling";
#else
  return "ODROID Smart Power";
#endif
}

hb_energy_impl* hb_energy_impl_alloc_osp(void) {
  hb_energy_impl* hei = (hb_energy_impl*) malloc(sizeof(hb_energy_impl));
  hei->finit = &hb_energy_init_osp;
  hei->fread = &hb_energy_read_total_osp;
  hei->ffinish = &hb_energy_finish_osp;
  hei->fsource = &hb_energy_get_source_osp;
  return hei;
}
