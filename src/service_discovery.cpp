#include "service_discovery.h"
#include "pingsweep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ctype.h>
#include <string.h>

static uint32_t host_order(const IPAddress &ip)
{
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | ip[3];
}

static void clean_token(char *text)
{
    for (char *p = text; *p; ++p)
        if (!isprint((unsigned char)*p) || *p == ',' || *p == '"') *p = ' ';
}

static uint16_t discover_ssdp()
{
    WiFiUDP udp;
    if (!udp.begin(1901)) return 0;
    const char request[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write((const uint8_t *)request, sizeof(request) - 1);
    udp.endPacket();

    uint16_t replies = 0;
    const uint32_t deadline = millis() + 1600U;
    while ((int32_t)(deadline - millis()) > 0) {
        const int size = udp.parsePacket();
        if (size <= 0) { delay(15); continue; }
        char response[384];
        int got = udp.read((uint8_t *)response, sizeof(response) - 1);
        if (got < 0) continue;
        response[got] = '\0';
        char service[64] = "SSDP/UPnP";
        char *line = strcasestr(response, "\nST:");
        if (!line) line = strcasestr(response, "\nSERVER:");
        if (line) {
            line = strchr(line, ':');
            if (line) {
                ++line;
                while (*line == ' ') ++line;
                size_t n = strcspn(line, "\r\n");
                if (n > sizeof(service) - 1) n = sizeof(service) - 1;
                memcpy(service, line, n); service[n] = '\0';
                clean_token(service);
            }
        }
        pingsweep_add_service_for_ip(host_order(udp.remoteIP()), service);
        ++replies;
    }
    udp.stop();
    return replies;
}

static uint16_t discover_dnssd()
{
    WiFiUDP udp;
    if (!udp.begin(5354)) return 0;
    // DNS PTR query for _services._dns-sd._udp.local.
    static const uint8_t query[] = {
        0x13,0x37, 0x00,0x00, 0x00,0x01, 0,0, 0,0, 0,0,
        0x09,'_','s','e','r','v','i','c','e','s',
        0x07,'_','d','n','s','-','s','d',
        0x04,'_','u','d','p', 0x05,'l','o','c','a','l',0,
        0x00,0x0C, 0x00,0x01
    };
    udp.beginPacket(IPAddress(224, 0, 0, 251), 5353);
    udp.write(query, sizeof(query));
    udp.endPacket();

    uint16_t replies = 0;
    const uint32_t deadline = millis() + 1300U;
    while ((int32_t)(deadline - millis()) > 0) {
        const int size = udp.parsePacket();
        if (size <= 0) { delay(15); continue; }
        uint8_t sink[384];
        udp.read(sink, sizeof(sink));
        pingsweep_add_service_for_ip(host_order(udp.remoteIP()), "DNS-SD responder");
        ++replies;
    }
    udp.stop();
    return replies;
}

uint16_t service_discovery_run()
{
    if (WiFi.status() != WL_CONNECTED) return 0;
    return discover_ssdp() + discover_dnssd();
}
