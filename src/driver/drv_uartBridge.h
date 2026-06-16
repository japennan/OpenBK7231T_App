#pragma once

#include "../httpserver/new_http.h"

// Generic UART RX -> MQTT line bridge. Reads complete newline-terminated lines
// from the generic UART receive ring buffer (set up by the "uartInit <baud>"
// command) and publishes each line to MQTT under "<deviceTopic>/uart_rx".
// Start with: startDriver UARTBridge   (after uartInit <baud>)
void UARTBridge_Init(void);
void UARTBridge_RunQuickTick(void);
void UARTBridge_AppendInformationToHTTPIndexPage(http_request_t *request, int bPreState);

// Send "cmd\r\n" on the UART and wait up to timeoutMs for the next reply line
// (captured by this driver). Returns 1 and fills out[] on success, 0 on timeout.
int UARTBridge_SendCommandAndWait(const char *cmd, char *out, int outSize, int timeoutMs);
