/**
 * Shared memory implementation of heartbeat-accuracy-power.h
 *
 * @see heartbeat-util-shared.c
 * @author Hank Hoffmann
 * @author Connor Imes
 */
#include "heartbeat-accuracy-power.h"
#include "heartbeat-util-shared.h"
#include <energymon/energymon-default.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#define __STDC_FORMAT_MACROS

heartbeat_t* heartbeat_acc_pow_init(int64_t window_size,
                                    int64_t buffer_depth,
                                    const char* log_name,
                                    double min_perf,
                                    double max_perf,
                                    double min_acc,
                                    double max_acc,
                                    double min_pow,
                                    double max_pow) {
  int pid = getpid();
  char buf[64] = { '\0' };

  heartbeat_t* hb = (heartbeat_t*) malloc(sizeof(heartbeat_t));
  if (hb == NULL) {
    perror("Failed to malloc heartbeat");
    return NULL;
  }
  // set to NULL so free doesn't fail in finish function if we have to abort
  hb->window = NULL;
  hb->accuracy_window = NULL;
  hb->power_window = NULL;
  hb->text_file = NULL;

  if (energymon_get_default(&hb->em) || hb->em.finit(&hb->em)) {
    perror("Failed to initialize energymon");
    free(hb);
    return NULL;
  }
  if (hb->em.fsource(buf, sizeof(buf)) != NULL) {
    printf("Initialized energy reading from: %s\n", buf);
  }

  hb->state = HB_alloc_state(pid);
  if (hb->state == NULL) {
    heartbeat_finish(hb);
    return NULL;
  }
  hb->state->pid = pid;

  if(log_name != NULL) {
    hb->text_file = fopen(log_name, "w");
    if (hb->text_file == NULL) {
      perror("Failed to open heartbeat log file");
      heartbeat_finish(hb);
      return NULL;
    } else {
      fprintf(hb->text_file, "Beat    Tag    Timestamp    Global_Rate    Window_Rate    Instant_Rate    Global_Accuracy    Window_Accuracy    Instant_Accuracy    Global_Power    Window_Power    Instant_Power\n" );
    }
  } else {
    hb->text_file = NULL;
  }

  if(getenv("HEARTBEAT_ENABLED_DIR") == NULL) {
    heartbeat_finish(hb);
    return NULL;
  }
  sprintf(hb->filename, "%s/%d", getenv("HEARTBEAT_ENABLED_DIR"), hb->state->pid);
  printf("%s\n", hb->filename);

  hb->log = HB_alloc_log(hb->state->pid, buffer_depth);
  if(hb->log == NULL) {
    heartbeat_finish(hb);
    return NULL;
  }

  hb->first_timestamp = hb->last_timestamp = -1;
  hb->state->window_size = window_size;
  hb->window = (int64_t*) malloc(window_size*sizeof(int64_t));
  if (hb->window == NULL) {
    perror("Failed to malloc window size");
    heartbeat_finish(hb);
    return NULL;
  }
  hb->accuracy_window = (double*) malloc(window_size*sizeof(double));
  if (hb->accuracy_window == NULL) {
    perror("Failed to malloc accuracy window");
    heartbeat_finish(hb);
    return NULL;
  }
  hb->power_window = (double*) malloc(window_size*sizeof(double));
  if (hb->power_window == NULL) {
    perror("Failed to malloc power window");
    heartbeat_finish(hb);
    return NULL;
  }
  hb->current_index = 0;
  hb->state->min_heartrate = min_perf;
  hb->state->max_heartrate = max_perf;
  hb->state->min_accuracy  = min_acc;
  hb->state->max_accuracy  = max_acc;
  hb->state->min_power     = min_pow;
  hb->state->max_power     = max_pow;
  hb->state->counter = 0;
  hb->state->buffer_index = 0;
  hb->state->read_index = 0;
  hb->state->buffer_depth = buffer_depth;
  pthread_mutex_init(&hb->mutex, NULL);
  hb->steady_state = 0;
  hb->state->valid = 0;

  hb->global_accuracy = 0;
  hb->global_power = 0;
  hb->total_energy = 0;

  hb->binary_file = fopen(hb->filename, "w");
  if ( hb->binary_file == NULL ) {
    perror("Failed to open heartbeat log");
    heartbeat_finish(hb);
    return NULL;
  }
  fclose(hb->binary_file);

  return hb;
}

heartbeat_t* heartbeat_init(int64_t window_size,
                            int64_t buffer_depth,
                            const char* log_name,
                            double min_target,
                            double max_target) {
  return heartbeat_acc_pow_init(window_size, buffer_depth, log_name,
                                min_target, max_target,
                                0.0, 0.0,
                                0.0, 0.0);
}

/**
 *
 * @param hb pointer to heartbeat_t
 */
static void hb_flush_buffer(heartbeat_t volatile * hb) {
  int64_t i;
  int64_t nrecords = hb->state->buffer_index; // buffer_depth

  //printf("Flushing buffer - %lld records\n",
  //	 (long long int) nrecords);

  if(hb->text_file != NULL) {
    for(i = 0; i < nrecords; i++) {
      fprintf(hb->text_file,
              "%lld    %d    %lld    %f    %f    %f    %f    %f    %f    %f    %f    %f\n",
              (long long int) hb->log[i].beat,
              hb->log[i].tag,
              (long long int) hb->log[i].timestamp,
              hb->log[i].global_rate,
              hb->log[i].window_rate,
              hb->log[i].instant_rate,
              hb->log[i].global_accuracy,
              hb->log[i].window_accuracy,
              hb->log[i].instant_accuracy,
              hb->log[i].global_power,
              hb->log[i].window_power,
              hb->log[i].instant_power);
    }

    fflush(hb->text_file);
  }
}

void heartbeat_finish(heartbeat_t* hb) {
  if (hb != NULL) {
    pthread_mutex_destroy(&hb->mutex);
    free(hb->window);
    free(hb->accuracy_window);
    free(hb->power_window);
    if(hb->text_file != NULL) {
      hb_flush_buffer(hb);
      fclose(hb->text_file);
    }
    remove(hb->filename);
    if (hb->em.ffinish(&hb->em)) {
      perror("Failed to finish energymon");
    }
    /*TODO : need to deallocate log */
    free(hb);
  }
}

/**
 * Helper function to compute windowed heart rate and accuracy
 *
 * @param hb pointer to heartbeat_t
 * @param time int64_t
 * @param accuracy double
 * @param accuracy_rate double
 * @param energy double
 * @param power_rate double
 */
static inline float hb_window_average_accuracy(heartbeat_t volatile * hb,
    int64_t time,
    double accuracy,
    double* accuracy_rate,
    double energy,
    double* power_rate) {
  int i;
  double average_time = 0;
  double average_accuracy = 0;
  double average_power = 0;
  double fps;
  double window_time = 0;
  double window_energy = 0;


  if(!hb->steady_state) {  // not yet reached a full window of heartbeats
    hb->window[hb->current_index] = time;
    hb->accuracy_window[hb->current_index] = accuracy;
    hb->power_window[hb->current_index] = energy;

    for(i = 0; i < hb->current_index+1; i++) {
      average_time     += (double) hb->window[i];
      average_accuracy += hb->accuracy_window[i];
      average_power    += hb->power_window[i];
    }
    hb->last_window_time       = window_time = average_time;
    average_time               = average_time     / ((double) hb->current_index+1);
    average_accuracy           = average_accuracy / ((double) hb->current_index+1);
    hb->last_window_energy     = window_energy = average_power;
    average_power              = average_power    / hb->last_window_time;

    hb->last_average_time     = average_time;
    hb->last_average_accuracy = average_accuracy;
    // hb->last_average_power    = average_power;

    hb->current_index++;
    if( hb->current_index == hb->state->window_size) { // setup for full window next time
      hb->current_index = 0;
      hb->steady_state = 1;
    }
  } else { // now we have a full window
    average_time =
      hb->last_average_time -
      ((double) hb->window[hb->current_index]/ (double) hb->state->window_size);
    average_accuracy =
      hb->last_average_accuracy -
      ((double) hb->accuracy_window[hb->current_index]/ (double) hb->state->window_size);
    window_time =
      hb->last_window_time -
      ((double) hb->window[hb->current_index]);
    window_energy =
      hb->last_window_energy -
      hb->power_window[hb->current_index];



    average_time += (double) time /  (double) hb->state->window_size;
    average_accuracy += (double) accuracy /  (double) hb->state->window_size;
    window_time += (double) time;
    window_energy += energy;



    hb->last_average_time = average_time;
    hb->last_average_accuracy = average_accuracy;
    hb->last_window_time = window_time;
    hb->last_window_energy = window_energy;


    hb->window[hb->current_index] = time;
    hb->accuracy_window[hb->current_index] = accuracy;
    hb->power_window[hb->current_index] = energy;

    hb->current_index++;

    if( hb->current_index == hb->state->window_size)
      hb->current_index = 0;
  }
  fps = (1.0 / (float) average_time)*1000000000;

  *accuracy_rate = average_accuracy;

  *power_rate = window_energy/(window_time   / 	1000000000.0);

  //printf("%f  %f  %f\n", *power_rate, window_energy, window_time);

  return fps;
}

int64_t heartbeat_acc( heartbeat_t* hb, int tag, double accuracy ) {
  struct timespec time_info;
  int64_t time;
  int64_t old_last_time;
  double old_last_energy;
  double energy = 0.0;

  pthread_mutex_lock(&hb->mutex);
  //printf("Registering Heartbeat\n");
  old_last_time = hb->last_timestamp;
  old_last_energy = hb->last_energy;

  clock_gettime( CLOCK_REALTIME, &time_info );
  time = ( (int64_t) time_info.tv_sec * 1000000000 + (int64_t) time_info.tv_nsec );

  energy = hb->em.fread(&hb->em) / 1000000.0;

  hb->last_timestamp = time;
  hb->last_energy = energy;

  if(hb->first_timestamp == -1) {
    //printf("In heartbeat - first time stamp\n");
    hb->first_timestamp = time;
    hb->last_timestamp  = time;
    hb->last_energy = energy;
    hb->window[0] = 0;
    hb->accuracy_window[0] = accuracy;
    hb->power_window[0] = 0;

    //printf("             - accessing state and log\n");
    hb->log[0].beat = hb->state->counter;
    hb->log[0].tag = tag;
    hb->log[0].timestamp = time;
    hb->log[0].window_rate = 0;
    hb->log[0].instant_rate = 0;
    hb->log[0].global_rate = 0;
    hb->log[0].window_accuracy = accuracy;
    hb->log[0].instant_accuracy = accuracy;
    hb->log[0].global_accuracy = accuracy;
    hb->log[0].window_power = 0;
    hb->log[0].instant_power = 0;
    hb->log[0].global_power = 0;
    hb->state->counter++;
    hb->global_accuracy += accuracy;
    hb->total_energy = 0;
    hb->state->buffer_index++;
    hb->state->valid = 1;
  } else {
    //printf("In heartbeat - NOT first time stamp - read index = %d\n",hb->state->read_index );
    double window_accuracy;
    double window_power;
    int index =  hb->state->buffer_index;
    hb->last_timestamp = time;
    double window_heartrate = hb_window_average_accuracy(hb,
                              time-old_last_time,
                              accuracy,
                              &window_accuracy,
                              energy - old_last_energy,
                              &window_power);
    double global_heartrate =
      (((double) hb->state->counter+1) /
       ((double) (time - hb->first_timestamp)))*1000000000.0;
    double instant_heartrate = 1.0 /(((double) (time - old_last_time))) *
                               1000000000.0;

    hb->global_accuracy += accuracy;
    double global_accuracy = hb->global_accuracy
                             / (double) (hb->state->counter+1);
    double instant_accuracy = accuracy;

    hb->total_energy += energy - old_last_energy;
    double global_power = hb->total_energy /
                          ((double) (time - hb->first_timestamp))*1000000000.0;
    double instant_power = (energy - old_last_energy) /
                           (((double) (time - old_last_time))) * 	1000000000.0;


    hb->log[index].beat             = hb->state->counter;
    hb->log[index].tag              = tag;
    hb->log[index].timestamp        = time;
    hb->log[index].window_rate      = window_heartrate;
    hb->log[index].instant_rate     = instant_heartrate;
    hb->log[index].global_rate      = global_heartrate;
    hb->log[index].window_accuracy  = window_accuracy;
    hb->log[index].instant_accuracy = instant_accuracy;
    hb->log[index].global_accuracy  = global_accuracy;
    hb->log[index].window_power     = window_power;
    hb->log[index].instant_power    = instant_power;
    hb->log[index].global_power     = global_power;
    hb->state->buffer_index++;
    hb->state->counter++;
    hb->state->read_index++;

    if(hb->state->buffer_index%hb->state->buffer_depth == 0) {
      if(hb->text_file != NULL)
        hb_flush_buffer(hb);
      hb->state->buffer_index = 0;
    }
    if(hb->state->read_index%hb->state->buffer_depth == 0) {
      hb->state->read_index = 0;
    }
  }
  pthread_mutex_unlock(&hb->mutex);
  return time;
}

int64_t heartbeat(heartbeat_t* hb, int tag) {
  return heartbeat_acc(hb, tag, 0.0);
}
