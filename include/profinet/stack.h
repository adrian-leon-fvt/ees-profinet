/**
 * @file stack.h
 * @brief PROFINET raw-socket endpoint with FrameID-based hook dispatch.
 */
#ifndef PROFINET_STACK_H
#define PROFINET_STACK_H

#include "profinet/frame.h"
#include "profinet/lldp.h"

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace profinet
{

/** @brief Callback invoked for each received frame matching its range. */
using Hook = std::function<void(const FrameView &)>;

/**
 * @brief Binds an AF_PACKET socket to one interface, receives PROFINET
 *        frames and dispatches them to registered hooks.
 * @note Needs CAP_NET_RAW. Register hooks before calling poll()/run();
 *       hook registration is not thread-safe.
 */
class Stack
{
  public:
    /**
     * @brief Open the socket on @p interface_name, enable VLAN recovery,
     *        bind, and join the DCP and LLDP multicast groups.
     * @param interface_name e.g. "eth0".
     * @throws std::system_error (with errno) naming the failed step:
     *         socket, SIOCGIFINDEX, SIOCGIFHWADDR, PACKET_AUXDATA, bind,
     *         PACKET_ADD_MEMBERSHIP.
     */
    explicit Stack(const std::string &interface_name);
    /** @brief Close the socket (drops multicast memberships). */
    virtual ~Stack();
    Stack(const Stack &) = delete;
    Stack &operator=(const Stack &) = delete;

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
     * @brief Ask the NIC to deliver frames sent to multicast MAC @p group.
     * @note The constructor already joins k_multicast_dcp and
     *       k_multicast_lldp. Membership lasts until the Stack is
     *       destroyed; joining twice is harmless.
     * @throws std::system_error on failure.
     */
    void join_multicast(const Mac &group);

    /**
     * @brief Wait up to @p timeout_ms for frames, then dispatch every
     *        queued frame.
     * @param timeout_ms 0 = non-blocking, -1 = forever.
     * @return Number of frames that reached at least one hook's filter
     *         (every PROFINET frame, plus frames of a hooked EtherType);
     *         0 if interrupted by a signal.
     * @throws std::system_error on poll/recvmsg failure; hook exceptions
     *         propagate.
     */
    int poll(int timeout_ms);

    /** @brief `while (!stopped) poll(100);` Blocks the calling thread. */
    void run();

    /**
     * @brief Make run() return within ~100 ms. Thread- and signal-safe.
     * @note No effect if called before run() starts.
     */
    void stop();

    /**
     * @brief build(dst, mac(), id, payload, vlan) + send_raw().
     * @param dst Destination MAC.
     * @param id FrameID.
     * @param payload Bytes after FrameID.
     * @param vlan Optional 802.1Q tag.
     */
    void send(
        const Mac &dst,
        std::uint16_t id,
        Bytes payload,
        const std::optional<Vlan> &vlan = std::nullopt);

    /**
     * @brief Transmit @p frame unchanged. Safe from any thread / hook.
     * @param frame Complete frame from destination MAC (≥ 14 bytes; pad to
     *              60 yourself).
     * @throws std::system_error on send failure (ENETDOWN, EMSGSIZE, ...).
     */
    void send_raw(Bytes frame);

    /** @return MAC of the bound interface, read once at construction. */
    const Mac &mac() const
    {
        return mac_;
    }

    /**
     * @return Socket fd for an external epoll/select loop. Don't read it
     *         yourself; call poll(0) when readable.
     */
    int fd() const
    {
        return fd_;
    }

  protected:
    /**
     * @brief Adopt an already-open socket (testing / privilege separation).
     *
     * Reach it through a subclass. Uses: unit-test hooks over a socketpair
     * (docs/testing.md), or open the raw socket in a privileged helper and
     * pass the fd to an unprivileged process.
     * @param fd AF_PACKET socket, or a connected datagram socket (e.g.
     *           socketpair) delivering whole Ethernet frames; closed on
     *           destruction. Non-AF_PACKET sockets transmit with send().
     * @param mac Source MAC reported by mac() and used by send().
     * @param ifindex Interface index for sendto() on AF_PACKET sockets.
     */
    Stack(int fd, const Mac &mac, int ifindex);

    /**
     * @brief Run matching hooks for one frame. Call it to inject frames
     *        from other sources (pcap replay, simulation).
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

    int fd_ = -1;
    int ifindex_ = 0;
    bool connected_ = false; ///< Adopted non-AF_PACKET socket: plain send().
    Mac mac_{};
    std::vector<Entry> hooks_;
    std::vector<std::pair<std::uint16_t, Hook>> ethertype_hooks_;
    std::atomic<bool> running_{false};
};

} // namespace profinet

#endif // PROFINET_STACK_H
