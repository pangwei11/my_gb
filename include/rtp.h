#pragma once
#include "globals.h"

int  rtp_socket_close(void);
int  rtp_udp_socket_create(const char *dest_ip, int dest_port, int *out_local_port);

int  send_rtp_packet(RtpContext *ctx, const uint8_t *payload, int payload_size,
                     uint32_t timestamp, int marker);

// tcp helpers
int  rtp_tcp_server_create(int *out_local_port);
void maybe_start_tcp_connect_after_ack(void);

// break / bye request
void trigger_connection_broken(void);
void check_and_handle_bye_request(void);

void* rtp_tcp_accept_thread(void *arg);
