/**
 * @file dispatcher.h
 * @brief Socket-free hook registry: feed it frames from any source (your own
 *        socket, pcap, XDP, replay) and it calls the matching hooks.
 */
#ifndef PROFINET_DISPATCHER_H
#define PROFINET_DISPATCHER_H

#include "profinet/frame.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace profinet
{

/** @brief Callback invoked for each received frame matching its filter. */
using Hook = std::function<void(const FrameView &)>;

/**
 * @brief FrameID / EtherType hook dispatch without I/O.
 *
 * Stack is a Dispatcher with an AF_PACKET socket attached. Use Dispatcher
 * alone when the application owns the socket or capture.
 * @note Registration is not thread-safe: register before feeding frames.
 */
class Dispatcher
{
  public:
    virtual ~Dispatcher() = default;

    /**
     * @name Hook registration
     * `on` / `on_any` see PROFINET (0x8892) frames only. Every matching hook
     * runs, in registration order. No removal: check a flag in the hook.
     */
    ///@{

    /** @brief Call @p hook for frames with FrameID in [first, last]. */
    void on(std::uint16_t first, std::uint16_t last, Hook hook);

    /** @brief Call @p hook for frames with exactly FrameID @p id. */
    void on(std::uint16_t id, Hook hook);

    /** @brief Call @p hook for every PROFINET frame. */
    void on_any(Hook hook);

    /**
     * @brief Call @p hook for every frame with EtherType @p ethertype
     *        (e.g. k_ethertype_lldp). FrameView::payload starts after the
     *        EtherType and frame_id is 0. For 0x8892 the view is the
     *        PROFINET one (payload after FrameID).
     */
    void on_ethertype(std::uint16_t ethertype, Hook hook);
    ///@}

    /**
     * @brief Parse one raw Ethernet frame and run the matching hooks.
     * @param raw Frame from destination MAC, without FCS. Only needs to
     *            live for the duration of the call.
     * @param vlan Tag recovered out of band (e.g. PACKET_AUXDATA or pcap
     *             metadata when the NIC strips it); overrides an in-band tag.
     * @return true if the frame is PROFINET or matched an EtherType hook;
     *         false for runts and truncated PROFINET frames.
     */
    bool feed(Bytes raw, const std::optional<Vlan> &vlan = std::nullopt) const;

    /**
     * @brief Run matching hooks for an already-parsed frame.
     * @return true if the frame is PROFINET or matched an EtherType hook.
     */
    bool dispatch(const FrameView &frame) const;

  private:
    struct Entry
    {
        std::uint16_t first;
        std::uint16_t last;
        Hook hook;
    };

    std::vector<Entry> hooks_;
    std::vector<std::pair<std::uint16_t, Hook>> ethertype_hooks_;
};

} // namespace profinet

#endif // PROFINET_DISPATCHER_H
