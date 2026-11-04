// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <jvm.h>
#include "mp4_probe.h"
#include "results.h"
#include "IStorage.h"

namespace skyline::service::fssrv {
    IStorage::IStorage(std::shared_ptr<vfs::Backing> backing, const DeviceState &state, ServiceManager &manager, bool traceProgramRomfs)
        : BaseService(state, manager), backing(std::move(backing)), romfsTrace(traceProgramRomfs ? diagnostics::NewId() : 0) {
        if (romfsTrace)
            diagnostics::Event("program_romfs_open", romfsTrace);
    }

    Result IStorage::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto offset{request.Pop<i64>()};
        auto size{request.Pop<i64>()};

        if (offset < 0) {
            LOGW("Trying to read a file with a negative offset");
            return result::InvalidOffset;
        }

        if (size < 0) {
            LOGW("Trying to read a file with a negative size");
            return result::InvalidSize;
        }

        if (static_cast<u64>(offset) > backing->size)
            return result::InvalidOffset;
        if (static_cast<u64>(size) > backing->size - static_cast<size_t>(offset))
            return result::InvalidSize;
        if (!size)
            return {};
        if (request.outputBuf.empty() || static_cast<u64>(size) > request.outputBuf.front().size())
            return result::InvalidSize;

        auto output = request.outputBuf.front().first(static_cast<size_t>(size));
        backing->Read(output, static_cast<size_t>(offset));
        if (romfsTrace && IsMp4Candidate(output)
            && lastCandidate.exchange(static_cast<u64>(offset), std::memory_order_relaxed) != static_cast<u64>(offset)) {
            diagnostics::Event("romfs_mp4_candidate", romfsTrace, offset, size, "ftyp; guest player unknown; classic path");
            state.jvm->ObserveRomfsMovie(static_cast<u64>(offset));
        }
        return {};
    }

    Result IStorage::GetSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(backing->size);
        return {};
    }
}
