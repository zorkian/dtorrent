#ifndef RATE_H
#define RATE_H

#include "def.h"
#include <sys/types.h>
#include <time.h>

#define MAX_SAMPLES 20

class Rate{
 private:
  time_t m_last_timestamp;
  time_t m_total_timeused;
  u_int64_t m_count_bytes;
  u_int64_t m_recent_base;
  
  // bandwidth history data
  size_t n_samples;
  time_t m_timestamp_sample[MAX_SAMPLES];
  u_int64_t m_bytes_sample[MAX_SAMPLES];

 public:
  Rate(){ m_last_timestamp = m_total_timeused = (time_t)0;
    m_recent_base = m_count_bytes = 0;
    n_samples=0; for(int i=0;i<MAX_SAMPLES;i++) m_timestamp_sample[i]=0;
  }
  void Reset(){ m_last_timestamp = m_total_timeused = (time_t)0;
    m_recent_base = m_count_bytes;
    n_samples = 0; for(int i=0;i<MAX_SAMPLES;i++) m_timestamp_sample[i]=0;
  }
  void StartTimer();
  void StopTimer();
  void CountAdd(size_t nbytes);
  void operator=(const Rate &ra);
  u_int64_t Count() const { return m_count_bytes; }
  size_t RateMeasure() const;
  size_t RateMeasure(const Rate &ra) const;
  time_t TimeUsed(const time_t *pnow) const;
};

#endif
