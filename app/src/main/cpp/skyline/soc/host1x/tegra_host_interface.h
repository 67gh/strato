// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include "syncpoint.h"
#include "classes/class.h"

namespace skyline::soc::host1x {
    /**
     * @note Device CallMethod executes synchronously; an OpDone method observes completion of all preceding work.
     * @brief The 'Tegra Host Interface' or THI sits inbetween the Host1x and the class falcons, implementing completion signalling and a method interface
     */
    template<typename ClassType>
    class TegraHostInterface {
      private:
        SyncpointSet &syncpoints;
        ClassType deviceClass; //!< The device class behind the THI, such as NVDEC or VIC

        u32 storedMethod{}; //!< Method that will be used for deviceClass.CallMethod, set using Method0

      public:
        TegraHostInterface(SyncpointSet &syncpoints, const DeviceState &state)
            : deviceClass([] {}, state),
              syncpoints(syncpoints) {}

        void CallMethod(u32 method, u32 argument)  {
            constexpr u32 Method0MethodId{0x10}; //!< Sets the method to be called on the device class upon a call to Method1, see TRM '15.5.6 NV_PVIC_THI_METHOD0'
            constexpr u32 Method1MethodId{0x11}; //!< Calls the method set by Method1 with the supplied argument, see TRM '15.5.7 NV_PVIC_THI_METHOD1"

            switch (method) {
                case IncrementSyncpointMethodId: {
                    IncrementSyncpointMethod incrSyncpoint{.raw = argument};

                    if (incrSyncpoint.index >= syncpoints.size())
                        throw exception("Invalid THI syncpoint: {}", incrSyncpoint.index);

                    switch (incrSyncpoint.condition) {
                        case IncrementSyncpointMethod::Condition::Immediate:
                            LOGD("Increment syncpoint: {}", incrSyncpoint.index);
                            syncpoints.at(incrSyncpoint.index).Increment();
                            break;
                        case IncrementSyncpointMethod::Condition::OpDone:
                            LOGD("Synchronous OpDone syncpoint: {}", incrSyncpoint.index);
                            syncpoints.at(incrSyncpoint.index).Increment(); // synchronous backend: all preceding writes are visible
                            break;
                        default:
                            throw exception("Unsupported THI syncpoint condition: {}", static_cast<u8>(incrSyncpoint.condition));
                    }
                    break;
                }
                case Method0MethodId:
                    storedMethod = argument;
                    break;
                case Method1MethodId:
                    deviceClass.CallMethod(storedMethod, argument);
                    break;
                default:
                    throw exception("Unknown THI method: 0x{:X}, argument: 0x{:X}", method, argument);
            }
        }
    };
}
