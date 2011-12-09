#include <inttypes.h>
#include <pcap.h>
/* sigaction, sigprocmask, SIG* */
#include <signal.h>
#include <stdio.h>
/* exit() */
#include <stdlib.h>
/* strdup() */
#include <string.h>
/* time() */
#include <time.h>
/* sleep() */
#include <unistd.h>
/* update compression */
#include <zlib.h>
/* inet_ntoa() */
#include <arpa/inet.h>
/* DNS message header */
#include <arpa/nameser.h>
/* ETHER_HDR_LEN */
#include <net/ethernet.h>
/* IPPROTO_... */
#include <netinet/in.h>
/* struct ip */
#include <netinet/ip.h>
/* struct tcphdr */
#include <netinet/tcp.h>
/* struct udphdr */
#include <netinet/udp.h>
/* gettimeofday */
#include <sys/time.h>

#include "address_table.h"
#ifndef DISABLE_ANONYMIZATION
#include "anonymization.h"
#endif
#ifdef ENABLE_FREQUENT_UPDATES
#include "device_throughput_table.h"
#endif
#include "dns_parser.h"
#include "dns_table.h"
#include "drop_statistics.h"
#include "flow_table.h"
#include "packet_series.h"
#include "whitelist.h"

static pcap_t* pcap_handle = NULL;

static packet_series_t packet_data;
static flow_table_t flow_table;
static dns_table_t dns_table;
static address_table_t address_table;
static domain_whitelist_t domain_whitelist;
static drop_statistics_t drop_statistics;
#ifdef ENABLE_FREQUENT_UPDATES
static device_throughput_table_t device_throughput_table;
#endif

/* Set of signals that get blocked while processing a packet. */
sigset_t block_set;

/* Will be filled in the bismark node ID, from /etc/bismark/ID. */
static char bismark_id[256];

/* Will be filled in with the current timestamp when the program starts. This
 * value serves as a unique identifier across instances of bismark-passive that
 * have run on the same machine. */
static int64_t start_timestamp_microseconds;

/* Will be incremented and sent with each update. */
static int sequence_number = 0;
#ifdef ENABLE_FREQUENT_UPDATES
static int frequent_sequence_number = 0;
#endif

static unsigned int alarm_count = 0;
#ifdef ENABLE_FREQUENT_UPDATES
#define ALARMS_PER_UPDATE (UPDATE_PERIOD_SECONDS / FREQUENT_UPDATE_PERIOD_SECONDS)
#else
#define ALARMS_PER_UPDATE 1
#endif

/* This extracts flow information from raw packet contents. */
static uint16_t get_flow_entry_for_packet(
    const u_char* const bytes,
    int cap_length,
    int full_length,
    flow_table_entry_t* const entry,
    int* const mac_id,
    u_char** const dns_bytes,
    int* const dns_bytes_len) {
  const struct ether_header* const eth_header = (struct ether_header*)bytes;
  uint16_t ether_type = ntohs(eth_header->ether_type);
#ifdef ENABLE_FREQUENT_UPDATES
  if (device_throughput_table_record(&device_throughput_table,
                                     eth_header->ether_shost,
                                     full_length)
      || device_throughput_table_record(&device_throughput_table,
                                        eth_header->ether_dhost,
                                        full_length)) {
#ifndef NDEBUG
    fprintf(stderr, "Error adding to device throughput table\n");
#endif
  }
#endif
  if (ether_type == ETHERTYPE_IP) {
    const struct iphdr* ip_header = (struct iphdr*)(bytes + ETHER_HDR_LEN);
    entry->ip_source = ntohl(ip_header->saddr);
    entry->ip_destination = ntohl(ip_header->daddr);
    entry->transport_protocol = ip_header->protocol;
    address_table_lookup(
        &address_table, entry->ip_source, eth_header->ether_shost);
    address_table_lookup(
        &address_table, entry->ip_destination, eth_header->ether_dhost);
    if (ip_header->protocol == IPPROTO_TCP) {
      const struct tcphdr* tcp_header = (struct tcphdr*)(
          (void *)ip_header + ip_header->ihl * sizeof(uint32_t));
      entry->port_source = ntohs(tcp_header->source);
      entry->port_destination = ntohs(tcp_header->dest);
    } else if (ip_header->protocol == IPPROTO_UDP) {
      const struct udphdr* udp_header = (struct udphdr*)(
          (void *)ip_header + ip_header->ihl * sizeof(uint32_t));
      entry->port_source = ntohs(udp_header->source);
      entry->port_destination = ntohs(udp_header->dest);

      if (entry->port_source == NS_DEFAULTPORT) {
        *dns_bytes = (u_char*)udp_header + sizeof(struct udphdr);
        *dns_bytes_len = cap_length - (*dns_bytes - bytes);
        *mac_id = address_table_lookup(
            &address_table, entry->ip_destination, eth_header->ether_dhost);
      }
    } else {
#ifndef NDEBUG
      fprintf(stderr, "Unhandled transport protocol: %u\n", ip_header->protocol);
#endif
    }
  } else {
#ifndef NDEBUG
    fprintf(stderr, "Unhandled network protocol: %hu\n", ether_type);
#endif
  }
  return ether_type;
}

/* libpcap calls this function for every packet it receives. */
static void process_packet(
        u_char* const user,
        const struct pcap_pkthdr* const header,
        const u_char* const bytes) {
  if (sigprocmask(SIG_BLOCK, &block_set, NULL) < 0) {
#ifndef NDEBUG
    perror("sigprocmask");
#endif
    exit(1);
  }

#ifndef NDEBUG
  static int packets_received = 0;
  ++packets_received;
  if (packets_received % 1000 == 0) {
    struct pcap_stat statistics;
    pcap_stats(pcap_handle, &statistics);
    printf("-----\n");
    printf("STATISTICS (printed once for every thousand packets)\n");
    printf("Libpcap has dropped %d packets since process creation\n", statistics.ps_drop);
    printf("There are %d entries in the flow table\n", flow_table.num_elements);
    printf("The flow table has dropped %d flows\n", flow_table.num_dropped_flows);
    printf("The flow table has expired %d flows\n", flow_table.num_expired_flows);
    printf("-----\n");
  }
  if (packet_data.discarded_by_overflow % 1000 == 1) {
    printf("%d packets have overflowed the packet table!\n", packet_data.discarded_by_overflow);
  }
#endif

  flow_table_entry_t flow_entry;
  flow_table_entry_init(&flow_entry);
  int mac_id = -1;
  u_char* dns_bytes;
  int dns_bytes_len = -1;
  int ether_type = get_flow_entry_for_packet(
      bytes, header->caplen, header->len, &flow_entry, &mac_id, &dns_bytes, &dns_bytes_len);
  uint16_t flow_id;
  switch (ether_type) {
    case ETHERTYPE_AARP:
      flow_id = FLOW_ID_AARP;
      break;
    case ETHERTYPE_ARP:
      flow_id = FLOW_ID_ARP;
      break;
    case ETHERTYPE_AT:
      flow_id = FLOW_ID_AT;
      break;
    case ETHERTYPE_IP:
      {
        flow_id = flow_table_process_flow(&flow_table,
                                          &flow_entry,
                                          header->ts.tv_sec);
#ifndef NDEBUG
        if (flow_id == FLOW_ID_ERROR) {
          fprintf(stderr, "Error adding to flow table\n");
        }
#endif
      }
      break;
    case ETHERTYPE_IPV6:
      flow_id = FLOW_ID_IPV6;
      break;
    case ETHERTYPE_IPX:
      flow_id = FLOW_ID_IPX;
      break;
    case ETHERTYPE_REVARP:
      flow_id = FLOW_ID_REVARP;
      break;
    default:
      flow_id = FLOW_ID_ERROR;
      break;
  }

  int packet_id = packet_series_add_packet(
        &packet_data, &header->ts, header->len, flow_id);
  if (packet_id < 0) {
#ifndef NDEBUG
    fprintf(stderr, "Error adding to packet series\n");
#endif
    drop_statistics_process_packet(&drop_statistics, header->len);
  }

  if (dns_bytes_len > 0 && mac_id >= 0) {
    process_dns_packet(dns_bytes, dns_bytes_len, &dns_table, packet_id, mac_id);
  }

  if (sigprocmask(SIG_UNBLOCK, &block_set, NULL) < 0) {
#ifndef NDEBUG
    perror("sigprocmask");
#endif
    exit(1);
  }
}

/* Write an update to UPDATE_FILENAME. This is the file that will be sent to the
 * server. The data is compressed on-the-fly using gzip. */
static void write_update() {
  struct pcap_stat statistics;
  int have_pcap_statistics;
  if (pcap_handle) {
    have_pcap_statistics = !pcap_stats(pcap_handle, &statistics);
#ifndef NDEBUG
    if (!have_pcap_statistics) {
      pcap_perror(pcap_handle, "Error fetching pcap statistics");
    }
#endif
  } else {
    have_pcap_statistics = 0;
  }

#ifndef DISABLE_FLOW_THRESHOLDING
  if (flow_table_write_thresholded_ips(&flow_table,
                                       start_timestamp_microseconds,
                                       sequence_number)) {
#ifndef NDEBUG
    fprintf(stderr, "Couldn't write thresholded flows log\n");
#endif
  }
#endif

#ifndef NDEBUG
  printf("Writing differential log to %s\n", PENDING_UPDATE_FILENAME);
#endif
  gzFile handle = gzopen (PENDING_UPDATE_FILENAME, "wb");
  if (!handle) {
#ifndef NDEBUG
    perror("Could not open update file for writing");
#endif
    exit(1);
  }

  dns_table_mark_unanonymized(&dns_table, &flow_table);

  time_t current_timestamp = time(NULL);

  if (!gzprintf(handle,
                "%d\n%s\n",
                FILE_FORMAT_VERSION,
                BUILD_ID)) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
  if (!gzprintf(handle,
                "%s %" PRId64 " %d %" PRId64 "\n",
                bismark_id,
                start_timestamp_microseconds,
                sequence_number,
                (int64_t)current_timestamp)) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
  if (have_pcap_statistics) {
    if (!gzprintf(handle,
                  "%u %u %u\n",
                  statistics.ps_recv,
                  statistics.ps_drop,
                  statistics.ps_ifdrop)) {
#ifndef NDEBUG
      perror("Error writing update");
#endif
      exit(1);
    }
  }
  if (!gzprintf(handle, "\n")) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
  if (sequence_number == 0) {
    if (domain_whitelist_write_update(&domain_whitelist, handle)) {
      exit(1);
    }
  } else {
    if (!gzprintf(handle, "\n")) {
#ifndef NDEBUG
      perror("Error writing update");
#endif
      exit(1);
    }
  }
#ifndef DISABLE_ANONYMIZATION
  if (anonymization_write_update(handle)) {
    exit(1);
  }
#else
  if (!gzprintf(handle, "UNANONYMIZED\n\n")) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
#endif
  if (packet_series_write_update(&packet_data, handle)
      || flow_table_write_update(&flow_table, handle)
      || dns_table_write_update(&dns_table, handle)
      || address_table_write_update(&address_table, handle)
      || drop_statistics_write_update(&drop_statistics, handle)) {
    exit(1);
  }
  gzclose(handle);

  char update_filename[FILENAME_MAX];
  snprintf(update_filename,
           FILENAME_MAX,
           UPDATE_FILENAME,
           bismark_id,
           start_timestamp_microseconds,
           sequence_number);
  if (rename(PENDING_UPDATE_FILENAME, update_filename)) {
#ifndef NDEBUG
    perror("Could not stage update");
#endif
    exit(1);
  }

  ++sequence_number;

  packet_series_init(&packet_data);
  flow_table_advance_base_timestamp(&flow_table, current_timestamp);
  dns_table_destroy(&dns_table);
  dns_table_init(&dns_table, &domain_whitelist);
  drop_statistics_init(&drop_statistics);
}

#ifdef ENABLE_FREQUENT_UPDATES
static void write_frequent_update() {
  FILE* handle = fopen (PENDING_FREQUENT_UPDATE_FILENAME, "w");
  if (!handle) {
#ifndef NDEBUG
    perror("Could not open update file for writing");
#endif
    exit(1);
  }
  if (!fprintf(handle,
               "%d\n",
               FREQUENT_FILE_FORMAT_VERSION) < 0) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
  time_t current_timestamp = time(NULL);
  if (!fprintf(handle,
               "%s %" PRId64 "\n\n",
               BUILD_ID,
               (int64_t)current_timestamp) < 0) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
#ifndef DISABLE_ANONYMIZATION
  if (anonymization_write_update(handle)) {
    exit(1);
  }
#else
  if (!gzprintf(handle, "UNANONYMIZED\n\n")) {
#ifndef NDEBUG
    perror("Error writing update");
#endif
    exit(1);
  }
#endif
  if (device_throughput_table_write_update(&device_throughput_table, handle)) {
    exit(1);
  }
  fclose(handle);

  char update_filename[FILENAME_MAX];
  snprintf(update_filename,
           FILENAME_MAX,
           FREQUENT_UPDATE_FILENAME,
           bismark_id,
           start_timestamp_microseconds,
           frequent_sequence_number);
  if (rename(PENDING_FREQUENT_UPDATE_FILENAME, update_filename)) {
#ifndef NDEBUG
    perror("Could not stage update");
#endif
    exit(1);
  }

  ++frequent_sequence_number;

  device_throughput_table_init(&device_throughput_table);
}
#endif

static void set_next_alarm() {
#ifdef ENABLE_FREQUENT_UPDATES
  alarm(FREQUENT_UPDATE_PERIOD_SECONDS);
#else
  alarm(UPDATE_PERIOD_SECONDS);
#endif
}

static void handle_signals(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    write_update();
#ifdef ENABLE_FREQUENT_UPDATES
    write_frequent_update();
#endif
    exit(0);
  } else if (sig == SIGALRM) {
    alarm_count += 1;
    if (alarm_count % ALARMS_PER_UPDATE == 0) {
      write_update();
    }
#ifdef ENABLE_FREQUENT_UPDATES
    write_frequent_update();
#endif
    set_next_alarm();
  }
}

static int initialize_signal_handler() {
  struct sigaction action;
  action.sa_handler = handle_signals;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (sigaction(SIGINT, &action, NULL) < 0
      || sigaction(SIGTERM, &action, NULL) < 0
      || sigaction(SIGALRM, &action, NULL)) {
    perror("sigaction");
    return -1;
  }
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGINT);
  sigaddset(&block_set, SIGTERM);
  sigaddset(&block_set, SIGALRM);
  return 0;
}

static pcap_t* initialize_pcap(const char* const interface) {
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t* const handle = pcap_open_live(interface, BUFSIZ, 0, 1000, errbuf);
  if (!handle) {
    fprintf(stderr, "Couldn't open device %s: %s\n", interface, errbuf);
    return NULL;
  }

  if (pcap_datalink(handle) != DLT_EN10MB) {
    fprintf(stderr, "Must capture on an Ethernet link\n");
    return NULL;
  }
  return handle;
}

static int initialize_bismark_id() {
  FILE* handle = fopen(BISMARK_ID_FILENAME, "r");
  if (!handle) {
    perror("Cannot open Bismark ID file " BISMARK_ID_FILENAME);
    return -1;
  }
  if(fscanf(handle, "%255s\n", bismark_id) < 1) {
    perror("Cannot read Bismark ID file " BISMARK_ID_FILENAME);
    return -1;
  }
  fclose(handle);
  return 0;
}

static int initialize_domain_whitelist(const char* const filename) {
  domain_whitelist_init(&domain_whitelist);

  FILE* handle = fopen(filename, "r");
  if (!handle) {
    perror("Cannot open domain whitelist");
    return -1;
  }

  int length;
  if (fseek(handle, 0, SEEK_END) == -1
      || ((length = ftell(handle)) == -1)
      || fseek(handle, 0, SEEK_SET) == -1) {
    perror("Cannot read domain whitelist");
    fclose(handle);
    return -1;
  }

  char* contents = malloc(length);
  if (!contents) {
    perror("Cannot allocate whitelist buffer");
    fclose(handle);
    return -1;
  }
  if (fread(contents, length, 1, handle) != 1) {
    perror("Cannot read domain whitelist");
    free(contents);
    fclose(handle);
    return -1;
  }

  fclose(handle);

  if (domain_whitelist_load(&domain_whitelist, contents) < 0) {
    fprintf(stderr, "Error reading domain whitelist.\n");
    free(contents);
    return -1;
  }
  free(contents);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <interface> [whitelist]\n", argv[0]);
    return 1;
  }

  struct timeval start_timeval;
  gettimeofday(&start_timeval, NULL);
  start_timestamp_microseconds
      = start_timeval.tv_sec * NUM_MICROS_PER_SECOND + start_timeval.tv_usec;

  if (initialize_bismark_id()) {
    return 1;
  }

  if (argc < 3 || initialize_domain_whitelist(argv[2])) {
    fprintf(stderr, "Error loading domain whitelist; whitelisting disabled.\n");
  }

#ifndef DISABLE_ANONYMIZATION
  if (anonymization_init()) {
    fprintf(stderr, "Error initializing anonymizer\n");
    return 1;
  }
#endif
  packet_series_init(&packet_data);
  flow_table_init(&flow_table);
  dns_table_init(&dns_table, &domain_whitelist);
  address_table_init(&address_table);
  drop_statistics_init(&drop_statistics);
#ifdef ENABLE_FREQUENT_UPDATES
  device_throughput_table_init(&device_throughput_table);
#endif

  if (initialize_signal_handler()) {
    return 1;
  }
  set_next_alarm();

  /* By default, pcap uses an internal buffer of 500 KB. Any packets that
   * overflow this buffer will be dropped. pcap_stats tells the number of
   * dropped packets.
   *
   * Because pcap does its own buffering, we don't need to run packet
   * processing in a separate thread. (It would be easier to just increase
   * the buffer size if we experience performance problems.) */
  pcap_handle = initialize_pcap(argv[1]);
  if (!pcap_handle) {
    return 1;
  }
  return pcap_loop(pcap_handle, -1, process_packet, NULL);
}
