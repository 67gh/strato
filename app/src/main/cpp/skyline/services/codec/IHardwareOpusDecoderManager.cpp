// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/serviceman.h>

#include "IHardwareOpusDecoderManager.h"
#include "IHardwareOpusDecoder.h"

namespace skyline::service::codec {
    static u32 CalculateBufferSize(i32 sampleRate, i32 channelCount, i32 useLargerFrameSize = 0) {
        u32 requiredSize{static_cast<u32>(opus_decoder_get_size(channelCount))};
        requiredSize += MaxInputBufferSize + CalculateOutBufferSize(sampleRate, channelCount, useLargerFrameSize ? MaxFrameSizeEx : MaxFrameSizeNormal);
        return requiredSize;
    }

    static u32 CalculateMultiStreamBufferSize(i32 sampleRate, i32 channelCount, i32 streamCount, i32 stereoStreamCount, bool useLargerFrameSize) {
        i32 size{opus_multistream_decoder_get_size(streamCount, stereoStreamCount)};
        if (size <= 0)
            throw exception("Invalid Opus multi-stream parameters: streams: {}, stereo streams: {}", streamCount, stereoStreamCount);

        return static_cast<u32>(size) + MaxInputBufferSize + CalculateOutBufferSize(sampleRate, channelCount, useLargerFrameSize ? MaxFrameSizeEx : MaxFrameSizeNormal);
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoder(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus decoder: Sample rate: {}, Channel count: {}, Work buffer handle: 0x{:X} (Size: 0x{:X})", sampleRate, channelCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, sampleRate, channelCount, workBufferSize, workBuffer), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus decoder: Sample rate: {}, Channel count: {}, Work buffer handle: 0x{:X} (Size: 0x{:X})", sampleRate, channelCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, sampleRate, channelCount, workBufferSize, workBuffer, useLargerFrameSize), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount, useLargerFrameSize));
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto &parameters{request.inputBuf.at(0).as<MultiStreamParameters>()};

        response.Push<u32>(CalculateMultiStreamBufferSize(parameters.sampleRate, parameters.channelCount, parameters.streamCount, parameters.stereoStreamCount, false));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        MultiStreamParameters parameters{request.inputBuf.at(0).as<MultiStreamParameters>()};
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus multi-stream decoder: Sample rate: {}, Channel count: {}, Streams: {} (stereo: {}), Work buffer handle: 0x{:X} (Size: 0x{:X})", parameters.sampleRate, parameters.channelCount, parameters.streamCount, parameters.stereoStreamCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, parameters, workBufferSize, workBuffer), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto &parameters{request.inputBuf.at(0).as<MultiStreamParametersEx>()};

        response.Push<u32>(CalculateMultiStreamBufferSize(parameters.sampleRate, parameters.channelCount, parameters.streamCount, parameters.stereoStreamCount, parameters.useLargerFrameSize != 0));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        MultiStreamParametersEx parametersEx{request.inputBuf.at(0).as<MultiStreamParametersEx>()};
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        MultiStreamParameters parameters{parametersEx.sampleRate, parametersEx.channelCount, parametersEx.streamCount, parametersEx.stereoStreamCount, parametersEx.mappings};

        LOGD("Creating Opus multi-stream decoder: Sample rate: {}, Channel count: {}, Streams: {} (stereo: {}), Work buffer handle: 0x{:X} (Size: 0x{:X})", parameters.sampleRate, parameters.channelCount, parameters.streamCount, parameters.stereoStreamCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, parameters, workBufferSize, workBuffer, parametersEx.useLargerFrameSize != 0), session, response);
        return {};
    }
}
