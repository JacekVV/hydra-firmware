#include "FloodingRouter.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "configuration.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include "gps/GeoCoord.h"
#include "modules/TextMessageModule.h"
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif

FloodingRouter::FloodingRouter()
{
    // fw+: profile defaults, timings for the MEDIUM_FAST preset (~200 ms packet time)
    // SPARSE: fast propagation, avoid CAD collisions
    profileParamsSparse.base = 50;
    profileParamsSparse.hop = 20;
    profileParamsSparse.snrGain = 8;
    profileParamsSparse.jitter = 30;
    profileParamsSparse.backboneBias = 0;

    // BALANCED: moderate delay, general purpose
    profileParamsBalanced.base = 120;
    profileParamsBalanced.hop = 40;
    profileParamsBalanced.snrGain = 10;
    profileParamsBalanced.jitter = 60;
    profileParamsBalanced.backboneBias = 0;

    // DENSE: slower propagation, maximize cancel opportunities
    profileParamsDense.base = 200;
    profileParamsDense.hop = 60;
    profileParamsDense.snrGain = 12;
    profileParamsDense.jitter = 100;
    profileParamsDense.backboneBias = 0;

    // BACKBONE_BRIDGE: like BALANCED with an earlier transmission bias
    profileParamsBridge.base = 100;
    profileParamsBridge.hop = 40;
    profileParamsBridge.snrGain = 10;
    profileParamsBridge.jitter = 50;
    profileParamsBridge.backboneBias = 30;
}

/**
 * Send a packet on a suitable interface.  This routine will
 * later free() the packet to pool.  This routine is not allowed to stall.
 * If the txmit queue is full it might return an error
 */
ErrorCode FloodingRouter::send(meshtastic_MeshPacket *p)
{
    flood_counter++;
    // Add any messages _we_ send to the seen message list (so we will ignore all retransmissions we see)
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // First set the relayer to us
    wasSeenRecently(p);                                         // FIXME, move this to a sniffSent method

    return Router::send(p);
}

bool FloodingRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    if (!isToUs(p) && (p->hop_limit <= 0) && !isFromUs(p)) {
        blocked_by_hoplimit++;
    }
    bool wasUpgraded = false;
    bool seenRecently =
        wasSeenRecently(p, true, nullptr, nullptr, &wasUpgraded); // Updates history; returns false when an upgrade is detected

    // Handle hop_limit upgrade scenario for rebroadcasters
    if (wasUpgraded && perhapsHandleUpgradedPacket(p)) {
        return true; // we handled it, so stop processing
    }

    if (!seenRecently && !wasUpgraded && textMessageModule) {
        seenRecently = textMessageModule->recentlySeen(p->id);
    }

    if (seenRecently) {
        printPacket("Ignore dupe incoming msg", p);
        rxDupe++;

        /* If the original transmitter is doing retransmissions (hopStart equals hopLimit) for a reliable transmission, e.g., when
        the ACK got lost, we will handle the packet again to make sure it gets an implicit ACK. */
        bool isRepeated = p->hop_start > 0 && p->hop_start == p->hop_limit;
        if (isRepeated) {
            LOG_DEBUG("Repeated reliable tx");
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id)) {
                if (reprocessPacket(p))
                    perhapsRebroadcast(p);
            }
        } else {
            perhapsCancelDupe(p);
        }

        return true;
    }

    return Router::shouldFilterReceived(p);
}

bool FloodingRouter::perhapsHandleUpgradedPacket(const meshtastic_MeshPacket *p)
{
    // We never relay our own packets: consume an upgraded copy with our number without touching the TX
    // queue, rather than let it fall through to the dupe handling (perhapsCancelDupe) as well.
    if (isFromUs(p))
        return true;

    // isRebroadcaster() is duplicated in perhapsRebroadcast(), but this avoids confusing log messages
    if (isRebroadcaster() && iface && p->hop_limit > 0) {
        // Verify the replacement before deleting the valid lower-hop copy waiting in the TX queue.
        // This is intentionally redundant with ReliableRouter's ingress gate: it keeps this helper
        // safe if another caller is introduced later.
        if (passesRoutingAuthGate(const_cast<meshtastic_MeshPacket *>(p)) != RoutingAuthVerdict::ACCEPT)
            return true;

        // If we overhear a duplicate copy of the packet with more hops left than the one we are waiting to
        // rebroadcast, then remove the packet currently sitting in the TX queue and use this one instead.
        uint8_t dropThreshold = p->hop_limit; // remove queued packets that have fewer hops remaining
        if (iface->removePendingTXPacket(getFrom(p), p->id, dropThreshold)) {
            LOG_DEBUG("Processing upgraded packet 0x%08x for rebroadcast with hop limit %d (dropping queued < %d)", p->id,
                      p->hop_limit, dropThreshold);

            if (!reprocessPacket(p))
                return true;
            perhapsRebroadcast(p);

            rxDupe++;
            // We already enqueued the improved copy, so make sure the incoming packet stops here.
            return true;
        }
    }

    return false;
}

bool FloodingRouter::reprocessPacket(const meshtastic_MeshPacket *p)
{
    // Reprocessing a dupe is for relaying it; our own packets are never relayed, and their implicit ACK
    // is generated in ReliableRouter::shouldFilterReceived before any dupe handling.
    if (isFromUs(p))
        return false;

    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        auto decodedState = perhapsDecode(const_cast<meshtastic_MeshPacket *>(p));
        if (decodedState != DecodeState::DECODE_SUCCESS && decodedState != DecodeState::DECODE_OPAQUE)
            return false;
    }

    if (nodeDB)
        nodeDB->updateFrom(*p);

#if !MESHTASTIC_EXCLUDE_TRACEROUTE
    if (traceRouteModule && p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
        traceRouteModule->processUpgradedPacket(*p);
    }
#endif
    return true;
}

bool FloodingRouter::roleAllowsCancelingDupe(const meshtastic_MeshPacket *p)
{
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
        config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE) {
        // ROUTER, ROUTER_LATE should never cancel relaying a packet (i.e. we should always rebroadcast),
        // even if we've heard another station rebroadcast it already.
        return false;
    }

    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
        // CLIENT_BASE: if the packet is from or to a favorited node,
        // we should act like a ROUTER and should never cancel a rebroadcast (i.e. we should always rebroadcast),
        // even if we've heard another station rebroadcast it already.
        return !nodeDB->isFromOrToFavoritedNode(*p);
    }

    // All other roles (such as CLIENT) should cancel a rebroadcast if they hear another station's rebroadcast.
    return true;
}

void FloodingRouter::perhapsCancelDupe(const meshtastic_MeshPacket *p)
{
    // fw+: prefer opportunistic cancel on first hear; fall back to the role-based cancel
    if (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA) {
        if (isOpportunisticEnabled() && moduleConfig.node_mod_admin.opportunistic_cancel_on_first_hear) {
            if (Router::cancelSending(p->from, p->id)) {
                txRelayCanceled++;
                opportunistic_canceled++;
            }
        } else if (roleAllowsCancelingDupe(p)) {
            // cancel rebroadcast of this message *if* there was already one, unless we're a router!
            // But only LoRa packets should be able to trigger this.
            if (Router::cancelSending(p->from, p->id))
                txRelayCanceled++;
        }
    }
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE && iface) {
        iface->clampToLateRebroadcastWindow(getFrom(p), p->id);
    }
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE && iface && nodeDB &&
        nodeDB->isFromOrToFavoritedNode(*p)) {
        iface->clampToLateRebroadcastWindow(getFrom(p), p->id);
    }
}

bool FloodingRouter::isRebroadcaster()
{
    return config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE &&
           config.device.rebroadcast_mode != meshtastic_Config_DeviceConfig_RebroadcastMode_NONE;
}

void FloodingRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
                        (p->decoded.request_id != 0 || p->decoded.reply_id != 0);
    if (isAckorReply && !isToUs(p) && !isBroadcast(p->to)) {
        // do not flood direct message that is ACKed or replied to
        LOG_DEBUG("Rxd an ACK/reply not for me, cancel rebroadcast");
        Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
    }

    perhapsRebroadcast(p);

    // handle the packet as normal
    Router::sniffReceived(p, c);

    // fw+: observe RX and recompute the opportunistic profile once per window
    observeRxForProfile(p);
    maybeRecomputeProfile(millis());
}

// fw+: opportunistic flooding helpers

void FloodingRouter::observeRxForProfile(const meshtastic_MeshPacket *p)
{
    if (profileWindowStartMs == 0) {
        profileWindowStartMs = millis();
        lastRxDupeCounter = rxDupe;
        chanUtilEma = airTime->channelUtilizationPercent();
    }

    // non-duplicate broadcasts in this window
    if (isBroadcast(p->to))
        windowBroadcastNonDup++;

    // smooth channel utilization
    const float alpha = 0.2f;
    chanUtilEma = (1.0f - alpha) * chanUtilEma + alpha * airTime->channelUtilizationPercent();
}

void FloodingRouter::maybeRecomputeProfile(uint32_t nowMs)
{
    if (profileWindowStartMs == 0)
        return;
    if (nowMs - profileWindowStartMs < profileWindowMs)
        return;

    uint32_t dupes = rxDupe - lastRxDupeCounter;
    float dupeRatio = 0.0f;
    if (windowBroadcastNonDup > 0)
        dupeRatio = (float)dupes / (float)windowBroadcastNonDup;

    // estimated 1-hop neighbors (not via MQTT)
    neighborsWin = 0;
    const size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total; ++i) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n)
            continue;
        if (n->hops_away == 0 && !nodeInfoLiteViaMqtt(n) && n->num != nodeDB->getNodeNum())
            neighborsWin++;
    }

    OpportunisticProfile newTarget = OpportunisticProfile::BALANCED;
    if (hasBackboneNeighbor()) {
        newTarget = OpportunisticProfile::BACKBONE_BRIDGE;
    } else if (neighborsWin <= 2 && dupeRatio < 0.10f && chanUtilEma < 15.0f) {
        newTarget = OpportunisticProfile::SPARSE;
    } else if (neighborsWin >= 8 || dupeRatio >= 0.30f || chanUtilEma >= 40.0f) {
        newTarget = OpportunisticProfile::DENSE;
    }

    if (newTarget == targetProfile) {
        stableWindows++;
    } else {
        targetProfile = newTarget;
        stableWindows = 0;
    }

    if (stableWindows >= requiredStableWindows)
        currentProfile = targetProfile;

    profileWindowStartMs = nowMs;
    lastRxDupeCounter = rxDupe;
    windowBroadcastNonDup = 0;
}

const FloodingRouter::ProfileParams &FloodingRouter::getParamsFor(OpportunisticProfile p) const
{
    switch (p) {
    case OpportunisticProfile::SPARSE:
        return profileParamsSparse;
    case OpportunisticProfile::DENSE:
        return profileParamsDense;
    case OpportunisticProfile::BACKBONE_BRIDGE:
        return profileParamsBridge;
    case OpportunisticProfile::BALANCED:
    default:
        return profileParamsBalanced;
    }
}

bool FloodingRouter::isOpportunisticEnabled() const
{
    if (!moduleConfig.has_node_mod_admin)
        return false;
    return moduleConfig.node_mod_admin.opportunistic_flooding_enabled;
}

bool FloodingRouter::isOpportunisticAuto() const
{
    if (!moduleConfig.has_node_mod_admin)
        return true;
    return moduleConfig.node_mod_admin.opportunistic_auto;
}

uint32_t FloodingRouter::clampDelay(uint32_t d) const
{
    // D_MIN must exceed the CAD time (~15-20 ms) to avoid collisions
    // D_MAX is about 2.5x packet time for the worst-case dense network
    const uint32_t D_MIN = 25;
    const uint32_t D_MAX = 500;
    if (d < D_MIN)
        return D_MIN;
    if (d > D_MAX)
        return D_MAX;
    return d;
}

uint32_t FloodingRouter::computeOpportunisticDelayMs(const meshtastic_MeshPacket *p) const
{
    if (!moduleConfig.has_node_mod_admin)
        return 0;

    const auto &adm = moduleConfig.node_mod_admin;
    const uint32_t base = adm.opportunistic_base_delay_ms;
    const uint32_t hop = adm.opportunistic_hop_delay_ms;
    const uint32_t snrGain = adm.opportunistic_snr_gain_ms;
    const uint32_t jitter = adm.opportunistic_jitter_ms;

    if (base == 0 && hop == 0 && snrGain == 0 && jitter == 0)
        return 0;

    // hops already used, from hop_start and hop_limit
    int32_t usedHops = 0;
    if (p->hop_start > 0) {
        usedHops = (int32_t)p->hop_start - (int32_t)p->hop_limit;
        if (usedHops < 0)
            usedHops = 0;
    }

    // SNR scales the delay only when positive, clamped to [0, 20]
    float snr = p->rx_snr;
    if (snr < 0)
        snr = 0;
    if (snr > 20)
        snr = 20;

    const uint32_t rj = jitter ? random(jitter + 1) : 0;

    uint16_t pBase;
    uint16_t pHop;
    uint8_t pSnr;
    int32_t backboneBias = 0;
    if (isOpportunisticAuto()) {
        const ProfileParams &pp = getParamsFor(currentProfile);
        pBase = pp.base;
        pHop = pp.hop;
        pSnr = pp.snrGain;
        backboneBias = (int32_t)pp.backboneBias;
    } else {
        // manual override from the admin config, with fallbacks for unset values
        pBase = base ? base : 120;
        pHop = hop ? hop : 40;
        pSnr = snrGain ? (uint8_t)snrGain : 10;
    }

    // overflow-safe: clamp each intermediate term
    int32_t hopDelay = (int32_t)(pHop * usedHops);
    if (hopDelay > 5000)
        hopDelay = 5000;
    int32_t snrReduction = (int32_t)(pSnr * (int32_t)snr);
    if (snrReduction > 5000)
        snrReduction = 5000;

    int32_t d = (int32_t)pBase + hopDelay - snrReduction + (int32_t)rj - backboneBias;
    if (d <= 0)
        return (uint32_t)rj; // pure jitter, never negative
    return clampDelay((uint32_t)d);
}

bool FloodingRouter::hasBackboneNeighbor() const
{
    // heuristic: a direct neighbor more than 10 km away means we are a backbone bridge.
    // Needs both positions; without them fail open (false).
    const meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!self || !nodeDB->hasValidPosition(self))
        return false;

    meshtastic_PositionLite selfPos;
    if (!nodeDB->copyNodePosition(self->num, selfPos))
        return false;
    const double selfLat = selfPos.latitude_i * 1e-7;
    const double selfLon = selfPos.longitude_i * 1e-7;
    const float thresholdMeters = 10000.0f;

    const size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total; ++i) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == self->num)
            continue;
        if (n->hops_away != 0 || nodeInfoLiteViaMqtt(n))
            continue;

        meshtastic_PositionLite nPos;
        if (!nodeDB->copyNodePosition(n->num, nPos) || (nPos.latitude_i == 0 && nPos.longitude_i == 0))
            continue;

        const double nLat = nPos.latitude_i * 1e-7;
        const double nLon = nPos.longitude_i * 1e-7;
        if (GeoCoord::latLongToMeter(selfLat, selfLon, nLat, nLon) >= thresholdMeters)
            return true;
    }
    return false;
}
