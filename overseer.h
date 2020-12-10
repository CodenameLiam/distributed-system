#pragma once
#include <sys/types.h>
#include <stdbool.h>

/* format for argument options */
typedef struct options_t
{
    char *file;
    char **argv;
    char *out_file;
    char *log_file;
    int timeout;
    pid_t mem;
    int memkill;
} options_t;

/* format of a single request. */
struct request
{
    // options_t options;    /* options sent by the controller                 */
    // int arg_length;       /* number of arguments sent by the controller     */
    int newfd;            /* new socket connection file desciptor           */
    char *buffer;         /* information sent by the controller             */
    char *command;        /* controller command in simple string form       */
    struct request *next; /* pointer to next request, NULL if none.         */
};

struct memlog {
    pid_t pid;
    time_t time;
    int memory;
    char* file;
    char** args;
    int num_args;
    struct memlog *next;
};

void init(int argc, char *argv[], int *sockfd, struct sockaddr_in *host_addr);
void init_threads(int *thr_id, pthread_t *p_threads);
void handle_interrupt(int sig);
void exit_gracefully(pthread_t *p_threads);
// void extract_options(options_t *options, char **buffer, int *arg_length);
// void extract_params(char *log_file, char *out_file, int *timeout, char *buffer);
void extract_options(char *buffer, char *file, char **args, char *log_file, char *out_file, bool *mem, pid_t *mem_id, bool* memkill, double* mem_perc, int *timeout, int *arg_length);
void free_heap(int arg_index, char *file, char **args, char *out_file, char *log_file);
// void extract_params(char *log_file, int *timeout, char *buffer);
void request_handler(int sockfd);
void *thread_pool(void *data);
void handle_request(struct request *a_request, int thread_id);
void add_request(int newfd, char *buffer, char *comand, pthread_mutex_t *p_mutex, pthread_cond_t *p_cond_var);
struct request *get_request();
void message_log(char *output, char *log_file);
void date_log(char *log_file);

void timeout_handler(int sig);
void signal_handler(int sig);
void child_handler(int sig);

void log_proccess_info(pid_t pid, char* file, char** args, int num_args);
int calculate_memory_usage(pid_t pid);
void memory_log_history(pid_t pid);
void memory_log_current();
void add_process(pid_t pid);
void remove_process(pid_t pid);

void send_bytes(char* buffer, int *sockfd);
// void bind_overseer(int *sockfd, int port, struct sockaddr_in *host_addr);
// void display_usage();

// int recieve_array(char *buff, int num_items, int sockfd);
// void copy_arr(char *copy, char *paste, int start);
// void date_log();
// options *extract_options(char buff[]);