#ifndef RATE_H
#define RATE_H

#include "def.h"
#include <inttypes.h>
#include <sys/types.h>
#include <time.h>

#include "bttypes.h"

typedef struct _bwsample{
  double timestamp;
  bt_length_t bytes;
  struct _bwsample *next;
}BWSAMPLE;

class Rate{
 private:
  time_t m_last_timestamp;
  time_t m_total_timeused;
  dt_datalen_t m_count_bytes;
  // m_last:    tracks recent xfer(s) for timing & limit comparison
  // m_recent:  the most recent measurable xfer
  // m_prev:    the prior m_recent
  double m_last_realtime, m_recent_realtime, m_prev_realtime;
  bt_length_t m_last_size, m_recent_size, m_prev_size;
  double m_late;
  dt_rate_t m_nominal;
  time_t m_nom_time;
  struct{
    dt_rate_t value;
    time_t lasttime;
    double recent;
  } m_lastrate;

  unsigned char m_ontime:1;
  unsigned char m_update_nominal:1;
  unsigned char m_reserved:6;

  BWSAMPLE *m_history, *m_history_last;  // bandwidth history data

  Rate *m_selfrate;

  static BWSAMPLE *NewSample();

 public:
  Rate();

  void Reset();
  void StartTimer();
  void StopTimer();
  void ClearHistory();
  void Cleanup();
  void CountAdd(bt_length_t nbytes);
  void UnCount(bt_length_t nbytes);
  void RateAdd(bt_length_t nbytes, dt_rate_t bwlimit);
  void RateAdd(bt_length_t nbytes, dt_rate_t bwlimit, double timestamp);
  void operator=(const Rate &ra);
  dt_datalen_t Count() const { return m_count_bytes; }
  dt_rate_t CurrentRate();
  dt_rate_t NominalRate();
  dt_rate_t RateMeasure();
  dt_rate_t RateMeasure(const Rate &ra);
  time_t TimeUsed();
  double LastRealtime() const { return m_last_realtime; }
  bt_length_t LastSize() const { return m_last_size; }
  void SetSelf(Rate *rate) { m_selfrate = rate; }
  double Late() const { return m_late; }
  int Ontime() const { return m_ontime ? 1 : 0; }
  void Ontime(int yn) { m_ontime = yn ? 1 : 0; }
};

#endif
