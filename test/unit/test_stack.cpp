/**
 * @file test_stack.cpp
 * @brief Unit tests for profinet::Stack hook dispatch and receive loop.
 *
 * A SOCK_DGRAM socketpair stands in for the AF_PACKET socket, so no
 * privileges or network interfaces are needed.
 */
#include "profinet/stack.h"

#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace profinet;

namespace
{
const Mac k_dst{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};
const Mac k_src{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

class StackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), 0);
        stack_ = std::make_unique<Stack>(fds[0], k_src, 1);
        peer_ = fds[1];
    }

    void TearDown() override
    {
        ::close(peer_);
    }

    void inject(const std::uint16_t id, const std::optional<Vlan> &vlan = {})
    {
        const std::vector<std::uint8_t> payload(44, 0x11);
        inject_raw(build(k_dst, k_src, id, payload, vlan));
    }

    void inject_raw(const std::vector<std::uint8_t> &frame)
    {
        ASSERT_EQ(
            ::send(peer_, frame.data(), frame.size(), 0),
            static_cast<ssize_t>(frame.size()));
    }

    std::unique_ptr<Stack> stack_;
    int peer_ = -1;
};
} // namespace

TEST_F(StackTest, PollDispatchesToMatchingRangeOnly)
{
    std::vector<std::uint16_t> rt, dcp;
    stack_->on(
        frame_id::k_rt_class1_first,
        frame_id::k_rt_class1_last,
        [&](const FrameView &f) { rt.push_back(f.frame_id); });
    stack_->on(
        frame_id::k_dcp_hello,
        frame_id::k_dcp_identify_rsp,
        [&](const FrameView &f) { dcp.push_back(f.frame_id); });
    inject(0xC001);
    inject(frame_id::k_dcp_identify_req);
    inject(0x8000);

    const int handled = stack_->poll(100);

    EXPECT_EQ(handled, 3);
    EXPECT_EQ(rt, std::vector<std::uint16_t>{0xC001});
    EXPECT_EQ(dcp, std::vector<std::uint16_t>{frame_id::k_dcp_identify_req});
}

TEST_F(StackTest, ExactIdAndAnyHooksBothFireInRegistrationOrder)
{
    std::vector<int> order;
    stack_->on(0xC001, [&](const FrameView &) { order.push_back(1); });
    stack_->on_any([&](const FrameView &) { order.push_back(2); });
    stack_->on(0xC002, [&](const FrameView &) { order.push_back(3); });
    inject(0xC001);

    stack_->poll(100);

    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST_F(StackTest, NonProfinetFramesAreDroppedSilently)
{
    int calls = 0;
    stack_->on_any([&](const FrameView &) { ++calls; });
    std::vector<std::uint8_t> ipv4(60, 0);
    ipv4[12] = 0x08;
    inject_raw(ipv4);

    EXPECT_EQ(stack_->poll(100), 0);
    EXPECT_EQ(calls, 0);
}

TEST_F(StackTest, InBandVlanTagIsReportedToHook)
{
    std::optional<Vlan> seen;
    stack_->on_any([&](const FrameView &f) { seen = f.vlan; });
    inject(0xC001, Vlan{5, 42});

    stack_->poll(100);

    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->pcp, 5);
    EXPECT_EQ(seen->vid, 42);
}

TEST_F(StackTest, PollTimesOutWithNothingPending)
{
    EXPECT_EQ(stack_->poll(0), 0);
}

TEST_F(StackTest, SendOnAdoptedSocketReachesPeerWithOwnMacAsSource)
{
    const std::uint8_t payload[] = {1, 2, 3};

    stack_->send(k_dst, frame_id::k_dcp_identify_rsp, payload);

    std::uint8_t buf[128];
    const ssize_t n = ::recv(peer_, buf, sizeof(buf), MSG_DONTWAIT);
    ASSERT_EQ(n, 60);
    const auto f = parse(Bytes(buf, static_cast<std::size_t>(n)));
    ASSERT_TRUE(f);
    EXPECT_EQ(f->src, k_src);
    EXPECT_EQ(f->dst, k_dst);
    EXPECT_EQ(f->frame_id, frame_id::k_dcp_identify_rsp);
    EXPECT_EQ(f->payload[2], 3);
}

TEST_F(StackTest, EthertypeHookGetsLldpAndFrameIdHooksDoNot)
{
    std::optional<Lldp> lldp;
    int pn_calls = 0;
    stack_->on_any([&](const FrameView &) { ++pn_calls; });
    stack_->on_ethertype(k_ethertype_lldp, [&](const FrameView &f) {
        lldp = parse_lldp(f);
    });
    inject_raw(build_lldp(k_dst, "dev-7", "port-002"));

    EXPECT_EQ(stack_->poll(100), 1);

    EXPECT_EQ(pn_calls, 0);
    ASSERT_TRUE(lldp);
    EXPECT_EQ(lldp->chassis_id, "dev-7");
}

TEST_F(StackTest, EthertypeHookOnProfinetSeesProfinetView)
{
    std::uint16_t id = 0;
    stack_->on_ethertype(k_ethertype, [&](const FrameView &f) { id = f.frame_id; });
    inject(0xC001);

    stack_->poll(100);

    EXPECT_EQ(id, 0xC001);
}

TEST_F(StackTest, TruncatedProfinetFrameIsDropped)
{
    int calls = 0;
    stack_->on_any([&](const FrameView &) { ++calls; });
    stack_->on_ethertype(k_ethertype, [&](const FrameView &) { ++calls; });
    const std::vector<std::uint8_t> runt{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x88, 0x92, 0xC0};
    inject_raw(runt);

    EXPECT_EQ(stack_->poll(100), 0);
    EXPECT_EQ(calls, 0);
}

TEST_F(StackTest, StopFromHookEndsRun)
{
    int calls = 0;
    stack_->on_any([&](const FrameView &) {
        ++calls;
        stack_->stop();
    });
    inject(0xC001);

    stack_->run();

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(stack_->mac(), k_src);
}

TEST(StackOwnership, BorrowedFdStaysOpenOwnedFdIsClosed)
{
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), 0);

    {
        Stack borrowed(fds[0], k_src, 1, fd_ownership_t::BORROW);
    }
    EXPECT_NE(::fcntl(fds[0], F_GETFD), -1);

    {
        Stack owned(fds[0], k_src, 1);
    }
    EXPECT_EQ(::fcntl(fds[0], F_GETFD), -1);
    ::close(fds[1]);
}
