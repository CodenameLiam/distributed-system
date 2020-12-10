#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "controller.h"

/* length of our communication buffer */
#define MAXBYTES 1024

/***********************************************************************
 * func:            Controller Entry Point 
 ***********************************************************************/
int main(int argc, char *argv[])
{

    /* controller socket file descriptor */
    int sockfd;

    /* initialise the controller */
    init(argc, argv);

    /* connect to the server */
    connect_to_server(&sockfd, argv);

    /* send information to the server */
    send_bytes(argc, argv, &sockfd);

    if(strcmp(argv[3], "mem") == 0)
    {
        // recv_bytes(&sockfd);
    }

    /* close the socket file descriptor */
    close(sockfd);

    return 0;
}

/***********************************************************************
 * func:            Initialises the application and looks for errors
 * param argc:      The number of arguments given to the controller
 * param argv:      Command line arguments provided to the controller
 ***********************************************************************/
void init(int argc, char *argv[])
{
    /* print help message */
    if (argc > 1 && strcmp(argv[1], "--help") == 0)
    {
        display_usage();
        exit(EXIT_SUCCESS);
    }

    /* required arguments not met */
    if (argc < 4)
    {
        display_usage();
        exit(EXIT_FAILURE);
    }
}

/***********************************************************************
 * func:            Displays the commands required to start 
 *                  the controller
 ***********************************************************************/
void display_usage()
{
    printf("Usage: controller <address> <port> {[-o out_file] [-log log_file] [-t seconds] <file> [arg...] | mem [pid] | memkill <percent>}\n");
}

/***********************************************************************
 * func:            Connect to a given server address
 * param sockfd:    Pointer to the socket file descriptor
 * param argv:      Command line arguments provided to the controller
 ***********************************************************************/
void connect_to_server(int *sockfd, char **argv)
{
    /* connector's address information */
    struct hostent *he;
    struct sockaddr_in host_addr;

    /* get the port number */
    int port = strtol(argv[2], NULL, 10);
    if (!(port > 0 && port < 65535))
    {
        fprintf(stderr, "Please enter a valid port number\n");
        exit(EXIT_FAILURE);
    }

    if ((he = gethostbyname(argv[1])) == NULL)
    {
        herror("gethostbyname");
        exit(EXIT_FAILURE);
    }

    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* clear address struct */
    memset(&host_addr, 0, sizeof(host_addr));

    host_addr.sin_family = AF_INET;   /* host byte order */
    host_addr.sin_port = htons(port); /* short, network byte order */
    host_addr.sin_addr = *((struct in_addr *)he->h_addr);

    if (connect(*sockfd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr)) == -1)
    {
        fprintf(stderr, "Could not connect to overseer at %s %s\n", argv[1], argv[2]);
        exit(EXIT_FAILURE);
    }
}

/***********************************************************************
 * func:            Sends bytes from the controller to the server
 * param argc:      The number of arguments given to the controller
 * param argv:      Command line arguments provided to the controller
 * param sockfd:    Pointer to the socket file descriptor
 ***********************************************************************/
void send_bytes(int argc, char *argv[], int *sockfd)
{
    char buff[MAXBYTES];
    int index = 0;

    /* copy arguments using a buffer */
    for (int i = 0; i < argc - 3; i++)
    {
        strcpy(&buff[index], argv[3 + i]);
        index += strlen(argv[3 + i]);
        buff[index++] = ' ';
    }

    buff[index] = '\0';

    /* send each byte to the observer */
    if (send(*sockfd, buff, MAXBYTES, 0) == -1)
    {
        perror("send");
        exit(EXIT_FAILURE);
    }
}

void recv_bytes(int *sockfd)
{
    int newfd;                      /* new connection on newfd          */
    struct sockaddr_in client_addr; /* connector's address information  */
    socklen_t sin_size;             /* argument length for socket       */

    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* start listnening */
    if (listen(*sockfd, 1) == -1)
    {
        perror("listen");
        exit(1);
    }

    sin_size = sizeof(struct sockaddr_in);
    if ((newfd = accept(*sockfd, (struct sockaddr *)&client_addr, &sin_size)) == -1)
    {
        perror("accept");
    }

    char buffer[MAXBYTES];
    if (recv(newfd, buffer, MAXBYTES, 0) == -1)
    {
        perror("recv");
        exit(EXIT_FAILURE);
    };

    printf("%s\n", buffer);
}