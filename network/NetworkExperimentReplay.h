#pragma once

#include <string>

namespace net {

    // Returns true when replay mode was requested and handled.
    bool RunNetworkExperimentReplayFromEnv(const std::string& outputDirectory);

} // namespace net
