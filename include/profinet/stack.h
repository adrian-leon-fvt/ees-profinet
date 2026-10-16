/**
 * @file stack.h
 * @brief PROFINET raw-socket endpoint with FrameID-based hook dispatch.
 *
 * Linux only (AF_PACKET): not built or installed with PROFINET_POSIX=OFF.
 * On other platforms feed received frames into profinet::Dispatcher.
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

/** @brief Who closes the fd passed to Stack(int, const Mac &, int, ...). */
enum class fd_ownership_t
{
    OWN,   ///< Stack closes the fd on destruction.
    BORROW ///< Caller keeps the fd and closes it after the Stack is gone.
};

/**
 * @brief Binds an AF_PACKET socket to one interface, receives frames and
 *        feeds them to the inherited Dispatcher hooks.
 * @note Stack(interface_name) needs CAP_NET_RAW; an adopted fd needs no
 *       privileges here. Register hooks before calling poll()/run();
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

    /**
     * @brief Use a socket the application opened.
     *
     * Uses: privilege separation (privileged helper opens the raw socket),
     * a socket shared with other code, or unit tests over a socketpair.
     * Nothing is configured on @p fd: enable PACKET_AUXDATA and join
     * multicast groups (join_multicast()) yourself if needed.
     * @param fd AF_PACKET socket, or a connected datagram socket (e.g.
     *           socketpair) delivering whole Ethernet frames. Non-AF_PACKET
     *           sockets transmit with send().
     * @param mac Source MAC reported by mac() and used by send().
     * @param ifindex Interface index for sendto() on AF_PACKET sockets.
     * @param ownership OWN: closed by ~Stack(). BORROW: left open.
     */
    Stack(
        int fd,
        const Mac &mac,
        int ifindex,
        fd_ownership_t ownership = fd_ownership_t::OWN);

    /** @brief Close the socket if owned (drops multicast memberships). */
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

  private:
    int fd_ = -1;
    int ifindex_ = 0;
    bool connected_ = false; ///< Adopted non-AF_PACKET socket: plain send().
    bool owns_fd_ = true;
    Mac mac_{};
    std::atomic<bool> running_{false};
};

} // namespace profinet

#endif // PROFINET_STACK_H
