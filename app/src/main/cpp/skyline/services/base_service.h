// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Modified 2024: Strato Revival Project — added graceful stub fallback

#pragma once

#include <kernel/ipc.h>
#include <cxxabi.h>
#include <mutex>
#include <unordered_set>
#include <string>

constexpr static skyline::u32 TipcFunctionIdFlag{1U << 31}; //!< Flag applied to the stored service function ID to differentiate between TIPC and HIPC functions
#define SERVICE_STRINGIFY(string) #string
#define SFUNC(id, Class, Function) std::pair<u32, std::pair<Result(Class::*)(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &), const char*>>{id, {&Class::Function, SERVICE_STRINGIFY(Class::Function)}}
#define SFUNC_TIPC(id, Class, Function) std::pair<u32, std::pair<Result(Class::*)(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &), const char*>>{TipcFunctionIdFlag | id, {&Class::Function, SERVICE_STRINGIFY(Class::Function)}}
#define SFUNC_BASE(id, Class, BaseClass, Function) std::pair<u32, std::pair<Result(Class::*)(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &), const char*>>{id, {&Class::CallBaseFunction<BaseClass, decltype(&BaseClass::Function), &BaseClass::Function>, SERVICE_STRINGIFY(Class::Function)}}
#define SERVICE_DECL_AUTO(name, value) decltype(value) name = value

/**
 * @brief SERVICE_DECL now uses find() instead of at() and falls back to a stub
 *        function that returns ResultNotImplemented instead of crashing.
 */
#define SERVICE_DECL(...) \
    private: \
        template<typename BaseClass, typename BaseFunctionType, BaseFunctionType BaseFunction> \
        Result CallBaseFunction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) { \
            return (static_cast<BaseClass *>(this)->*BaseFunction)(session, request, response); \
        } \
        SERVICE_DECL_AUTO(functions, frozen::make_unordered_map({__VA_ARGS__})); \
    protected: \
        ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) override { \
            u32 key{(isTipc ? TipcFunctionIdFlag : 0U) | id}; \
            auto it{functions.find(key)}; \
            if (it == functions.end()) [[unlikely]] { \
                if (BaseService::ShouldLogStubCall(GetName(), id)) LOGW("[STUB] Missing function in service '{}': cmdId=0x{:X} (tipc={}) — further calls to this cmdId will not be logged", GetName(), id, isTipc); \
                return ServiceFunctionDescriptor{ \
                    reinterpret_cast<DerivedService*>(this), \
                    reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(&BaseService::StubFunction), \
                    "StubFunction" \
                }; \
            } \
            auto& function{it->second}; \
            return ServiceFunctionDescriptor{ \
                reinterpret_cast<DerivedService*>(this), \
                reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(function.first), \
                function.second \
            }; \
        }
#define SRVREG(class, ...) std::make_shared<class>(state, manager, ##__VA_ARGS__)

namespace skyline::kernel::type {
    class KSession;
}

namespace skyline::service {
    using namespace kernel;
    using ServiceName = u64; //!< Service names are a maximum of 8 bytes so we use a u64 to store them

    class ServiceManager;

    /**
     * @brief The base class for the HOS service interfaces hosted by sysmodules
     */
    class BaseService : public std::enable_shared_from_this<BaseService> {
      private:
        std::string name; //!< The name of the service, it's only assigned after GetName is called and shouldn't be used directly

      protected:
        const DeviceState &state;
        ServiceManager &manager;

        class DerivedService; //!< A placeholder derived class which is used for class function semantics

        /**
         * @brief A per-function descriptor for HLE service functions
         */
        struct ServiceFunctionDescriptor {
            DerivedService *clazz; //!< A pointer to the class that this was derived from, it's used as the 'this' pointer for the function
            Result (DerivedService::*function)(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &); //!< A function pointer to a HLE implementation of the service function
            const char *name; //!< A pointer to a static string in the format "Class::Function" for the specific service class/function

            constexpr Result operator()(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
                return (clazz->*function)(session, request, response);
            }
        };

      public:
        /**
         * @brief Generic stub implementation used for missing/unimplemented IPC functions.
         * @note This must be a real member function (not a lambda) since only pointer-to-member-function
         *       values can be reinterpret_cast to another pointer-to-member-function type per the standard.
         * @note Must be public: forming a pointer-to-member to an inherited protected member from the
         *       SERVICE_DECL macro (expanded in each derived service class) requires the member to be
         *       named via the current derived class, which the macro cannot do generically.
         */
        Result StubFunction(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
            return Result{0xF601}; // NotImplemented
        }

      public:
        /**
         * @brief Returns true only the first time it's called for a given (service, cmdId)
         *        pair. Used to avoid the logcat I/O cost of logging on every single call to
         *        a missing/stubbed IPC function — some games poll unimplemented functions
         *        every frame, and unconditional per-call logging there was measurably
         *        causing frame pacing stutter despite a stable average FPS.
         */
        static bool ShouldLogStubCall(std::string_view serviceName, u32 cmdId) {
            static std::mutex mtx;
            static std::unordered_set<std::string> logged;
            std::string key{serviceName};
            key += ':';
            key += std::to_string(cmdId);
            std::scoped_lock lock{mtx};
            return logged.insert(std::move(key)).second;
        }

      public:
        BaseService(const DeviceState &state, ServiceManager &manager) : state(state), manager(manager) {}

        /**
         * @note To be able to extract the name of the underlying class and ensure correct destruction order
         */
        virtual ~BaseService() = default;

        virtual ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) {
            if (ShouldLogStubCall(GetName(), id))
                LOGW("[STUB] BaseService::GetServiceFunction called (cmdId=0x{:X}, tipc={}) — further calls to this cmdId will not be logged", id, isTipc);
            return ServiceFunctionDescriptor{
                reinterpret_cast<DerivedService*>(this),
                reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(&BaseService::StubFunction),
                "BaseService::Stub"
            };
        }

        /**
         * @return A string with the name of the service class
         * @note The lifetime of the returned string is tied to that of the class
         */
        virtual const std::string &GetName() {
            if (name.empty()) {
                auto mangledName{typeid(*this).name()};

                int status{};
                size_t length{};
                std::unique_ptr<char, decltype(&std::free)> demangled{abi::__cxa_demangle(mangledName, nullptr, &length, &status), std::free};

                name = (status == 0) ? std::string(demangled.get() + std::char_traits<char>::length("skyline::service::")) : mangledName;
            }
            return name;
        }

        /**
         * @brief Handles an IPC Request to a service
         */
        Result HandleRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
    };
}