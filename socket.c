#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h> // for bind, recvfrom, socket, AF_INET

#define UDP_BUFFER_SIZE 500

in_addr_t parseIp(uint8_t *addr) {
  return (addr[0] << 24 | addr[1] << 16 | addr[2] << 8 | addr[3]);
}

int init_socket(int port) {
  struct sockaddr_in clientaddr = {0};
  char buffer[UDP_BUFFER_SIZE] = {0};


  clientaddr.sin_family = AF_INET; // IPv4
  clientaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  clientaddr.sin_port = htons(port);

  int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socketFd < 0) {
    fprintf(stderr, "Failed to create UDP socket, status = %d\n", socketFd);
  }


  int bindErr =
      bind(socketFd, (const struct sockaddr *)&clientaddr, sizeof(clientaddr));
  if (bindErr < 0) {
    fprintf(stderr, "Failed to bind to interface, status = %d\n", bindErr);
  }
  printf("Listening for UDP traffic on port %d\n", ntohs(clientaddr.sin_port));

  return socketFd;
}