#pragma once
// hydra: log over Wi-Fi without hurting the board, plus a "black box" that survives a reset.
//
// - Every log line (all levels) goes into a ring buffer in RTC memory. After a reset (SW, WDT, panic, brownout, deep sleep)
//   the previous boot's tail is copied out at the first log call and sent once Wi-Fi is up.
// - Lines at or above the chosen level (INFO by default, DEBUG with ":debug" after the port in network.rsyslog_server)
//   go into a RAM buffer that is sent as one UDP datagram at most every 500 ms from the main loop, over one socket that stays
//   open. A full buffer drops its oldest lines (counted) instead of blocking.
// Datagram: "HYDRALOG 1 <kind>\n" + lines "<uptime_s> <level> [thread] text\n"; kind = "live" or "prev <reason> <first> <last>" (0/1: one black box goes out in several datagrams).
#include "configuration.h"
#include <stdarg.h>

#if defined(ARCH_ESP32)
void hydraLogLine(const char *logLevel, const char *thread, const char *format, va_list arg);
void hydraLogTick(); // main loop: send pending lines
#endif
