/*
 * net-cluster — MQTT client shell command.
 * License: MIT
 *
 * Registered by native_shell (which owns the lwIP stack).
 *   mqtt pub <broker_ip> <topic> <message>
 *   mqtt sub <broker_ip> <topic> [seconds]
 */
#pragma once

void cmd_mqtt(int argc, char **argv);
