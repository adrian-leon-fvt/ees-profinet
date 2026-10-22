/**
 * @file npcap_stack.h
 * @brief Example-only Npcap transport feeding portable Dispatcher hooks.
 */
#ifndef PROFINET_EXAMPLE_NPCAP_STACK_H
#define PROFINET_EXAMPLE_NPCAP_STACK_H

#include "profinet/dispatcher.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <pcap.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace profinet_example
{
class NpcapStack : public profinet::Dispatcher
{
  public:
    explicit NpcapStack(const std::string &interface_name)
    {
        const auto storage = adapters();
        const auto *adapter =
            reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(storage.data());
        std::string device;
        for (; adapter; adapter = adapter->Next)
        {
            const std::string name =
                std::string("\\Device\\NPF_") + adapter->AdapterName;
            if (interface_name != utf8(adapter->FriendlyName) &&
                interface_name != name &&
                interface_name != adapter->AdapterName)
            {
                continue;
            }
            if (adapter->PhysicalAddressLength != mac_.size())
            {
                throw std::runtime_error(
                    "Selected adapter has no Ethernet MAC");
            }
            std::memcpy(mac_.data(), adapter->PhysicalAddress, mac_.size());
            device = name;
            break;
        }
        if (device.empty())
        {
            throw std::invalid_argument(
                "Ethernet adapter not found: " + interface_name +
                "; use --list to see adapter names");
        }

        // Npcap DLLs normally live outside PATH. Load only from System32.
        wchar_t system_directory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            throw std::runtime_error("Cannot locate Windows system directory");
        }
        const auto library =
            std::wstring(system_directory) + L"\\Npcap\\wpcap.dll";
        // ponytail: keep Npcap loaded for process lifetime; examples are
        // short-lived.
        if (!LoadLibraryExW(
                library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        {
            throw std::system_error(
                GetLastError(), std::system_category(), "Load Npcap runtime");
        }
        char error[PCAP_ERRBUF_SIZE]{};
        capture_.reset(pcap_open_live(device.c_str(), 65536, 1, 100, error));
        if (!capture_)
        {
            throw std::runtime_error(std::string("pcap_open_live: ") + error);
        }
        if (pcap_datalink(capture_.get()) != DLT_EN10MB)
        {
            throw std::runtime_error("Selected adapter is not Ethernet");
        }
        if (pcap_setnonblock(capture_.get(), 1, error) != 0)
        {
            throw std::runtime_error(std::string("pcap_setnonblock: ") + error);
        }
        if (pcap_setmintocopy(capture_.get(), 0) != 0)
        {
            throw std::runtime_error(pcap_geterr(capture_.get()));
        }
    }

    static void list()
    {
        const auto storage = adapters();
        for (const auto *adapter =
                 reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(storage.data());
             adapter;
             adapter = adapter->Next)
        {
            if (adapter->PhysicalAddressLength == profinet::Mac{}.size())
            {
                std::printf(
                    "%s  (\\Device\\NPF_%s)\n",
                    utf8(adapter->FriendlyName).c_str(),
                    adapter->AdapterName);
            }
        }
    }

    const profinet::Mac &mac() const
    {
        return mac_;
    }

    void send(
        const profinet::Mac &destination,
        const std::uint16_t id,
        const profinet::Bytes payload)
    {
        const auto frame = profinet::build(destination, mac_, id, payload);
        send_raw(frame);
    }

    void send_raw(const profinet::Bytes frame)
    {
        if (pcap_sendpacket(
                capture_.get(), frame.data(), static_cast<int>(frame.size())) !=
            0)
        {
            throw std::runtime_error(pcap_geterr(capture_.get()));
        }
    }

    int poll(const int timeout_ms)
    {
        const DWORD ready = WaitForSingleObject(
            pcap_getevent(capture_.get()),
            timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
        if (ready == WAIT_TIMEOUT)
        {
            return 0;
        }
        if (ready != WAIT_OBJECT_0)
        {
            throw std::system_error(
                GetLastError(), std::system_category(), "Wait for Npcap");
        }
        int handled = 0;
        pcap_pkthdr *header = nullptr;
        const u_char *packet = nullptr;
        int result;
        while ((result = pcap_next_ex(capture_.get(), &header, &packet)) == 1)
        {
            if (header->caplen != header->len)
            {
                throw std::runtime_error("Npcap truncated an Ethernet frame");
            }
            handled += feed(profinet::Bytes(packet, header->caplen)) ? 1 : 0;
        }
        if (result < 0)
        {
            throw std::runtime_error(pcap_geterr(capture_.get()));
        }
        return handled;
    }

  private:
    static std::vector<unsigned char> adapters()
    {
        ULONG size = 16384;
        std::vector<unsigned char> storage(size);
        ULONG error;
        do
        {
            storage.resize(size);
            error = GetAdaptersAddresses(
                AF_UNSPEC,
                0,
                nullptr,
                reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()),
                &size);
        } while (error == ERROR_BUFFER_OVERFLOW);
        if (error != NO_ERROR)
        {
            throw std::system_error(
                error, std::system_category(), "GetAdaptersAddresses");
        }
        return storage;
    }

    static std::string utf8(const wchar_t *text)
    {
        const int length = WideCharToMultiByte(
            CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (length == 0)
        {
            throw std::runtime_error("Cannot convert adapter name to UTF-8");
        }
        std::string result(static_cast<std::size_t>(length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                0,
                text,
                -1,
                result.data(),
                length,
                nullptr,
                nullptr) == 0)
        {
            throw std::runtime_error("Cannot convert adapter name to UTF-8");
        }
        result.pop_back();
        return result;
    }

    std::unique_ptr<pcap_t, decltype(&pcap_close)> capture_{
        nullptr, &pcap_close};
    profinet::Mac mac_{};
};
} // namespace profinet_example

#endif
