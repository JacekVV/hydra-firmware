#pragma once
#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "BaseTelemetryModule.h"
#include "NodeDB.h"
#include "ProtobufModule.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

class DeviceTelemetryModule : private concurrency::OSThread,
                              public BaseTelemetryModule,
                              public ProtobufModule<meshtastic_Telemetry>
{
    CallbackObserver<DeviceTelemetryModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<DeviceTelemetryModule, const meshtastic::Status *>(this, &DeviceTelemetryModule::handleStatusUpdate);

  public:
    DeviceTelemetryModule()
        : concurrency::OSThread("DeviceTelemetry"),
          ProtobufModule("DeviceTelemetry", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg)
    {
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);
        setIntervalFromNow(setStartDelay()); // Wait until NodeInfo is sent
    }
    virtual bool wantUIFrame() { return false; }

  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;
    /**
     * Send our Telemetry into the mesh
     */
    bool sendTelemetry(NodeNum dest = NODENUM_BROADCAST, bool phoneOnly = false);

  private:
    meshtastic_Telemetry getDeviceTelemetry();
    meshtastic_Telemetry getLocalStatsTelemetry();

    void sendLocalStatsToPhone();
    uint32_t sendToPhoneIntervalMs = SECONDS_IN_MINUTE * 1000;           // Send to phone every minute
    uint32_t sendStatsToPhoneIntervalMs = 15 * SECONDS_IN_MINUTE * 1000; // Send stats to phone every 15 minutes
    uint32_t lastSentStatsToPhone = 0;

    // fw+: local_stats_extended to mesh, gated by node_mod_admin.local_stats_extended_over_mesh_enabled
    meshtastic_Telemetry getLocalStatsExtendedTelemetry();
    void sendLocalStatsExtendedToMesh();
    uint32_t sendLocalStatsExtendedIntervalMs = 5 * SECONDS_IN_MINUTE * 1000;
    uint32_t lastSentLocalStatsExtendedToMesh = 0;

    // fw+: rx packet history for local_stats_extended. Index 0 is the open 10-minute bucket, 1..5 are the
    // closed ones, newest first (the same layout the fork uses).
    static const uint8_t RX_HISTORY_BUCKETS = 6;
    static const uint32_t RX_HISTORY_BUCKET_SECS = 10 * SECONDS_IN_MINUTE;
    uint32_t rxPacketHistory[RX_HISTORY_BUCKETS] = {0};
    uint32_t rxPacketsSeen = 0;
    uint32_t rxBucketStartSecs = 0;
    bool rxHistoryStarted = false;
    void updateRxPacketHistory();

    // fw+: local_stats to mesh; local_stats_extended follows it once per cycle
    void sendLocalStatsToMesh();
    uint32_t lastSentLocalStatsToMesh = 0;
    bool localStatsExtendedPending = false;
};