/*
 * net-cluster — minimal telnet server (plaintext, single foreground session).
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Socket/IAC/session plumbing only.  The shell coupling (tokenise + dispatch)
 * is injected as the exec() callback by native_shell.  We redirect STDOUT to
 * the client socket for the session, so every existing command's printf output
 * is delivered to the remote — the same trick the shell's `>` redirect uses.
 */

#include "nc_telnet.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

/* RFC 854 telnet command bytes we need to recognise. */
enum { TELNET_IAC = 0xFF };

int telnetd_run(unsigned short port, void (*exec)(char *line))
{
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { printf("telnetd: socket failed\n"); return -1; }

  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    printf("telnetd: bind on port %u failed\n", port);
    close(srv);
    return -1;
  }
  if (listen(srv, 1) < 0) {
    printf("telnetd: listen failed\n");
    close(srv);
    return -1;
  }

  printf("telnetd: listening on port %u (one session; client 'exit' to end)\n",
         port);

  struct sockaddr_in cli;
  socklen_t          clen = sizeof(cli);
  int fd = accept(srv, (struct sockaddr *)&cli, &clen);
  close(srv);                       // single foreground session
  if (fd < 0) { printf("telnetd: accept failed\n"); return -1; }
  printf("telnetd: client connected from %s\n", inet_ntoa(cli.sin_addr));

  /* Redirect stdout to the socket for the whole session.  When stdout is not a
   * tty it is fully buffered, so we fflush after every prompt / command. */
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  dup2(fd, STDOUT_FILENO);

  printf("TuringOS telnet — type 'exit' to disconnect\r\n");
  printf("turingos(telnet)> ");
  fflush(stdout);

  char    line[512];
  int     li   = 0;
  bool    quit = false;
  char    buf[256];

  while (!quit) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;                        // client disconnected

    for (ssize_t i = 0; i < n && !quit; i++) {
      unsigned char c = (unsigned char)buf[i];

      if (c == TELNET_IAC) {        // skip 3-byte IAC <cmd> <opt> negotiation
        i += 2;
        continue;
      }
      if (c == '\r')                // ignore CR (clients send CRLF)
        continue;

      if (c == '\n') {
        line[li] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t')
          p++;

        if (strcmp(p, "exit") == 0 || strcmp(p, "bye") == 0 ||
            strcmp(p, "quit") == 0) {
          quit = true;
          break;
        }
        if (*p) {
          exec(p);                  // command output -> socket (stdout dup'd)
          fflush(stdout);
        }
        li = 0;
        printf("turingos(telnet)> ");
        fflush(stdout);
      } else if (li < (int)sizeof(line) - 1) {
        line[li++] = (char)c;
      }
    }
  }

  printf("bye\r\n");
  fflush(stdout);

  dup2(saved, STDOUT_FILENO);       // restore console stdout
  close(saved);
  close(fd);
  printf("telnetd: session ended\n");
  return 0;
}
