#ifndef BTCONFIG_H
#define BTCONFIG_H

extern size_t cfg_req_slice_size;

#define MAX_METAINFO_FILESIZ	4194304
// According to specs the max slice size is 128K.  But most clients do not
// accept a value that large, so we limit to 64K.  Note that there is a
// comparison in RequestQueue::IsValidRequest() (see btrequest.cpp) that
// doubles the value so that we will accept a request for 128K.
#define cfg_max_slice_size 65536
extern size_t cfg_req_queue_length;
#define MAX_PF_LEN 8
#define PEER_ID_LEN 20
#define PEER_PFX "-CD0202-"

extern size_t cfg_cache_size;

extern size_t cfg_max_peers;
extern size_t cfg_min_peers;

extern unsigned long cfg_listen_ip;
extern int cfg_listen_port;
extern int cfg_max_listen_port;
extern int cfg_min_listen_port;

extern time_t cfg_seed_hours;
extern double cfg_seed_ratio;

extern int cfg_max_bandwidth;
extern int cfg_max_bandwidth_down;
extern int cfg_max_bandwidth_up;

// arguments global value
extern char *arg_metainfo_file;
extern char *arg_bitfield_file;
extern char *arg_save_as;
extern char *arg_user_agent;

extern unsigned char arg_flg_force_seed_mode;
extern unsigned char arg_flg_check_only;
extern unsigned char arg_flg_exam_only;
extern unsigned char arg_flg_make_torrent;
extern size_t arg_file_to_download;
extern unsigned char arg_verbose;

extern size_t arg_piece_length;
extern char *arg_announce;

extern char *arg_ctcs;
extern int cfg_exit_zero_peers;
#endif
