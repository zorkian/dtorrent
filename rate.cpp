#include "rate.h"

#define RATE_INTERVAL 20

void Rate::StartTimer()
{
  if( !m_last_timestamp ) time(&m_last_timestamp);
}

void Rate::StopTimer()
{
  if( m_last_timestamp ){
    m_total_timeused += (time((time_t*) 0) - m_last_timestamp);
    m_last_timestamp = 0;
  }
}

void Rate::CountAdd(size_t nbytes)
{
  time_t now = time((time_t*) 0);

  m_count_bytes += nbytes;

  // save bandwidth history data
  for (int i=0; i <= n_samples; i++)
  {
    if (i < MAX_SAMPLES)
    {
      if (now == m_timestamp_sample[i]) {
        m_bytes_sample[i] += nbytes;
        break;
      }
      else if (now - RATE_INTERVAL > m_timestamp_sample[i]) {
        m_timestamp_sample[i] = now;
        m_bytes_sample[i] = nbytes;
        if (n_samples < MAX_SAMPLES) n_samples++;
        break;
      }
    }
  }
}

void Rate::operator=(const Rate &ra)
{
  m_last_timestamp = time((time_t*) 0);
  m_count_bytes = ra.m_count_bytes;
}

size_t Rate::RateMeasure() const
{
  // calculate rate based on bandwidth history data
  time_t timestamp = time((time_t*) 0);
  u_int64_t countbytes = 0;
  time_t timeused = 0;

  if( !m_last_timestamp ) return 0; // no current rate

  timeused = (TimeUsed(&timestamp) < RATE_INTERVAL) ?
    TimeUsed(&timestamp) : RATE_INTERVAL;
  if( timeused < 1 ) timeused = 1;

  for (int i=0; i<n_samples; i++)
  {
    if (timestamp - m_timestamp_sample[i] <= timeused)
      countbytes += m_bytes_sample[i];
  }
  return (size_t)(countbytes / timeused);
}

size_t Rate::RateMeasure(const Rate &ra_to) const
{
  int tmp;
  time_t timeused = time((time_t*) 0) - m_last_timestamp;
  if( timeused < 1 ) timeused = 1;
  tmp = (ra_to.m_count_bytes - ra_to.m_recent_base)
      - (m_count_bytes - m_recent_base);
  return (size_t)( (tmp>0) ? (tmp/timeused) : 0 );
}

time_t Rate::TimeUsed(const time_t *pnow) const
{
  return (*pnow - m_last_timestamp);
}
