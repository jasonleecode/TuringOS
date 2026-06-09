/*
 * native_shell — telnet command glue.
 *
 * The socket/IAC/session machinery lives in net-cluster's libnc_telnet; here
 * we provide the shell-specific exec callback (tokenise a line and dispatch it
 * through the same commands[] table the console uses) and the `telnetd` command.
 */

#include "commands.h"
#include <nc_telnet.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

#define TELNET_MAX_ARGS 32

/* Tokenise one line on whitespace and dispatch via the shared command table.
 * Mirrors main.cc's console dispatch (minus redirects/backgrounding). */
static void shell_exec_line(char *line)
{
  char *argv[TELNET_MAX_ARGS];
  int   argc = 0;
  char *p    = line;

  while (*p && argc < TELNET_MAX_ARGS - 1) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
  }
  argv[argc] = nullptr;
  if (argc == 0) return;

  for (int i = 0; i < num_commands; i++) {
    if (strcmp(argv[0], commands[i].name) == 0) {
      commands[i].func(argc, argv);
      return;
    }
  }
  printf("%s: command not found\n", argv[0]);
}

void cmd_telnetd(int argc, char **argv)
{
  unsigned short port = 23;
  if (argc >= 2) {
    int p = atoi(argv[1]);
    if (p > 0 && p < 65536)
      port = (unsigned short)p;
  }

  if (!net_is_ready()) {
    printf("telnetd: network not ready yet\n");
    return;
  }

  telnetd_run(port, shell_exec_line);
}
