/*
 * net-cluster — minimal telnet server (plaintext, single foreground session).
 * License: MIT
 *
 * Pure socket/IAC/session plumbing; the shell coupling lives in native_shell.
 * Listens on `port`, accepts one client, and runs a line loop: each received
 * line is passed to exec() with stdout redirected to the socket, so every
 * existing shell command's output is sent to the client.  Returns when the
 * client disconnects or sends exit/bye/quit.
 */
#pragma once

int telnetd_run(unsigned short port, void (*exec)(char *line));
