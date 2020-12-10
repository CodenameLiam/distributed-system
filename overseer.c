#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include "overseer.h"

/* how many pending connections queue will hold */
#define BACKLOG 10

/* length of our communication buffer */
#define MAXBYTES 1024

/* maxmimum number of arguments we can receive from a controller */
#define MAXARGS 10

/* number of threads used to service requests */
#define NUM_HANDLER_THREADS 5

/* behaviour for when logging is not required */
#define NO_LOG ""

#define MAX_PIDS 100

/* global mutex for requests. */
pthread_mutex_t request_mutex;

/* global mutex for quitting requests. */
pthread_mutex_t quit_mutex;

/* global mutex for memory log linked list access. */
pthread_mutex_t memory_log_mutex;

pthread_mutex_t pid_array_mutex;

/* global condition variable for our program. */
pthread_cond_t got_request;

/* number of pending requests, initially none */
int num_requests = 0;

volatile sig_atomic_t request_interrupt = 0;
int running = 1;

int num_processes = 0;
int pid_array[MAX_PIDS];

struct sysinfo system_info;

// int request_interrupt = 0;

struct request *requests = NULL;     /* head of linked list of requests. */
struct request *last_request = NULL; /* pointer to last request.         */

struct memlog *memory_log = NULL; /* head of linked list of memory logs */

int sockfd; /* listen on sockfd */

/***********************************************************************
 * func:            Overseer Entry Point 
 ***********************************************************************/
int main(int argc, char *argv[])
{

    int pid = getpid();
    printf("PID is %d   \n", pid);

    if (sysinfo(&system_info) == -1)
    {
        fprintf(stderr, "Could not retrieve system information, aborting! \n");
        exit(EXIT_FAILURE);
    }

    struct sigaction a;
    a.sa_handler = handle_interrupt;
    a.sa_flags = 0;
    sigemptyset(&a.sa_mask);
    sigaction(SIGINT, &a, NULL);

    struct sockaddr_in host_addr; /* my address information */

    int thr_id[NUM_HANDLER_THREADS];          /* thread IDs            */
    pthread_t p_threads[NUM_HANDLER_THREADS]; /* thread's structures   */

    /* initialise the overseer */
    init(argc, argv, &sockfd, &host_addr);

    /* initialise threadpool */
    init_threads(thr_id, p_threads);

    /* handle incoming requests */
    request_handler(sockfd);

    /* exit program gracefully */
    exit_gracefully(p_threads);

    exit(EXIT_SUCCESS);
}

/***********************************************************************
 * func:            Initialises the application and looks for errors
 * param argc:      The number of arguments given to the controller
 * param argv:      Command line arguments provided to the controller
 * param sockfd:    Pointer to the overseer file descriptor
 * param host_addr: Pointer to the socket address
 ***********************************************************************/
void init(int argc, char *argv[], int *sockfd, struct sockaddr_in *host_addr)
{
    /* required arguments not met */
    if (argc < 2)
    {
        fprintf(stderr, "Usage: overseer <port>\n");
        exit(EXIT_FAILURE);
    }

    /* get the port number */
    int port = strtol(argv[1], NULL, 10);
    if (!(port > 0 && port < 65535))
    {
        fprintf(stderr, "Please enter a valid port number\n");
        exit(EXIT_FAILURE);
    }

    /* generate the socket */
    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Enable address/port reuse, useful for server development */
    int opt_enable = 1;
    setsockopt(*sockfd, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable));
    setsockopt(*sockfd, SOL_SOCKET, SO_REUSEPORT, &opt_enable, sizeof(opt_enable));

    /* clear address struct */
    memset(host_addr, 0, sizeof(*host_addr));

    /* generate the end point */
    host_addr->sin_family = AF_INET;         /* host byte order */
    host_addr->sin_port = htons(port);       /* short, network byte order */
    host_addr->sin_addr.s_addr = INADDR_ANY; /* auto-fill with my IP */

    /* bind the socket to the end point */
    if (bind(*sockfd, (struct sockaddr *)host_addr, sizeof(struct sockaddr)) == -1)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    /* start listnening */
    if (listen(*sockfd, BACKLOG) == -1)
    {
        perror("listen");
        exit(1);
    }

    /* print listnening message */
    printf("Overseer listening on port %d\n", port);
}

/***********************************************************************
 * func:            Initialise the thread pool
 ***********************************************************************/
void init_threads(int *thr_id, pthread_t *p_threads)
{
    /* initialise mutex locks */
    pthread_mutex_init(&request_mutex, NULL);
    pthread_mutex_init(&quit_mutex, NULL);
    pthread_mutex_init(&memory_log_mutex, NULL);
    pthread_mutex_init(&pid_array_mutex, NULL);
    pthread_cond_init(&got_request, NULL);

    /* create the request-handling threads */
    for (int i = 0; i < NUM_HANDLER_THREADS; i++)
    {
        thr_id[i] = i;
        pthread_create(&p_threads[i], NULL, thread_pool, (void *)&thr_id[i]);
    }
}

/***********************************************************************
 * func:            Thread pool to handle incoming requests
 * param data:      ID of thread, for printing purposes.
 ***********************************************************************/
void *thread_pool(void *data)
{
    int thread_id = *((int *)data); /* thread identifying number           */
    struct request *a_request;      /* pointer to a request.               */

    /* lock the mutex, to access the requests list exclusively. */
    pthread_mutex_lock(&request_mutex);

    /* do forever.... */

    while (running)
    {
        if (num_requests > 0)
        {
            /* a request is pending */
            a_request = get_request();

            if (a_request)
            {
                /* got a request - handle it and free it */
                /* unlock mutex - so other threads would be able to handle */
                /* other reqeusts waiting in the queue paralelly.          */
                pthread_mutex_unlock(&request_mutex);
                handle_request(a_request, thread_id);
                free(a_request);
                /* and lock the mutex again. */
                pthread_mutex_lock(&request_mutex);
            }
        }
        else
        {
            /* wait for a request to arrive. note the mutex will be */
            /* unlocked here, thus allowing other threads access to */
            /* requests list.                                       */

            pthread_cond_wait(&got_request, &request_mutex);
            /* and after we return from pthread_cond_wait, the mutex  */
            /* is locked again, so we don't need to lock it ourselves */
        }

        pthread_mutex_lock(&quit_mutex);
        if (request_interrupt)
        {
            running = 0;
        }
        pthread_mutex_unlock(&quit_mutex);
    }

    pthread_mutex_unlock(&request_mutex);
    return NULL;
}

/***********************************************************************
 * func:                    Add a request to the requests list
 * param options:           Options send from the controller
 * param pthread_mutex_t:   Linked list mutex
 * param pthread_cond_t:    Linked list condition variable
 ***********************************************************************/
void add_request(
    int newfd,
    char *buffer,
    char *comand,
    pthread_mutex_t *p_mutex,
    pthread_cond_t *p_cond_var)
{
    struct request *a_request; /* pointer to newly added request.     */

    /* create structure with new request */
    a_request = (struct request *)malloc(sizeof(struct request));
    if (!a_request)
    {
        /* malloc failed?? */
        fprintf(stderr, "add_request: out of memory\n");
        exit(EXIT_FAILURE);
    }

    // a_request->options = *options;
    // a_request->arg_length = arg_length;
    a_request->newfd = newfd;
    a_request->buffer = buffer;
    a_request->command = comand;
    a_request->next = NULL;

    /* lock the mutex, to assure exclusive access to the list */
    pthread_mutex_lock(p_mutex);

    /* add new request to the end of the list, updating list */
    /* pointers as required */
    if (num_requests == 0)
    {
        /* special case - list is empty */
        requests = a_request;
        last_request = a_request;
    }
    else
    {
        last_request->next = a_request;
        last_request = a_request;
    }

    /* increase total number of pending requests by one. */
    num_requests++;

    /* unlock mutex */
    pthread_mutex_unlock(p_mutex);

    /* signal the condition variable - there's a new request to handle */
    pthread_cond_signal(p_cond_var);
}

/***********************************************************************
 * func:            Gets the first pending request from the requests 
 *                  list, removing it from the list.
 * return:          Pointer to the removed request, or NULL if none.
 ***********************************************************************/
struct request *get_request()
{
    struct request *a_request; /* pointer to request.                 */

    if (num_requests > 0)
    {
        a_request = requests;
        requests = a_request->next;
        if (requests == NULL)
        {
            /* this was the last request on the list */
            last_request = NULL;
        }
        /* decrease the total number of pending requests */
        num_requests--;
    }
    else
    {
        /* requests list is empty */
        a_request = NULL;
    }

    /* return the request to the caller. */
    return a_request;
}

/***********************************************************************
 * func:                Handles a single given request 
 * param a_request:     Pointer the the request
 * param thread_id:     ID of the calling thread
 ***********************************************************************/
void handle_request(struct request *a_request, int thread_id)
{
    if (a_request)
    {

        char buffer[MAXBYTES];
        strcpy(buffer, a_request->buffer);

        int arg_length = 0;
        int timeout = 10; /* timeout number for the file */
        bool mem = false;
        pid_t mem_id = -1;
        bool memkill = false;
        double mem_perc = 0.0;
        char *file = calloc(MAXBYTES, sizeof(char));

        char *log_file = calloc(MAXBYTES, sizeof(char));
        char *out_file = calloc(MAXBYTES, sizeof(char));
        char **args = calloc(MAXARGS, sizeof(char *));

        int file_desc = -1;
        pid_t pid = -1;

        double time_spent = 0;
        double time_spent_kill = 0;
        clock_t time_start;
        clock_t time_start_kill;
        clock_t time_one_second;

        extract_options(buffer, file, args, log_file, out_file, &mem, &mem_id, &memkill, &mem_perc, &timeout, &arg_length);

        if (mem)
        {
            if (mem_id != -1)
            {
                memory_log_history(mem_id);
            }
            else
            {
                memory_log_current();
            }
        }
        else if (memkill)
        {
            for (int i = 0; i < num_processes; i++)
            {
                /* calculate memory usage as a percent */
                double mem_usage = (calculate_memory_usage(pid_array[i]) / (double)system_info.totalram) * 100.0;
                if (mem_usage >= mem_perc)
                {
                    /* kill the process if it takes up more than the specified memory percentage */
                    kill(pid_array[i], SIGKILL);
                }
            }
        }
        else
        {
            char log_content[MAXBYTES] = "\0"; /* content that will be written to the log file */

            date_log(log_file);
            sprintf(log_content, "attempting to execute %s\n", a_request->command);
            message_log(log_content, log_file);

            pid = fork();

            if (pid == 0)
            {

                int exit_success = 0;

                if (strlen(out_file) > 0)
                {

                    /* output to file */
                    file_desc = open(out_file, O_CREAT | O_RDWR | O_APPEND, S_IRUSR | S_IWUSR);

                    if (file_desc < 0)
                    {
                        /* free heap allocated memory */
                        free_heap(arg_length, file, args, out_file, log_file);

                        exit(EXIT_FAILURE);
                    }

                    dup2(file_desc, STDOUT_FILENO); // make stdout go to file
                    dup2(file_desc, STDERR_FILENO); // make stderr go to file

                    close(file_desc);

                    exit_success = execv(file, args);
                }
                else
                {
                    /* output to terminal */
                    exit_success = execv(file, args);
                }

                exit_success = execv(file, args);

                if (exit_success == -1)
                {
                    date_log(log_file);
                    printf("could not execute %s\n", a_request->command);
                    exit(EXIT_FAILURE);
                }

                /* close the socket file descriptor */
                close(a_request->newfd);
                exit(EXIT_SUCCESS);
            }
            else if (pid < 0)
            {
                /* process fork error */
                fprintf(stderr, "fork failed\n");
            }
            else
            {
                /* add the new PID to global array of PIDs */
                add_process(pid);

                /* process handled successfully */
                sprintf(log_content, "%s has been executed with pid %d on thread '%d'\n", a_request->command, pid, thread_id);
                date_log(log_file);
                message_log(log_content, log_file);

                /* handle child process timeout */
                int exit_status = -1;
                int wait_status = 0;

                time_start = clock();
                time_one_second = clock();

                /* wait for program to exit or timeout to occur */
                while (wait_status == 0 && time_spent < timeout)
                {
                    if ((clock() - time_one_second) / CLOCKS_PER_SEC >= 1.0)
                    {
                        /* reset 1 second timer */
                        time_one_second = clock();

                        /* append pid and mem info to memlog */
                        log_proccess_info(pid, file, args, arg_length);
                    }

                    time_spent = (double)(clock() - time_start) / CLOCKS_PER_SEC;
                    wait_status = waitpid(pid, &exit_status, WNOHANG);
                }

                /* process timed out */
                if (wait_status == 0)
                {
                    /* terminate the process */
                    if (kill(pid, SIGTERM) == 0)
                    {
                        sprintf(log_content, "sent SIGTERM to %d\n", pid);
                        date_log(log_file);
                        message_log(log_content, log_file);
                    }
                }

                time_start_kill = clock();

                /* wait for program to terminate or timeout to occur */
                while (wait_status == 0 && time_spent_kill < 5)
                {
                    time_spent_kill = (double)(clock() - time_start_kill) / CLOCKS_PER_SEC;
                    wait_status = waitpid(pid, &exit_status, WNOHANG);
                }

                /* process termination timed out */
                if (wait_status == 0)
                {
                    /* terminate the process */
                    if (kill(pid, SIGKILL) == 0)
                    {
                        sprintf(log_content, "sent SIGKILL to %d\n", pid);
                        date_log(log_file);
                        message_log(log_content, log_file);
                    }
                }

                /* wait for the correct exit status */
                while (wait_status == 0)
                {
                    wait_status = waitpid(pid, &exit_status, WNOHANG);
                }

                /* remove the PID from the global PID array */
                remove_process(pid);

                /* process terminated successfully */
                sprintf(log_content, "%d has terminated with status code %d\n", pid, WEXITSTATUS(exit_status));
                date_log(log_file);
                message_log(log_content, log_file);

                fflush(stdout);
            }
        }

        free_heap(arg_length, file, args, out_file, log_file);

        close(a_request->newfd);
    }
}

/***********************************************************************
 * func:                Handles incomming requests from controllers by 
 *                      creating new threads using the given arguments
 * param sockfd:        The socket file descriptor
 * param client_addr:   Address of the connecting controller
 ***********************************************************************/
void request_handler(int sockfd)
{
    int newfd;                      /* new connection on newfd          */
    struct sockaddr_in client_addr; /* connector's address information  */
    socklen_t sin_size;             /* argument length for socket       */

    /* repeat: accept, send, close the connection */
    /* for every accepted connection, use a sepetate process or thread to serve it */
    while (!request_interrupt)
    {
        /* main accept() loop */
        sin_size = sizeof(struct sockaddr_in);
        if ((newfd = accept(sockfd, (struct sockaddr *)&client_addr, &sin_size)) == -1)
        {
            perror("accept");
            continue;
        }

        date_log(NO_LOG);
        printf("connection received from %s\n", inet_ntoa(client_addr.sin_addr));

        /* get the buffer containing controller arguments */
        char buffer[MAXBYTES];
        if (recv(newfd, buffer, MAXBYTES, 0) == -1)
        {
            perror("recv");
            exit(EXIT_FAILURE);
        };

        /* get the command string from the controller buffer */
        char command[MAXBYTES];
        strcpy(command, buffer);

        /* add the request to the threadpool for execution */
        add_request(newfd, buffer, command, &request_mutex, &got_request);

        /* if 1 second has elapsed on the timer, append running process */
    }
}

/***********************************************************************
 * func:                Extracts the arguments from a controller
 * param options:       Pointer to a structure for storing controller 
 *                      options (arguments)
 * param buffer:        String buffer containing controller arguments
 * param file:          Process file name
 * param args:          Argument list for the controller proccess
 * param log_file:      Overseer logging output file name
 * param out_file:      Process output file name
 * param mem:           Memory information command
 * param timeout:       Process timeout value
 * param arg_length:    Number of arguments sent to the process
 ***********************************************************************/
void extract_options(
    char *buffer,
    char *file,
    char **args,
    char *log_file,
    char *out_file,
    bool *mem,
    pid_t *mem_id,
    bool *memkill,
    double *mem_perc,
    int *timeout,
    int *arg_length)
{
    char *options = strtok(buffer, " ");
    int option_index = 0;
    int arg_index = 0;

    while (options != NULL)
    {

        if (strcmp(options, "-log") == 0)
        {
            options = strtok(NULL, " ");
            strcpy(log_file, options);
        }
        else if (strcmp(options, "-o") == 0)
        {
            options = strtok(NULL, " ");
            strcpy(out_file, options);
        }
        else if (strcmp(options, "-t") == 0)
        {
            options = strtok(NULL, " ");
            *timeout = atoi(options);
        }
        else if (strcmp(options, "mem") == 0)
        {
            *mem = true;
            options = strtok(NULL, " ");
            if (options)
            {
                *mem_id = atoi(options);
            }
        }
        else if (strcmp(options, "memkill") == 0)
        {
            *memkill = true;
            options = strtok(NULL, " ");
            if (options)
            {
                *mem_perc = atof(options);
            }
        }
        else
        {
            if (option_index == 0)
            {
                strcpy(file, options);
                option_index++;
            }
            else
            {
                args[arg_index] = calloc(MAXBYTES, sizeof(char));
                strcpy(args[arg_index], options);
                arg_index++;
            }
        }

        options = strtok(NULL, " ");
    }

    *arg_length = arg_index;
}

/***********************************************************************
 * func:                Frees heap allocated memory
 * param arg_index:     Number of arguments passed from a controller
 * param file:          File name
 * param args:          File arguments
 * param out_file:      Output file name
 * param log_file:      Log file name
 * param mem:           Memory information command
 ***********************************************************************/
void free_heap(int arg_index, char *file, char **args, char *out_file, char *log_file)
{
    for (int i = 0; i < arg_index; i++)
    {
        free(args[i]);
    }
    free(args);
    free(log_file);
    free(out_file);
    free(file);
}

/***********************************************************************
 * func:                Handles the SIGINT interrupt
 * param sig:           Signal number
 ***********************************************************************/
void handle_interrupt(int sig)
{
    date_log(NO_LOG);
    printf("Received SIGINT\n");

    pthread_mutex_lock(&quit_mutex);
    request_interrupt = 1;
    pthread_mutex_unlock(&quit_mutex);
}

/***********************************************************************
 * func:                Reads the /proc/<pid>/maps file for the total memory usage of the process
 * param pid:           PID of process
 ***********************************************************************/
int calculate_memory_usage(pid_t pid)
{
    /* dynamically create file directory for pid */
    char filename[50];
    sprintf(filename, "/proc/%d/maps%s", pid, "\0");

    /* open /proc/[pid]/map */
    FILE *fp;
    fp = fopen(filename, "r");

    unsigned int mem_val_1;   /* first memory value on the maps file line    */
    unsigned int mem_val_2;   /* second memory value on the maps file line   */
    unsigned int inode;       /* inode value                                 */
    unsigned int mem_sum = 0; /* track the total memory usage of the process */

    char line_buff[200]; /* char buffer for each line in file           */

    while (line_buff == fgets(line_buff, 200, fp)) // read line from file, max 200 bytes
    {
        /* example /proc/[pid]/maps line output:
        address                   perms offset   dev    inode       pathname
        55f42f7bf000-55f42f801000 rw-p  00000000 00:00  0           [heap]
                  %x-%x           %*s   %*s      %*s    %d          %*s
        */
        sscanf(line_buff, "%x-%x %*s %*s %*s %d %*s", &mem_val_1, &mem_val_2, &inode);

        if (inode == 0)
        {
            /* add the difference between the two memory addresses to the total sum */
            mem_sum += mem_val_2 - mem_val_1;
        }
    }

    fclose(fp); // close file pointer

    return mem_sum;
}

/***********************************************************************
 * func:                Appends a new memory log to the global linked list of logs
 * param pid:           PID of process to log
 ***********************************************************************/
void log_proccess_info(pid_t pid, char *file, char **args, int num_args)
{
    struct memlog *new_memlog;     /* pointer to new memlog structure          */
    struct memlog *current_memlog; /* pointer to current memlog for appending  */
    time_t rawtime;                /* var to hold memlog time                  */
    time(&rawtime);

    /* create structure with new memlog */
    new_memlog = (struct memlog *)malloc(sizeof(struct memlog));
    if (!new_memlog)
    {
        /* malloc failed?? */
        fprintf(stderr, "add_request: out of memory\n");
        exit(EXIT_FAILURE);
    }

    pthread_mutex_lock(&memory_log_mutex);

    /* assign new memlog values */
    new_memlog->pid = pid;
    new_memlog->time = rawtime;
    new_memlog->memory = calculate_memory_usage(pid);
    new_memlog->file = file;
    new_memlog->args = args;
    new_memlog->num_args = num_args;
    new_memlog->next = NULL;

    if (memory_log == NULL)
    {
        memory_log = new_memlog;
    }
    else
    {
        current_memlog = memory_log;
        while (true)
        {
            if (current_memlog->next == NULL)
            {
                current_memlog->next = new_memlog;
                break;
            }
            current_memlog = current_memlog->next;
        }
    }

    free(new_memlog);

    pthread_mutex_unlock(&memory_log_mutex);
}

/***********************************************************************
 * func:                Prints the memory log info for a given pid
 * param pid:           PID of process to log
 ***********************************************************************/
void memory_log_history(pid_t pid)
{
    struct memlog *current_log;
    struct tm *info;
    char buffer[100];

    current_log = memory_log;

    if (current_log != NULL)
    {
        while (current_log->next != NULL)
        {
            if (current_log->pid == pid)
            {
                info = localtime(&current_log->time);
                strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", info);

                /* print to the overseer's output for now */
                printf("%s %d \n", buffer, current_log->memory);
            }
            current_log = current_log->next;
        }
    }
}

/***********************************************************************
 * func:                Returns the latest memlog of a given pid
 * param pid:           PID of process to return latest log
 ***********************************************************************/
struct memlog *get_latest_entry(pid_t pid)
{
    struct memlog *current_log;
    struct memlog *last_relevant_log = NULL;

    current_log = memory_log;

    if (current_log == NULL)
    {
        return NULL;
    }

    /* transverse entire history log until the last log relevant to the PID is found */
    /* this is necessary because the linked list is stored in chronological order    */
    while (current_log->next != NULL)
    {
        if (current_log->pid == pid)
        {
            last_relevant_log = current_log;
        }

        current_log = current_log->next;
    }

    return last_relevant_log;
}

/***********************************************************************
 * func:                Prints information about all currently running processes
 ***********************************************************************/
void memory_log_current()
{
    struct memlog *last_relevant_log;

    for (int i = 0; i < num_processes; i++)
    {
        last_relevant_log = get_latest_entry(pid_array[i]);

        if (last_relevant_log != NULL)
        {
            printf("%d %d %s ", pid_array[i], last_relevant_log->memory, last_relevant_log->file);

            for (int j = 0; j < last_relevant_log->num_args; j++)
            {
                printf("%s ", last_relevant_log->args[j]);
            }

            printf("\n");
        }
    }
}

/***********************************************************************
 * func:                Safely adds a PID to the global array of current PIDs
 * param pid:           PID of process to add
 ***********************************************************************/
void add_process(pid_t pid)
{
    pthread_mutex_lock(&pid_array_mutex);

    pid_array[num_processes++] = pid;

    pthread_mutex_unlock(&pid_array_mutex);
}

/***********************************************************************
 * func:                Safely removes a PID to the global array of current PIDs
 * param pid:           PID of process to remove
 ***********************************************************************/
void remove_process(pid_t pid)
{
    int found_pid = 0;

    pthread_mutex_lock(&pid_array_mutex);

    for (int i = 0; i < num_processes; i++)
    {
        if (pid_array[i] == pid)
        {
            found_pid = 1;
        }

        if (i == num_processes - 1 && found_pid == 1) // if found PID is last element in array
        {
            pid_array[i] = 0;
            break;
        }
        else if (found_pid == 1)
        {
            pid_array[i] = pid_array[i + 1];
        }
    }

    num_processes--;

    pthread_mutex_unlock(&pid_array_mutex);
}

/***********************************************************************
 * func:                Exits the program by closing all threads
 * param sig:           Threads to close
 ***********************************************************************/
void exit_gracefully(pthread_t *p_threads)
{
    pthread_cond_broadcast(&got_request);

    date_log(NO_LOG);
    printf("Cleaning and terminating...\n");

    // sleep(1);

    for (int i = 0; i < NUM_HANDLER_THREADS; i++)
    {
        pthread_join(p_threads[i], NULL);
    }

    exit(EXIT_SUCCESS);
}

/***********************************************************************
 * func:                Handles output of log messages
 * param output:        The message to be output
 * param log_file:      The file to output to
 ***********************************************************************/
void message_log(char *output, char *log_file)
{
    if (strlen(log_file) > 0)
    {

        FILE *out_file;
        int write_success;
        int close_success;

        if ((out_file = fopen(log_file, "a")) == NULL)
        {
            perror("fopen");
            exit(EXIT_FAILURE);
        };

        if ((write_success = fputs(output, out_file)) == EOF)
        {
            perror("fputs");
            exit(EXIT_FAILURE);
        };

        if ((close_success = fclose(out_file)) != 0)
        {
            perror("fclose");
            exit(EXIT_FAILURE);
        };
    }
    else
    {
        printf("%s", output);
    }

    return;
}

/***********************************************************************
 * func:                Logs the current date and time
 * param log_file:      The file to output to
 ***********************************************************************/
void date_log(char *log_file)
{
    time_t rawtime;
    struct tm *info;
    char buffer[80];

    time(&rawtime);

    info = localtime(&rawtime);

    strftime(buffer, 80, "%Y/%m/%d %X - ", info);

    if (strlen(log_file) > 0)
    {

        FILE *out_file;
        int write_success;
        int close_success;

        if ((out_file = fopen(log_file, "a")) == NULL)
        {
            perror("fopen");
            exit(EXIT_FAILURE);
        };

        if ((write_success = fputs(buffer, out_file)) == EOF)
        {
            perror("fputs");
            exit(EXIT_FAILURE);
        };

        if ((close_success = fclose(out_file)) != 0)
        {
            perror("fclose");
            exit(EXIT_FAILURE);
        };
    }
    else
    {
        printf(buffer);
    }

    return;
}

/***********************************************************************
 * func:            Sends bytes from the controller to the server
 * param argc:      The number of arguments given to the controller
 * param argv:      Command line arguments provided to the controller
 * param sockfd:    Pointer to the socket file descriptor
 ***********************************************************************/
void send_bytes(char *buffer, int *sockfd)
{
    /* send each byte to the observer */
    if (send(*sockfd, buffer, MAXBYTES, 0) == -1)
    {
        perror("send");
        exit(EXIT_FAILURE);
    }
}