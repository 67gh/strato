// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/address_space.inc>
#include "smmu.h"

namespace skyline {
    template class FlatAddressSpaceMap<u64, 0, u8 *, nullptr, true, soc::SmmuAddressSpaceBits, MemoryManagerBlockInfo>;
    template class FlatMemoryManager<u64, 0, soc::SmmuAddressSpaceBits, soc::SmmuPageSizeBits, soc::SmmuL2PageSizeBits>;
}
