#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#include "list.h"

#define MAX_MESSAGE_LEN 256

List *serverList;
List *remoteList;

int sock;
int remote_port;
char * remote_host;

struct sockaddr_in server, remote;

pthread_t input_tid, udp_output_tid, udp_tid, screen_tid;

pthread_mutex_t serverMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t remoteMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t serverCond;
pthread_cond_t remoteCond;

bool activeConnection = true;
bool sendMessage = false;
bool printMessage = false;


/*

    pthread_testcancel(); used frequently to test if thread died

*/


// deallocate memory and destroy mutexes
void free_all() {
    List_free(serverList, NULL);
    List_free(remoteList, NULL);

    pthread_mutex_unlock(&serverMutex);
    pthread_mutex_unlock(&remoteMutex);

    pthread_mutex_destroy(&serverMutex);
    pthread_mutex_destroy(&remoteMutex);
}

// Thread for user input
void *input_thread(void *arg) {
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    char message[MAX_MESSAGE_LEN] = "";

    while (activeConnection) {
        pthread_testcancel();
        pthread_mutex_lock(&serverMutex);

        while (sendMessage) {
            pthread_testcancel();
            pthread_cond_wait(&serverCond, &serverMutex);
        }

        while (!sendMessage) {
            pthread_testcancel();
            fgets(message, MAX_MESSAGE_LEN, stdin);

            List_append(serverList, message);
            sendMessage = true;

            // Unlock mutex, signal condition for sending
            pthread_mutex_unlock(&serverMutex);
            pthread_cond_signal(&serverCond);

            // Check for termination command
            if (message[0] == '!' && strlen(message) == 2) {
                activeConnection = false;
                printf("Session Terminated.\n");

                // Cancel threads, close socket, and free resources
                pthread_cancel(udp_output_tid);

                memset(message, '\0', MAX_MESSAGE_LEN);
                close(sock);

                free_all();

                pthread_cancel(udp_tid);
                pthread_cancel(screen_tid);
                
                pthread_exit(0);

                return NULL;
            }
        }
    }

    pthread_exit(NULL);

    return NULL;
}

// Thread for sending messages over UDP
void *udp_output_thread(void *arg) {
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (activeConnection) {
        pthread_testcancel();
        pthread_mutex_lock(&serverMutex);

        while (!sendMessage) {
            pthread_testcancel();
            pthread_cond_wait(&serverCond, &serverMutex);
        }

        while (sendMessage) {
            pthread_testcancel();

            // Send messages while the list is not empty
            while (List_count(serverList) > 0 && List_first(serverList) != NULL && List_curr(serverList) != NULL) {
                pthread_testcancel();
                List_first(serverList);
                sendto(sock, (char *)List_remove(serverList), MAX_MESSAGE_LEN, 0, (struct sockaddr *)&remote, sizeof(struct sockaddr_in));
            }

            // Unlock mutex, signal condition for sending, and set flag to false
            pthread_mutex_unlock(&serverMutex);
            pthread_cond_signal(&serverCond);
            sendMessage = false;
        }
    }

    pthread_exit(NULL);

    return NULL;
}

// Thread for receiving messages over UDP
void *udp_input_thread(void *arg) {
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    char message[MAX_MESSAGE_LEN] = "";
    socklen_t fromlen = sizeof(remote);

    while (activeConnection) {
        pthread_testcancel();
        pthread_mutex_lock(&remoteMutex);

        while (printMessage) {
            pthread_testcancel();
            pthread_cond_wait(&remoteCond, &remoteMutex);
        }

        while (!printMessage) {
            pthread_testcancel();
            recvfrom(sock, message, MAX_MESSAGE_LEN, 0, (struct sockaddr *)&remote, &fromlen);

            List_append(remoteList, message);
            printMessage = true;

            // Unlock mutex, signal condition, and cleanup
            pthread_mutex_unlock(&remoteMutex);
            pthread_cond_signal(&remoteCond);

            // 33 is '!' in ascii
            // Check for termination command
            if (message[0] == '!' && strlen(message) == 2) {
                activeConnection = false;
                printf("%s %d has ended the session.\n", remote_host, remote_port);
                
                // Cancel screen output thread, clean up, and exit               
                pthread_cancel(screen_tid);
                
                memset(message, '\0', MAX_MESSAGE_LEN);
                close(sock);

                free_all();

                pthread_cancel(input_tid);
                pthread_cancel(udp_output_tid);

                pthread_exit(0);

                return NULL;
            }
        }
    }

    pthread_exit(NULL);

    return NULL;
}

// Thread for printing messages to the screen
void *output_thread(void *arg) {
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (activeConnection) {
        pthread_testcancel();
        pthread_mutex_lock(&remoteMutex);

        while (!printMessage) {
            pthread_testcancel();
            pthread_cond_wait(&remoteCond, &remoteMutex);
        }

        while (printMessage) {
            pthread_testcancel();

            // Print messages from the list while not empty
            while (List_count(remoteList) > 0) {
                pthread_testcancel();
                List_first(remoteList);
                printf("%s %d: %s", remote_host, remote_port, (char *)List_remove(remoteList));
            }

            // Unlock mutex, signal condition for printing, and set flag to false
            pthread_mutex_unlock(&remoteMutex);
            pthread_cond_signal(&remoteCond);
            printMessage = false;
        }
    }

    pthread_exit(NULL);

    return NULL;
}

// create a UDP socket
void create_socket(int server_port) {
    // Error handling for socket creation
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    if (sock == -1) {
        perror("Error: Could not create socket");
        exit(1);
    }

    printf("Socket created successfully.\n");

    // Configure server details
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(server_port);

    // Error handling for socket binding
    int binder = bind(sock, (struct sockaddr *)&server, sizeof(struct sockaddr_in));
    
    if (binder < 0) {
        perror("Error: Bind failed");
        close(sock);
        exit(1);
    }

    printf("Socket binding successful.\n");
    
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *result_ptr;
    int status = getaddrinfo(remote_host, NULL, &hints, &result_ptr);
    if (status != 0){
        fprintf(stderr, "Error: %s\n", gai_strerror(status));
        close(sock);
        exit(1);
    }

    struct sockaddr_in *resolved_address = (struct sockaddr_in*)result_ptr->ai_addr;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_addr = resolved_address->sin_addr;
    remote.sin_port = htons(remote_port);

    freeaddrinfo(result_ptr);
}

// create and manage threads
void create_pthreads() {
    // Create threads for different tasks
    pthread_create(&input_tid, NULL, input_thread, NULL);
    pthread_create(&udp_output_tid, NULL, udp_output_thread, NULL);
    pthread_create(&udp_tid, NULL, udp_input_thread, NULL);
    pthread_create(&screen_tid, NULL, output_thread, NULL);

    // Join threads
    pthread_join(input_tid, NULL);
    pthread_join(udp_output_tid, NULL);
    pthread_join(udp_tid, NULL);
    pthread_join(screen_tid, NULL);
}

int main(int argc, char *argv[]) {
    if (argc != 4){
        fprintf(stderr, "Usage: %s [my port number] [remote machine name] [remote port number]\n", argv[0]);
        exit(1);
    }

    int server_port = atoi(argv[1]);
    remote_host = argv[2];
    remote_port = atoi(argv[3]);

    printf("Initializing with: my port - %d, remote machine - %s, remote port - %d\n", server_port, remote_host, remote_port); // Add debug statement

    create_socket(server_port);

    // Initialize lists for server and remote messages
    serverList = List_create();
    remoteList = List_create();

    create_pthreads();

    close(sock);

    return 0;
}
