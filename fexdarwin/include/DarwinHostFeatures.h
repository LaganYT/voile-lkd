// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/Core/HostFeatures.h>
#include <string>

// Builds FEXCore::HostFeatures from sysctlbyname("hw.optional.arm.*") and a few direct probes. macOS user space cannot
// read the ARM ID registers (MIDR_EL1, ID_AA64*), mrs of those registers raises SIGILL (measured), so the Linux code
// path of FEX (Source/Common/HostFeatures.cpp) cannot be used.
FEXCore::HostFeatures GetDarwinHostFeatures(std::string* Report);
