#pragma once
#include "NodeDB.h"
#include <array>
#include <deque>
#include <stddef.h>
#include <stdint.h>

#define RXTXALL_ACTIVITY_COUNT 40

struct RxTxAllCounter {
    uint32_t rxTxAll_counter;
};

struct ExchangeEntry {
    uint32_t from_node;
    uint32_t to_node;
    meshtastic_PortNum port_num;
};
struct ExchangeList {
    std::array<ExchangeEntry, 20> entries; 
    size_t count = 0; 

    void addEntry(const ExchangeEntry &entry) {
        if(entry.from_node == entry.to_node){
            return;
        }
        
        if (count < entries.size()) {
            for (size_t i = count; i > 0; --i) {
                entries[i] = entries[i - 1];
            }
            entries[0] = entry;
            ++count;
        } else {
            for (size_t i = entries.size() - 1; i > 0; --i) {
                entries[i] = entries[i - 1];
            }
            entries[0] = entry;
        }
    }
};

class PacketCounter
{
public:
    PacketCounter() = default;

    int currentBucketIndex = 0;
    uint64_t bucketStartMs = 0;
    uint32_t currentBucketCount = 0;

    // Packets per 10-minute bucket, newest first (OnDemand and telemetry rx_packet_history)
    RxTxAllCounter rxTxAllActivities[RXTXALL_ACTIVITY_COUNT] = {};
    uint32_t rxTxAllActivitiesCount = 0;
    uint32_t rxPacketBucketPartial = 0;
    float rx_avg_60_min = 0;
    // Recent packets (from, to, portnum), newest first (OnDemand exchange log)
    ExchangeList packetHistoryLog;

    void onPacketReceived(const meshtastic_MeshPacket *p);
    void addRxPacketHistory(meshtastic_ModuleConfig_NodeModAdminConfig &admin, uint64_t timestamp);
    uint32_t getAvgLast60Min();
    size_t getCountLast10Min(uint64_t currentUptimeMs) const;

private:
    uint64_t getMonotonicUptimeMs();
    std::deque<uint64_t> m_timestamps;
};
