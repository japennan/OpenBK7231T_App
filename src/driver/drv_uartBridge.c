#include "../new_common.h"
#include "../logging/logging.h"
#include "../mqtt/new_mqtt.h"
#include "../httpserver/new_http.h"
#include "drv_uart.h"
#include "drv_uartBridge.h"

// Reads complete newline-terminated lines from the generic UART RX ring buffer
// (set up by the "uartInit <baud>" command) and publishes each line to MQTT
// under "<deviceTopic>/uart_rx". Used to relay replies from an MCU on the other
// end of the UART (e.g. the WL5's PY32) back to Wi-Fi/MQTT. Both '\r' and '\n'
// terminate a line; empty lines (e.g. the second byte of a CRLF) are ignored.

#define UB_LINE_MAX 128
static char ub_line[UB_LINE_MAX];
static int  ub_len = 0;
static char ub_lastLine[UB_LINE_MAX] = "";

void UARTBridge_Init(void) {
	ub_len = 0;
	addLogAdv(LOG_INFO, LOG_FEATURE_DRV,
		"UARTBridge: started; RX lines -> MQTT 'uart_rx' (run uartInit first)");
}

void UARTBridge_RunQuickTick(void) {
	int avail = UART_GetDataSize();
	int i;

	if (avail <= 0)
		return;

	for (i = 0; i < avail; i++) {
		byte b = UART_GetByte(i);
		if (b == '\r' || b == '\n') {
			if (ub_len > 0) {
				ub_line[ub_len] = 0;
				memcpy(ub_lastLine, ub_line, ub_len + 1);
				MQTT_PublishMain_StringString("uart_rx", ub_line, 0);
				addLogAdv(LOG_INFO, LOG_FEATURE_DRV, "UARTBridge RX: %s", ub_line);
				ub_len = 0;
			}
		} else if (ub_len < UB_LINE_MAX - 1) {
			ub_line[ub_len++] = (char)b;
		} else {
			// line too long - drop it and resync on the next terminator
			ub_len = 0;
		}
	}

	UART_ConsumeBytes(avail);
}

void UARTBridge_AppendInformationToHTTPIndexPage(http_request_t *request, int bPreState) {
	if (bPreState)
		return;
	hprintf255(request, "<h5>UARTBridge last RX line: %s</h5>", ub_lastLine);
}
