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
     * @brief Open the socket on @p interface_name.
     * @throws std::system_error on socket/ioctl/bind failure.
     */
    explicit Stack(const std::string &interface_name);
    virtual ~Stack();
    Stack(const Stack &) = delete;
    Stack &operator=(const Stack &) = delete;

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

    /**
     * @brief Ask the NIC to deliver frames sent to multicast MAC @p group.
     * @note The constructor already joins k_multicast_dcp and
     *       k_multicast_lldp. Membership lasts
     *       until the Stack is destroyed.
     * @throws std::system_error on failure.
     */
    void join_multicast(const Mac &group);

    /**
     * @brief Wait up to @p timeout_ms for frames and dispatch them.
     * @return Number of frames that reached at least one hook's filter
     *         (every PROFINET frame, plus frames of a hooked EtherType).
     */
    int poll(int timeout_ms);

    /** @brief Loop poll() until stop() is called (from any thread). */
    void run();

    /** @brief Make run() return. */
    void stop();

    /** @brief Send a PROFINET frame from this interface's MAC. */
    void send(
        const Mac &dst,
        std::uint16_t id,
        Bytes payload,
        const std::optional<Vlan> &vlan = std::nullopt);

    /** @brief Send a complete pre-built Ethernet frame. */
    void send_raw(Bytes frame);

    /** @return MAC address of the bound interface. */
    const Mac &mac() const
    {
        return mac_;
    }

    /** @return Socket fd, e.g. for an external epoll loop + poll(0). */
    int fd() const
    {
        return fd_;
    }

  protected:
    /**
     * @brief Adopt an already-open socket (testing / privilege separation).
     * @param fd AF_PACKET socket, or a connected datagram socket (e.g.
     *           socketpair) delivering whole Ethernet frames; closed on
     *           destruction. Non-AF_PACKET sockets transmit with send().
     */
    Stack(int fd, const Mac &mac, int ifindex);

    /**
     * @brief Run matching hooks for one received frame.
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
