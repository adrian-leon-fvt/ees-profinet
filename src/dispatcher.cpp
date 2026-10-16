/**
 * @file dispatcher.cpp
 * @brief Implementation of profinet::Dispatcher.
 */
#include "profinet/dispatcher.h"

namespace profinet
{

void Dispatcher::on(
    const std::uint16_t first,
    const std::uint16_t last,
    Hook hook)
{
    hooks_.push_back({first, last, std::move(hook)});
}

void Dispatcher::on(const std::uint16_t id, Hook hook)
{
    on(id, id, std::move(hook));
}

void Dispatcher::on_any(Hook hook)
{
    on(0x0000, 0xFFFF, std::move(hook));
}

void Dispatcher::on_ethertype(const std::uint16_t ethertype, Hook hook)
{
    ethertype_hooks_.emplace_back(ethertype, std::move(hook));
}

bool Dispatcher::feed(const Bytes raw, const std::optional<Vlan> &vlan) const
{
    auto frame = parse(raw);
    if (!frame)
    {
        frame = parse_ethernet(raw);
        if (!frame || frame->ethertype == k_ethertype)
        {
            return false; // runt or truncated PROFINET frame
        }
    }
    if (vlan)
    {
        frame->vlan = vlan;
    }
    return dispatch(*frame);
}

bool Dispatcher::dispatch(const FrameView &frame) const
{
    bool matched = frame.ethertype == k_ethertype;
    if (matched)
    {
        for (const Entry &e : hooks_)
        {
            if (frame.frame_id >= e.first && frame.frame_id <= e.last)
            {
                e.hook(frame);
            }
        }
    }
    for (const auto &[type, hook] : ethertype_hooks_)
    {
        if (type == frame.ethertype)
        {
            hook(frame);
            matched = true;
        }
    }
    return matched;
}

} // namespace profinet
