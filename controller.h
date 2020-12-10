#pragma once

void display_usage();
void init(int argc, char *argv[]);
void connect_to_server(int *sockfd, char **argv);
void send_bytes(int argc, char *argv[], int *sockfd);
void recv_bytes(int *sockfd);

// void handle_error();
// void basic_validation(int argc, char **argv);
