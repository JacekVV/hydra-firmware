#pragma once

#include "Router.h"

/**
 * This is a mixin that extends Router with the ability to do Naive Flooding (in the standard mesh protocol sense)
 *
 *   Rules for broadcasting (listing here for now, will move elsewhere eventually):

  If to==BROADCAST and id==0, this is a simple broadcast (0 hops).  It will be
  sent only by the current node and other nodes will not attempt to rebroadcast
  it.

  If to==BROADCAST and id!=0, this is a "naive flooding" broadcast.  The initial
  node will send it on all local interfaces.

  When other nodes receive this message, they will
  first check if their recentBroadcasts table contains the (from, id) pair that
  indicates this message.  If so, we've already seen it - so we discard it.  If
  not, we add it to the table and then resend this message on all interfaces.
  When resending we are careful to use the "from" ID of the original sender. Not
  our own ID.  When resending we pick a random delay between 0 and 10 seconds to
  decrease the chance of collisions with transmitters we can not even hear.

  Any entries in recentBroadcasts that are older than X seconds (longer than the
  max time a flood can take) will be discarded.
 */
class FloodingRouter : public Router
{
  public:
    /**
     * Constructor
     *
     */
    FloodingRouter();

    /**
     * Send a packet on a suitable interface.  This routine will
     * later free() the packet to pool.  This routine is not allowed to stall.
     * If the txmit queue is full it might return an error
     */
    virtual ErrorCode send(meshtastic_MeshPacket *p) override;

  protected:
    /**
     * Should this incoming filter be dropped?
     *
     * Called immediately on reception, before any further processing.
     * @return true to abandon the packet
     */
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *p) override;

    /**
     * Look for broadcasts we need to rebroadcast
     */
    virtual void sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c) override;

    /* Check if we should rebroadcast this packet, and do so if needed */
    virtual bool perhapsRebroadcast(const meshtastic_MeshPacket *p) = 0;

    /* Check if we should handle an upgraded packet (with higher hop_limit)
     * @return true if we handled it (so stop processing)
     */
    bool perhapsHandleUpgradedPacket(const meshtastic_MeshPacket *p);

    /* Call when we receive a packet that needs some reprocessing, but afterwards should be filtered */
    bool reprocessPacket(const meshtastic_MeshPacket *p);

    // Return false for roles like ROUTER which should always rebroadcast even when we've heard another rebroadcast of
    // the same packet
    bool roleAllowsCancelingDupe(const meshtastic_MeshPacket *p);

    /* Call when receiving a duplicate packet to check whether we should cancel a packet in the Tx queue */
    void perhapsCancelDupe(const meshtastic_MeshPacket *p);

    // Return true if we are a rebroadcaster
    bool isRebroadcaster();

    // fw+ Opportunistic/selective flooding: admin-config driven delay, cancel on first hear, adaptive profile
  public:
    enum class OpportunisticProfile : uint8_t { SPARSE = 0, BALANCED = 1, DENSE = 2, BACKBONE_BRIDGE = 3 };

    struct ProfileParams {
        uint16_t base = 60;
        uint16_t hop = 30;
        uint8_t snrGain = 8;
        uint16_t jitter = 40;
        uint8_t backboneBias = 0; // extra earlier ms if backbone
    };

    // Expose opportunistic info for OnDemand
    bool getOpportunisticEnabled() const override { return isOpportunisticEnabled(); }
    uint32_t getOpportunisticProfile() const override { return (uint32_t)currentProfile; }

  protected:
    bool isOpportunisticEnabled() const;
    bool isOpportunisticAuto() const;
    uint32_t computeOpportunisticDelayMs(const meshtastic_MeshPacket *p) const;
    uint32_t clampDelay(uint32_t d) const;
    bool hasBackboneNeighbor() const;
    void observeRxForProfile(const meshtastic_MeshPacket *p);
    void maybeRecomputeProfile(uint32_t nowMs);
    const ProfileParams &getParamsFor(OpportunisticProfile p) const;

    uint32_t opportunistic_scheduled = 0;
    uint32_t opportunistic_canceled = 0;

  private:
    OpportunisticProfile currentProfile = OpportunisticProfile::BALANCED;
    OpportunisticProfile targetProfile = OpportunisticProfile::BALANCED;
    uint32_t profileWindowStartMs = 0;
    uint32_t profileWindowMs = 60000; // 60 s
    uint8_t stableWindows = 0;
    uint8_t requiredStableWindows = 2;

    uint16_t windowBroadcastNonDup = 0; // non-duplicate broadcasts seen in window
    uint32_t lastRxDupeCounter = 0;     // snapshot of rxDupe at window start
    uint8_t neighborsWin = 0;           // estimated local neighbor count
    float chanUtilEma = 0.0f;           // EMA of channel utilization

    ProfileParams profileParamsSparse;
    ProfileParams profileParamsBalanced;
    ProfileParams profileParamsDense;
    ProfileParams profileParamsBridge;
};
