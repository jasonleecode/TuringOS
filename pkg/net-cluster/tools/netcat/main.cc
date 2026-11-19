// Standalone TCP client using the POSIX socket interface.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
int main(int argc,char **argv)
{
  if (argc != 4) { puts("usage: netcat <ip> <port> <msg>"); return 1; }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  char *end;
  long port = strtol(argv[2],&end,10);
  if (*end || port <= 0 || port > 65535 || inet_pton(AF_INET,argv[1],&address.sin_addr) != 1) {
    puts("netcat: invalid address"); return 1;
  }
  address.sin_port = htons(port);
  int fd = socket(AF_INET,SOCK_STREAM,0);
  if (fd < 0 || connect(fd,reinterpret_cast<sockaddr *>(&address),sizeof(address)) < 0) {
    perror("netcat: connect"); if (fd >= 0) close(fd); return 1;
  }
  size_t sent = 0, size = strlen(argv[3]);
  while (sent < size) {
    ssize_t n = send(fd,argv[3]+sent,size-sent,0);
    if (n <= 0) { perror("netcat: send"); close(fd); return 1; }
    sent += n;
  }
  printf("netcat: sent %u bytes to %s:%ld\n",(unsigned)sent,argv[1],port);
  char buf[1600];
  ssize_t n = recv(fd,buf,sizeof(buf),0);
  if (n < 0) perror("netcat: recv");
  else if (!n) puts("netcat: peer closed (no data)");
  else printf("netcat: recv %u bytes: %.*s\n",(unsigned)n,(int)n,buf);
  close(fd);
  return n < 0;
}
