#include "HydraLog.h"

#if defined(ARCH_ESP32)
#include "NodeDB.h"
#include "configuration.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

namespace
{
constexpr uint32_t BOX_MAGIC = 0x48594C47; // "HYLG"
constexpr size_t BOX_SIZE = 6144; // RTC slow memory has ~7.5 KB for the app
struct BlackBox {
    uint32_t magic;
    uint32_t head; // next write position
    uint32_t used; // bytes filled, up to BOX_SIZE
    char buf[BOX_SIZE];
};
RTC_NOINIT_ATTR BlackBox box; // not cleared on reset; validated by magic and bounds

constexpr size_t SEND_SIZE = 1360; // + header and drop notice stays under WiFiUDP's 1460-byte packet buffer
constexpr uint32_t SEND_EVERY_MS = 500;
char sendBuf[2][SEND_SIZE];
size_t sendLen[2];
int active = 0;
uint32_t dropped = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

bool inited = false;
char *prev = nullptr; // previous boot's black box, oldest line first; sent once, then freed
size_t prevLen = 0;
esp_reset_reason_t prevReason = ESP_RST_UNKNOWN;

bool sendEnabled = false;
int minRank = 1; // 0 DEBUG, 1 INFO, 2 WARN, 3 ERROR, 4 CRITICAL
IPAddress serverIp;
uint16_t serverPort = 0;
WiFiUDP udp;

const char *const REASONS[] = {"UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO"};

int rankOf(const char *logLevel)
{
    switch (logLevel ? logLevel[0] : 0) {
    case 'D':
        return 0;
    case 'I':
        return 1;
    case 'W':
        return 2;
    case 'E':
        return 3;
    case 'C':
        return 4;
    default:
        return 0; // TRACE and unknown count as DEBUG
    }
}

void boxInit()
{
    inited = true;
    if (box.magic == BOX_MAGIC && box.head < BOX_SIZE && box.used > 0 && box.used <= BOX_SIZE) {
        prev = (char *)malloc(box.used);
        if (prev) {
            size_t start = (box.head + BOX_SIZE - box.used) % BOX_SIZE;
            for (size_t i = 0; i < box.used; i++)
                prev[i] = box.buf[(start + i) % BOX_SIZE];
            prevLen = box.used;
            if (box.used == BOX_SIZE) { // the oldest line was partly overwritten: drop it
                char *nl = (char *)memchr(prev, '\n', prevLen);
                if (nl) {
                    size_t skip = nl + 1 - prev;
                    memmove(prev, nl + 1, prevLen - skip);
                    prevLen -= skip;
                }
            }
            prevReason = esp_reset_reason();
        }
    }
    box.magic = BOX_MAGIC;
    box.head = 0;
    box.used = 0;
}

void boxWrite(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        box.buf[box.head] = s[i];
        box.head = (box.head + 1) % BOX_SIZE;
    }
    box.used = box.used + n > BOX_SIZE ? BOX_SIZE : box.used + n;
}

/// network.rsyslog_server = "IP[:port][:debug]"; empty = nothing sent (the black box still records).
void readConfig()
{
    char s[sizeof(config.network.rsyslog_server)];
    strncpy(s, config.network.rsyslog_server, sizeof(s) - 1);
    s[sizeof(s) - 1] = 0;
    minRank = strstr(s, ":debug") ? 0 : 1;
    uint16_t port = 514;
    char *colon = strchr(s, ':');
    if (colon) {
        *colon = 0;
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536)
            port = p;
    }
    IPAddress ip;
    sendEnabled = s[0] && ip.fromString(s);
    serverIp = ip;
    serverPort = port;
}

void sendPrev()
{
    const char *reason = (unsigned)prevReason < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[prevReason] : "?";
    size_t pos = 0;
    while (pos < prevLen) {
        size_t n = prevLen - pos < SEND_SIZE ? prevLen - pos : SEND_SIZE;
        if (pos + n < prevLen) { // cut at a line end so lines are not split across datagrams
            size_t cut = n;
            while (cut > 0 && prev[pos + cut - 1] != '\n')
                cut--;
            if (cut > 0)
                n = cut;
        }
        udp.beginPacket(serverIp, serverPort);
        udp.printf("HYDRALOG 1 prev %s %d %d\n", reason, pos == 0, pos + n >= prevLen); // first / last part
        udp.write((const uint8_t *)prev + pos, n);
        udp.endPacket();
        pos += n;
    }
    free(prev);
    prev = nullptr;
    prevLen = 0;
}
} // namespace

void hydraLogLine(const char *logLevel, const char *thread, const char *format, va_list arg)
{
    if (!inited)
        boxInit();
    char line[200];
    int n = snprintf(line, sizeof(line), "%lu %c [%s] ", (unsigned long)(millis() / 1000), logLevel ? logLevel[0] : '?',
                     thread ? thread : "-");
    if (n < 0 || n >= (int)sizeof(line) - 2)
        return;
    int m = vsnprintf(line + n, sizeof(line) - n, format, arg);
    size_t len = m < 0 ? n : (size_t)(n + m) < sizeof(line) - 1 ? n + m : sizeof(line) - 1;
    while (len > (size_t)n && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    for (size_t i = n; i < len; i++)
        if (line[i] == '\n' || line[i] == '\r')
            line[i] = ' ';
    line[len++] = '\n';

    boxWrite(line, len);

    if (!sendEnabled || rankOf(logLevel) < minRank)
        return;
    portENTER_CRITICAL(&mux);
    // v23: when full, drop the oldest lines, not the newest: after a long Wi-Fi outage the line that matters is the
    // health report logged right after reconnecting, not the boot chatter queued before it
    char *buf = sendBuf[active];
    while (sendLen[active] + len > SEND_SIZE && sendLen[active] > 0) {
        char *nl = (char *)memchr(buf, '\n', sendLen[active]);
        size_t cut = nl ? (size_t)(nl + 1 - buf) : sendLen[active];
        memmove(buf, buf + cut, sendLen[active] - cut);
        sendLen[active] -= cut;
        dropped++;
    }
    memcpy(buf + sendLen[active], line, len);
    sendLen[active] += len;
    portEXIT_CRITICAL(&mux);
}

void hydraLogTick()
{
    static uint32_t lastCfg = 0, lastSend = 0;
    uint32_t now = millis();
    if (now - lastCfg >= 1000 || lastCfg == 0) {
        lastCfg = now;
        readConfig();
    }
    // send every 500 ms, or sooner once half the buffer is used (bursts would otherwise overflow it)
    if (!sendEnabled || (now - lastSend < SEND_EVERY_MS && sendLen[active] < SEND_SIZE / 2) || WiFi.status() != WL_CONNECTED)
        return;
    lastSend = now;

    if (prev)
        sendPrev();

    portENTER_CRITICAL(&mux);
    int full = active;
    active ^= 1;
    sendLen[active] = 0;
    uint32_t lost = dropped;
    dropped = 0;
    portEXIT_CRITICAL(&mux);

    if (sendLen[full] == 0 && lost == 0)
        return;
    udp.beginPacket(serverIp, serverPort);
    udp.print("HYDRALOG 1 live\n");
    if (lost)
        udp.printf("%lu W [hydralog] %lu lines dropped (buffer full)\n", (unsigned long)(now / 1000), (unsigned long)lost);
    udp.write((const uint8_t *)sendBuf[full], sendLen[full]);
    udp.endPacket();
}
#endif
