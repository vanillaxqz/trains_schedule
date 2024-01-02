#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <string.h>

extern int errno;

int port;

int main(int argc, char *argv[])
{
    int socket_desc;
    struct sockaddr_in server;

    if (argc != 3)
    {
        printf("[client]  syntax: %s <sv_addr> <port>\n", argv[0]);
        return -1;
    }

    port = atoi(argv[2]);

    if ((socket_desc = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("[client] error: socket().\n");
        return errno;
    }

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(argv[1]);
    server.sin_port = htons(port);

    if (connect(socket_desc, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
    {
        if (errno = ECONNREFUSED)
        {
            printf("[client] server is not online.\n");
            return 0;
        }
        perror("[client] error: connect().\n");
        return errno;
    }

    fd_set fds;
    char cmd[100];
    char buffer[2048];

    while (1)
    {
        FD_ZERO(&fds);
        FD_SET(socket_desc, &fds);
        FD_SET(STDIN_FILENO, &fds);

        if (select(socket_desc + 1, &fds, NULL, NULL, NULL) < 0)
        {
            perror("[client] error: select().\n");
            return errno;
        }

        if (FD_ISSET(socket_desc, &fds))
        {
            int bytes;
            bzero(buffer, 2048);
            if ((bytes = read(socket_desc, buffer, sizeof(buffer))) < 0)
            {
                if (errno == ECONNRESET)
                {
                    printf("\n[client] server closed the connection.\n");
                    return 0;
                }
                perror("[client] error: read().\n");
                return errno;
            }
            else if (bytes == 0)
            {
                if (strcmp(cmd, "quit") != 0)
                    printf("\n[client] server closed the connection.\n");
                return 0;
            }
            printf("\n%s\n[client] > ", buffer);
            fflush(stdout);
        }

        if (FD_ISSET(STDIN_FILENO, &fds))
        {
            bzero(cmd, 100);
            read(0, cmd, sizeof(cmd));
            cmd[strlen(cmd) - 1] = '\0';
            if (strlen(cmd) == 0)
                continue;
            if (write(socket_desc, cmd, strlen(cmd) + 1) < 0)
            {
                if (errno == ECONNRESET)
                {
                    printf("\n[client] server closed the connection.\n");
                    return 0;
                }
                perror("[client] error: write().\n");
                return errno;
            }
        }
    }
}
