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

/* When set, the async RX->MQTT tick stops consuming the UART RX buffer so a
 * PY32 firmware push (UARTBridge_PushFirmware) has exclusive use of the UART. */
static volatile int ub_ota_active = 0;

/* PY32 OTA wire protocol — must match the WL5 bootloader (User/ota_proto.h). */
#define OTA_BLOCK_SIZE  128
#define OTA_SOH         0x01
#define OTA_ACK         0x06
#define OTA_NAK         0x15
#define OTA_BEACON      0x43   /* 'C' */
#define OTA_DONE_OK     0x4F   /* 'O' */
#define OTA_DONE_ERR    0x45   /* 'E' */
// Incremented once per fully received line. Used by UARTBridge_SendCommandAndWait
// (the synchronous /api/uartcmd HTTP endpoint) to detect a fresh reply without
// touching the RX ring buffer that this driver's tick already consumes.
static volatile unsigned int ub_lineSeq = 0;

void UARTBridge_Init(void) {
	ub_len = 0;
	addLogAdv(LOG_INFO, LOG_FEATURE_DRV,
		"UARTBridge: started; RX lines -> MQTT 'uart_rx' (run uartInit first)");
}

void UARTBridge_RunQuickTick(void) {
	int avail;
	int i;

	if (ub_ota_active)
		return;		/* a firmware push owns the UART right now */

	avail = UART_GetDataSize();
	if (avail <= 0)
		return;

	for (i = 0; i < avail; i++) {
		byte b = UART_GetByte(i);
		if (b == '\r' || b == '\n') {
			if (ub_len > 0) {
				ub_line[ub_len] = 0;
				memcpy(ub_lastLine, ub_line, ub_len + 1);
				ub_lineSeq++;	// publish line first, then bump seq for waiters
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

// Synchronous request/response helper: send "cmd\r\n" on the UART, then wait
// (up to timeoutMs) for this driver's tick to capture the next reply line and
// copy it into out. Returns 1 on a reply, 0 on timeout. Requires the UARTBridge
// driver to be running (it is the sole RX consumer; we only watch ub_lineSeq).
int UARTBridge_SendCommandAndWait(const char *cmd, char *out, int outSize, int timeoutMs) {
	unsigned int startSeq = ub_lineSeq;
	int waited = 0;

	while (*cmd) {
		UART_SendByte((byte)*cmd);
		cmd++;
	}
	UART_SendByte('\r');
	UART_SendByte('\n');

	while (waited < timeoutMs) {
		if (ub_lineSeq != startSeq) {
			int n = (int)strlen(ub_lastLine);
			if (n >= outSize)
				n = outSize - 1;
			memcpy(out, ub_lastLine, n);
			out[n] = 0;
			return 1;
		}
		rtos_delay_milliseconds(5);
		waited += 5;
	}

	if (outSize > 0)
		out[0] = 0;
	return 0;
}

/* ---- PY32 firmware push (drives the bootloader's OTA protocol) ------------- */

static uint32_t ub_crc32(const uint8_t *d, uint32_t n) {
	uint32_t crc = 0xFFFFFFFFu;
	while (n--) {
		crc ^= *d++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
	}
	return crc ^ 0xFFFFFFFFu;
}

static void ub_drain_rx(void) {
	int n = UART_GetDataSize();
	if (n > 0)
		UART_ConsumeBytes(n);
}

/* one byte within timeout_ms, or -1 */
static int ub_get_byte(int timeout_ms) {
	int waited = 0;
	while (waited <= timeout_ms) {
		if (UART_GetDataSize() > 0) {
			int b = UART_GetByte(0) & 0xFF;
			UART_ConsumeBytes(1);
			return b;
		}
		rtos_delay_milliseconds(2);
		waited += 2;
	}
	return -1;
}

/* wait for a non-beacon byte (ACK/NAK/result), ignoring 'C' beacons */
static int ub_wait_resp(int timeout_ms) {
	int waited = 0;
	while (waited <= timeout_ms) {
		int b = ub_get_byte(50);
		if (b == OTA_BEACON) continue;
		if (b >= 0) return b;
		waited += 50;
	}
	return -1;
}

static void ub_send(const uint8_t *d, int n) {
	while (n-- > 0)
		UART_SendByte(*d++);
}

// Push a new PY32 application image (raw, for 0x08002000) over the UART by
// driving the bootloader protocol: FW:UPDATE -> wait beacon -> header -> blocks.
// Returns 0 on success; fills msg either way. Requires uartInit done first.
int UARTBridge_PushFirmware(const uint8_t *img, uint32_t len, char *msg, int msgsz) {
	uint8_t hdr[9];
	uint8_t blk[OTA_BLOCK_SIZE];
	uint32_t crc, nblk, k;
	int b, waited, seen;

	if (len == 0 || len > (20u * 1024u)) {
		snprintf(msg, msgsz, "ERR: bad length %u", (unsigned)len);
		return -1;
	}
	crc = ub_crc32(img, len);

	ub_ota_active = 1;
	ub_drain_rx();

	/* 1. tell the running app to hand off to the bootloader */
	ub_send((const uint8_t *)"FW:UPDATE\r\n", 11);

	/* 2. wait for the bootloader 'C' beacon */
	waited = 0; seen = 0;
	while (waited < 5000 && !seen) {
		b = ub_get_byte(200);
		if (b == OTA_BEACON) seen = 1;
		else if (b < 0)      waited += 200;
	}
	if (!seen) { ub_ota_active = 0; snprintf(msg, msgsz, "ERR: no bootloader beacon"); return -1; }
	ub_drain_rx();

	/* 3. header: SOH + len[4 LE] + crc[4 LE] */
	hdr[0] = OTA_SOH;
	hdr[1] = len;        hdr[2] = len >> 8;  hdr[3] = len >> 16;  hdr[4] = len >> 24;
	hdr[5] = crc;        hdr[6] = crc >> 8;  hdr[7] = crc >> 16;  hdr[8] = crc >> 24;
	ub_send(hdr, 9);

	/* 4. ACK after the bootloader erased the app region (can take ~1 s) */
	b = ub_wait_resp(8000);
	if (b != OTA_ACK) { ub_ota_active = 0; snprintf(msg, msgsz, "ERR: header resp %d", b); return -1; }

	/* 5. data blocks, ACK-paced */
	nblk = (len + OTA_BLOCK_SIZE - 1) / OTA_BLOCK_SIZE;
	for (k = 0; k < nblk; k++) {
		uint32_t off = k * OTA_BLOCK_SIZE;
		uint32_t chunk = (off + OTA_BLOCK_SIZE <= len) ? OTA_BLOCK_SIZE : (len - off);
		memset(blk, 0xFF, OTA_BLOCK_SIZE);
		memcpy(blk, img + off, chunk);
		ub_send(blk, OTA_BLOCK_SIZE);
		b = ub_wait_resp(4000);
		if (b != OTA_ACK) { ub_ota_active = 0; snprintf(msg, msgsz, "ERR: block %u resp %d", (unsigned)k, b); return -1; }
	}

	/* 6. final result */
	b = ub_wait_resp(4000);
	ub_ota_active = 0;
	if (b == OTA_DONE_OK) { snprintf(msg, msgsz, "OK: %u bytes, PY32 rebooting", (unsigned)len); return 0; }
	snprintf(msg, msgsz, "ERR: final resp %d", b);
	return -1;
}
