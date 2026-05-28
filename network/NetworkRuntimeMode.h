#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace net {

    enum class NetworkRuntimeMode {
        Loopback = 0,
        Sender = 1,
        Receiver = 2,
        Monitor = 3
    };

    inline const char* ToString(NetworkRuntimeMode mode) {
        switch (mode) {
        case NetworkRuntimeMode::Sender:
            return "Sender";
        case NetworkRuntimeMode::Receiver:
            return "Receiver";
        case NetworkRuntimeMode::Monitor:
            return "Monitor";
        case NetworkRuntimeMode::Loopback:
        default:
            return "Loopback";
        }
    }

    inline NetworkRuntimeMode ParseNetworkRuntimeMode(
        std::string value,
        NetworkRuntimeMode fallback = NetworkRuntimeMode::Loopback
    ) {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

        if (value == "sender" || value == "send") {
            return NetworkRuntimeMode::Sender;
        }
        if (value == "receiver" || value == "receive") {
            return NetworkRuntimeMode::Receiver;
        }
        if (value == "monitor") {
            return NetworkRuntimeMode::Monitor;
        }
        if (value == "loopback" || value == "loop") {
            return NetworkRuntimeMode::Loopback;
        }

        return fallback;
    }

    inline bool NetworkModeCanSendVideo(NetworkRuntimeMode mode) {
        return mode == NetworkRuntimeMode::Loopback ||
            mode == NetworkRuntimeMode::Sender;
    }

    inline bool NetworkModeCanReceiveVideo(NetworkRuntimeMode mode) {
        return mode == NetworkRuntimeMode::Loopback ||
            mode == NetworkRuntimeMode::Receiver;
    }

    inline bool NetworkModeCanRunExperiment(NetworkRuntimeMode mode) {
        return mode == NetworkRuntimeMode::Loopback;
    }

} // namespace net
