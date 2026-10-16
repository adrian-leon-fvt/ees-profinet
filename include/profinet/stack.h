/**
 * @file stack.h
 * @brief PROFINET raw-socket endpoint with FrameID-based hook dispatch.
 */
#ifndef PROFINET_STACK_H
#define PROFINET_STACK_H

#include "profinet/dispatcher.h"
#include "profinet/frame.h"
#include "profinet/lldp.h"

#include <atomic>
#include <string>

namespace profinet
{

/**
 * @brief Binds an AF_PACKET socket to one interface, receives frames and
 *        feeds them to the inherited Dispatcher hooks.
 * @note Needs CAP_NET_RAW. Register hooks before calling poll()/run();
 *       hook registration is not thread-safe.
 */
class Stack : public Dispatcher
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

  private:
    int fd_ = -1;
    int ifindex_ = 0;
    bool connected_ = false; ///< Adopted non-AF_PACKET socket: plain send().
    Mac mac_{};
    std::atomic<bool> running_{false};
};

} // namespace profinet

#endif // PROFINET_STACK_H
