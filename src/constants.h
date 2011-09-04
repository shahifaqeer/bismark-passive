#ifndef _BISMARK_PASSIVE_CONSTANTS_H_
#define _BISMARK_PASSIVE_CONSTANTS_H_

/*#define NDEBUG*/
#define PACKET_DATA_BUFFER_ENTRIES 65536
#define FLOW_TABLE_ENTRIES 65536
#define FLOW_TABLE_EXPIRATION_SECONDS (30*60)

#define UPDATE_PERIOD_SECONDS 60
#define UPDATE_FILENAME "/tmp/bismark-passive-update.gz"

#define DNS_TABLE_A_ENTRIES 1024
#define DNS_TABLE_CNAME_ENTRIES 1024

#define MAC_TABLE_ENTRIES 256

#define HT_NUM_PROBES 3
#define HT_C1 0.5
#define HT_C2 0.5

#define NUM_MICROS_PER_SECOND 1e6

#define TIMEVAL_TO_MICROS(tv) ((tv)->tv_sec * NUM_MICROS_PER_SECOND + (tv)->tv_usec)

#endif
