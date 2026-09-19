/* echo.c - TCP Echo Client for MP1 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "echo_io.h"
#include "util.h"

int main(int argc, char **argv) {
    int sockfd;
    int port;
    struct sockaddr_in servaddr;
    char sendline[ECHO_BUFSIZE];
    char recvline[ECHO_BUFSIZE];

    /* 1. Validate command line arguments: ./echo <server_ipv4> <port> */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ipv4> <port>\n", argv[0]);
        exit(1);
    }

    port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port %s (must be 1-65535)\n", argv[2]);
        exit(1);
    }

    /* 2. Create IPv4 TCP socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    /* 3. Setup server address structure */
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP format '%s'\n", argv[1]);
        close(sockfd);
        exit(1);
    }

    /* 4. Initiate connection */
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Connect failed");
        close(sockfd);
        exit(1);
    }

    /* 5. I/O loop: read from stdin, send to network, read echo, print to stdout */
    while (fgets(sendline, sizeof(sendline), stdin) != NULL) {
        if (writen(sockfd, sendline, strlen(sendline)) < 0) {
            perror("writen error");
            close(sockfd);
            exit(1);
        }

        ssize_t n = readline(sockfd, recvline, sizeof(recvline));
        if (n < 0) {
            perror("readline error");
            close(sockfd);
            exit(1);
        } else if (n == 0) {
            fprintf(stderr, "Echo client: Server terminated prematurely.\n");
            close(sockfd);
            exit(1);
        }

        fputs(recvline, stdout);
    }

    /* 6. Close socket on EOF (Ctrl+D) and exit */
    close(sockfd);
    exit(0);
}
