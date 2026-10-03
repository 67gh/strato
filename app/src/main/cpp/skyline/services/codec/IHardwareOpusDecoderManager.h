// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/base_service.h>
#include "IHardwareOpusDecoder.h"

namespace skyline::service::codec {
    /**
     * @brief Manages all instances of IHardwareOpusDecoder
     * @url https://switchbrew.org/wiki/Audio_services#hwopus
     */
    class IHardwareOpusDecoderManager : public BaseService {
      public:
        IHardwareOpusDecoderManager(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

        /**
         * @brief Returns an IHardwareOpusDecoder object
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoder
         */
        Result OpenHardwareOpusDecoder(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required size for the decoder's work buffer
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSize
         */
        Result GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns an IHardwareOpusDecoder object [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoder
         */
        Result OpenHardwareOpusDecoderEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required size for the decoder's work buffer [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeEx
         */
        Result GetWorkBufferSizeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required size for the work buffer of a multi-stream decoder
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeForMultiStream
         */
        Result GetWorkBufferSizeForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns an IHardwareOpusDecoder object for a multi-stream decoder
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoderForMultiStream
         */
        Result OpenHardwareOpusDecoderForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required size for the work buffer of a multi-stream decoder [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeForMultiStreamEx
         */
        Result GetWorkBufferSizeForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns an IHardwareOpusDecoder object for a multi-stream decoder [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoderForMultiStreamEx
         */
        Result OpenHardwareOpusDecoderForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IHardwareOpusDecoderManager, OpenHardwareOpusDecoder),
            SFUNC(0x1, IHardwareOpusDecoderManager, GetWorkBufferSize),
            SFUNC(0x2, IHardwareOpusDecoderManager, OpenHardwareOpusDecoderForMultiStream),
            SFUNC(0x3, IHardwareOpusDecoderManager, GetWorkBufferSizeForMultiStream),
            SFUNC(0x4, IHardwareOpusDecoderManager, OpenHardwareOpusDecoderEx),
            SFUNC(0x5, IHardwareOpusDecoderManager, GetWorkBufferSizeEx),
            SFUNC(0x6, IHardwareOpusDecoderManager, OpenHardwareOpusDecoderForMultiStreamEx),
            SFUNC(0x7, IHardwareOpusDecoderManager, GetWorkBufferSizeForMultiStreamEx),
        )
    };
}
