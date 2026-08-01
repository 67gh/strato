// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Modified 2024: Strato Revival Project — added TOTK support + StubService fallback

#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include "sm/IUserInterface.h"
#include "settings/ISettingsServer.h"
#include "settings/ISystemSettingsServer.h"
#include "apm/IManager.h"
#include "am/IApplicationProxyService.h"
#include "am/IAllSystemAppletProxiesService.h"
#include "audio/IAudioInManager.h"
#include "audio/IAudioOutManager.h"
#include "audio/IAudioRendererManager.h"
#include "bcat/IServiceCreator.h"
#include "codec/IHardwareOpusDecoderManager.h"
#include "fatalsrv/IService.h"
#include "hid/IHidServer.h"
#include "irs/IIrSensorServer.h"
#include "irs/iirsensor_core.h"
#include "timesrv/IStaticService.h"
#include "glue/IStaticService.h"
#include "glue/IWriterForSystem.h"
#include "glue/INotificationServicesForApplication.h"
#include "services/timesrv/core.h"
#include "fssrv/IFileSystemProxy.h"
#include "nvdrv/INvDrvServices.h"
#include "nvdrv/driver.h"
#include "hosbinder/IHOSBinderDriver.h"
#include "visrv/IApplicationRootService.h"
#include "visrv/ISystemRootService.h"
#include "visrv/IManagerRootService.h"
#include "pl/IPlatformServiceManager.h"
#include "pl/shared_font_core.h"
#include "aocsrv/IAddOnContentManager.h"
#include "pctl/IParentalControlServiceFactory.h"
#include "lbl/ILblController.h"
#include "lm/ILogService.h"
#include "ldn/IUserServiceCreator.h"
#include "account/IAccountServiceForApplication.h"
#include "friends/IServiceCreator.h"
#include "nfp/IUserManager.h"
#include "nifm/IStaticService.h"
#include "nim/IShopServiceAccessServerInterface.h"
#include "socket/bsd/IClient.h"
#include "socket/nsd/IManager.h"
#include "socket/sfdnsres/IResolver.h"
#include "spl/IRandomInterface.h"
#include "ssl/ISslService.h"
#include "prepo/IPrepoService.h"
#include "mmnv/IRequest.h"
#include "bt/IBluetoothUser.h"
#include "btm/IBtmUser.h"
#include "capsrv/IAlbumAccessorService.h"
#include "capsrv/ICaptureControllerService.h"
#include "capsrv/IAlbumApplicationService.h"
#include "capsrv/IScreenShotApplicationService.h"
#include "ro/IRoInterface.h"
#include "mii/IStaticService.h"
#include "olsc/IOlscServiceForApplication.h"
#include "clkrst/IClkrstManager.h"
#include "ts/IMeasurementServer.h"
#include "psm/IPsmServer.h"
#include "ntc/IEnsureNetworkClockAvailabilityService.h"
#include "stub_service.h"
#include "serviceman.h"

#define SERVICE_CASE(class, name, ...) \
    case util::MakeMagic<ServiceName>(name): { \
            std::shared_ptr<BaseService> serviceObject{std::make_shared<class>(state, *this, ##__VA_ARGS__)}; \
            serviceMap[util::MakeMagic<ServiceName>(name)] = serviceObject; \
            return serviceObject; \
        }

namespace skyline::service {
    struct GlobalServiceState {
        timesrv::core::TimeServiceObject timesrv;
        pl::SharedFontCore sharedFontCore;
        irs::SharedIirCore sharedIirCore;
        nvdrv::Driver nvdrv;

        explicit GlobalServiceState(const DeviceState &state) : timesrv(state), sharedFontCore(state), sharedIirCore(state), nvdrv(state) {}
    };

    ServiceManager::ServiceManager(const DeviceState &state) : state(state), smUserInterface(std::make_shared<sm::IUserInterface>(state, *this)), globalServiceState(std::make_shared<GlobalServiceState>(state)) {}

    std::shared_ptr<BaseService> ServiceManager::CreateOrGetService(ServiceName name) {
        auto serviceIter{serviceMap.find(name)};
        if (serviceIter != serviceMap.end())
            return (*serviceIter).second;

        switch (name) {
            SERVICE_CASE(fatalsrv::IService, "fatal:u")
            SERVICE_CASE(settings::ISettingsServer, "set")
            SERVICE_CASE(settings::ISystemSettingsServer, "set:sys")
            SERVICE_CASE(apm::IManager, "apm")
            SERVICE_CASE(am::IApplicationProxyService, "appletOE")
            SERVICE_CASE(am::IAllSystemAppletProxiesService, "appletAE")
            SERVICE_CASE(audio::IAudioInManager, "audin:u")
            SERVICE_CASE(audio::IAudioOutManager, "audout:u")
            SERVICE_CASE(audio::IAudioRendererManager, "audren:u")
            SERVICE_CASE(codec::IHardwareOpusDecoderManager, "hwopus")
            SERVICE_CASE(hid::IHidServer, "hid")
            SERVICE_CASE(irs::IIrSensorServer, "irs", globalServiceState->sharedIirCore)
            SERVICE_CASE(timesrv::IStaticService, "time:s", globalServiceState->timesrv, timesrv::constant::StaticServiceSystemPermissions)
            SERVICE_CASE(timesrv::IStaticService, "time:su", globalServiceState->timesrv, timesrv::constant::StaticServiceSystemUpdatePermissions)
            SERVICE_CASE(glue::IStaticService, "time:a", globalServiceState->timesrv.managerServer.GetStaticServiceAsAdmin(state, *this), globalServiceState->timesrv, timesrv::constant::StaticServiceAdminPermissions)
            SERVICE_CASE(glue::IStaticService, "time:r", globalServiceState->timesrv.managerServer.GetStaticServiceAsRepair(state, *this), globalServiceState->timesrv, timesrv::constant::StaticServiceRepairPermissions)
            SERVICE_CASE(glue::IStaticService, "time:u", globalServiceState->timesrv.managerServer.GetStaticServiceAsUser(state, *this), globalServiceState->timesrv, timesrv::constant::StaticServiceUserPermissions)
            SERVICE_CASE(glue::INotificationServicesForApplication, "notif:a")
            SERVICE_CASE(glue::IWriterForSystem, "ectx:w")
            SERVICE_CASE(glue::IWriterForSystem, "ectx:aw")
            SERVICE_CASE(fssrv::IFileSystemProxy, "fsp-srv")
            SERVICE_CASE(nvdrv::INvDrvServices, "nvdrv", globalServiceState->nvdrv, nvdrv::ApplicationSessionPermissions)
            SERVICE_CASE(nvdrv::INvDrvServices, "nvdrv:a", globalServiceState->nvdrv, nvdrv::AppletSessionPermissions)
            SERVICE_CASE(hosbinder::IHOSBinderDriver, "dispdrv", globalServiceState->nvdrv.core.nvMap)
            SERVICE_CASE(visrv::IApplicationRootService, "vi:u")
            SERVICE_CASE(visrv::ISystemRootService, "vi:s")
            SERVICE_CASE(visrv::IManagerRootService, "vi:m")
            SERVICE_CASE(pl::IPlatformServiceManager, "pl:u", globalServiceState->sharedFontCore)
            SERVICE_CASE(aocsrv::IAddOnContentManager, "aoc:u")
            SERVICE_CASE(pctl::IParentalControlServiceFactory, "pctl")
            SERVICE_CASE(pctl::IParentalControlServiceFactory, "pctl:a")
            SERVICE_CASE(pctl::IParentalControlServiceFactory, "pctl:s")
            SERVICE_CASE(pctl::IParentalControlServiceFactory, "pctl:r")
            SERVICE_CASE(lbl::ILblController, "lbl")
            SERVICE_CASE(lm::ILogService, "lm")
            SERVICE_CASE(ldn::IUserServiceCreator, "ldn:u")
            SERVICE_CASE(account::IAccountServiceForApplication, "acc:u0")
            SERVICE_CASE(friends::IServiceCreator, "friend:u")
            SERVICE_CASE(nfp::IUserManager, "nfp:user")
            SERVICE_CASE(nifm::IStaticService, "nifm:u")
            SERVICE_CASE(socket::IClient, "bsd:u")
            SERVICE_CASE(socket::IClient, "bsd:s")
            SERVICE_CASE(socket::IManager, "nsd:u")
            SERVICE_CASE(socket::IManager, "nsd:a")
            SERVICE_CASE(socket::IResolver, "sfdnsres")
            SERVICE_CASE(spl::IRandomInterface, "csrng")
            SERVICE_CASE(ssl::ISslService, "ssl")
            SERVICE_CASE(prepo::IPrepoService, "prepo:u")
            SERVICE_CASE(prepo::IPrepoService, "prepo:a")
            SERVICE_CASE(mmnv::IRequest, "mm:u")
            SERVICE_CASE(bcat::IServiceCreator, "bcat:u")
            SERVICE_CASE(bt::IBluetoothUser, "bt")
            SERVICE_CASE(btm::IBtmUser, "btm:u")
            SERVICE_CASE(capsrv::IAlbumAccessorService, "caps:a")
            SERVICE_CASE(capsrv::ICaptureControllerService, "caps:c")
            SERVICE_CASE(capsrv::IAlbumApplicationService, "caps:u")
            SERVICE_CASE(capsrv::IScreenShotApplicationService, "caps:su")
            SERVICE_CASE(nim::IShopServiceAccessServerInterface, "nim:eca")
            SERVICE_CASE(ro::IRoInterface, "ldr:ro")
            SERVICE_CASE(mii::IStaticService, "mii:e")
            SERVICE_CASE(mii::IStaticService, "mii:u")
            SERVICE_CASE(olsc::IOlscServiceForApplication, "olsc:u")
            SERVICE_CASE(clkrst::IClkrstManager, "clkrst")
            SERVICE_CASE(ts::IMeasurementServer, "ts")
            SERVICE_CASE(psm::IPsmServer, "psm")
            SERVICE_CASE(ntc::IEnsureNetworkClockAvailabilityService, "ntc")

            /* ── TOTK / Modern games support ── */
            SERVICE_CASE(settings::ISystemSettingsServer, "set:cal")
            SERVICE_CASE(settings::ISystemSettingsServer, "set:fd")
            SERVICE_CASE(apm::IManager, "apm:p")
            SERVICE_CASE(apm::IManager, "apm:sys")
            SERVICE_CASE(psm::IPsmServer, "spsm")
            SERVICE_CASE(nifm::IStaticService, "nifm:s")
            SERVICE_CASE(nifm::IStaticService, "nifm:a")
            SERVICE_CASE(socket::IClient, "bsdcfg")
            SERVICE_CASE(fssrv::IFileSystemProxy, "fsp-ldr")
            SERVICE_CASE(fssrv::IFileSystemProxy, "fsp-pr")

            /* ── Stubs for services not yet implemented ── */
            case util::MakeMagic<ServiceName>("pm:shell"):
            case util::MakeMagic<ServiceName>("pm:dmnt"):
            case util::MakeMagic<ServiceName>("pm:info"):
            case util::MakeMagic<ServiceName>("pm:bm"):
            case util::MakeMagic<ServiceName>("psc:c"):
            case util::MakeMagic<ServiceName>("usb:ds"):
            case util::MakeMagic<ServiceName>("usb:hs"):
            case util::MakeMagic<ServiceName>("usb:pd"):
            case util::MakeMagic<ServiceName>("usb:pm"):
            case util::MakeMagic<ServiceName>("ethc:c"):
            case util::MakeMagic<ServiceName>("ethc:i"):
            case util::MakeMagic<ServiceName>("ns"):
            case util::MakeMagic<ServiceName>("ns:am"):
            case util::MakeMagic<ServiceName>("ns:web"):
            case util::MakeMagic<ServiceName>("nim"):
            case util::MakeMagic<ServiceName>("nim:shp"):
            case util::MakeMagic<ServiceName>("es"):
            case util::MakeMagic<ServiceName>("lcs"):
            case util::MakeMagic<ServiceName>("lr"):
            case util::MakeMagic<ServiceName>("lg"):
            case util::MakeMagic<ServiceName>("ldn:m"):
            case util::MakeMagic<ServiceName>("ldn:s"):
            case util::MakeMagic<ServiceName>("bsd:s"):
            case util::MakeMagic<ServiceName>("bsd:u"): {
                std::string_view nameString(span(reinterpret_cast<char *>(&name), sizeof(name)).as_string(true));
                LOGW("[STUB] Creating stub for service: '{}'", nameString);
                auto stubService{std::make_shared<StubService>(state, *this, std::string(nameString))};
                serviceMap[name] = stubService;
                return stubService;
            }

            default: {
                std::string_view nameString(span(reinterpret_cast<char *>(&name), sizeof(name)).as_string(true));
                LOGW("[STUB] Unknown service requested: '{}'. Creating StubService.", nameString);
                auto stubService{std::make_shared<StubService>(state, *this, std::string(nameString))};
                serviceMap[name] = stubService;
                return stubService;
            }
        }
    }

    std::shared_ptr<BaseService> ServiceManager::NewService(ServiceName name, type::KSession &session, ipc::IpcResponse &response) {
        std::scoped_lock serviceGuard{mutex};
        auto serviceObject{CreateOrGetService(name)};
        KHandle handle{};
        if (session.isDomain) {
            session.domains.push_back(serviceObject);
            response.domainObjects.push_back(session.handleIndex);
            handle = session.handleIndex++;
        } else {
            handle = state.process->NewHandle<type::KSession>(serviceObject).handle;
            response.moveHandles.push_back(handle);
        }
        LOGD("Service has been created: \"{}\" (0x{:X})", serviceObject->GetName(), handle);
        return serviceObject;
    }

    void ServiceManager::RegisterService(std::shared_ptr<BaseService> serviceObject, type::KSession &session, ipc::IpcResponse &response) {
        std::scoped_lock serviceGuard{mutex};
        KHandle handle{};

        if (session.isDomain) {
            session.domains.push_back(serviceObject);
            response.domainObjects.push_back(session.handleIndex);
            handle = session.handleIndex++;
        } else {
            handle = state.process->NewHandle<type::KSession>(serviceObject).handle;
            response.moveHandles.push_back(handle);
        }

        LOGD("Service has been registered: \"{}\" (0x{:X})", serviceObject->GetName(), handle);
    }

    void ServiceManager::CloseSession(KHandle handle) {
        std::scoped_lock serviceGuard{mutex};
        auto session{state.process->GetHandle<type::KSession>(handle)};
        if (session->isOpen) {
            if (session->isDomain) {
                for (const auto &domainService : session->domains)
                    std::erase_if(serviceMap, [domainService](const auto &entry) {
                        return entry.second == domainService;
                    });
            } else {
                std::erase_if(serviceMap, [session](const auto &entry) {
                    return entry.second == session->serviceObject;
                });
            }
            session->isOpen = false;
        }
    }

    void ServiceManager::SyncRequestHandler(KHandle handle) {
        TRACE_EVENT("kernel", "ServiceManager::SyncRequestHandler");
        auto session{state.process->GetHandle<type::KSession>(handle)};
        LOGV("----IPC Start----");
        LOGV("Handle is 0x{:X}", handle);

        if (session->isOpen) {
            ipc::IpcRequest request(session->isDomain, state);
            ipc::IpcResponse response(state);

            switch (request.header->type) {
                case ipc::CommandType::Request:
                case ipc::CommandType::RequestWithContext:
                    if (session->isDomain) {
                        try {
                            auto service{session->domains.at(request.domain->objectId)};
                            if (service == nullptr)
                                throw exception("Domain request used an expired handle");
                            switch (request.domain->command) {
                                case ipc::DomainCommand::SendMessage:
                                    response.errorCode = service->HandleRequest(*session, request, response);
                                    break;

                                case ipc::DomainCommand::CloseVHandle:
                                    std::erase_if(serviceMap, [service](const auto &entry) {
                                        return entry.second == service;
                                    });
                                    session->domains.at(request.domain->objectId).reset();
                                    break;
                            }
                        } catch (std::out_of_range &) {
                            throw exception("Invalid object ID was used with domain request");
                        }
                    } else {
                        response.errorCode = session->serviceObject->HandleRequest(*session, request, response);
                    }
                    response.WriteResponse(session->isDomain);
                    break;

                case ipc::CommandType::Control:
                case ipc::CommandType::ControlWithContext:
                    LOGD("Control IPC Message: 0x{:X}", request.payload->value);
                    switch (static_cast<ipc::ControlCommand>(request.payload->value)) {
                        case ipc::ControlCommand::ConvertCurrentObjectToDomain:
                            response.Push(session->ConvertDomain());
                            break;

                        case ipc::ControlCommand::CloneCurrentObject:
                        case ipc::ControlCommand::CloneCurrentObjectEx:
                            response.moveHandles.push_back(state.process->InsertItem(session));
                            break;

                        case ipc::ControlCommand::QueryPointerBufferSize:
                            response.Push<u32>(0x8000);
                            break;

                        default:
                            throw exception("Unknown Control Command: {}", request.payload->value);
                    }
                    response.WriteResponse(false);
                    break;

                case ipc::CommandType::Close:
                case ipc::CommandType::TipcCloseSession:
                    LOGD("Closing Session");
                    CloseSession(handle);
                    break;
                default:
                    // TIPC command ID is encoded in the request type
                    if (request.isTipc) {
                        response.errorCode = session->serviceObject->HandleRequest(*session, request, response);
                        response.WriteResponse(session->isDomain, true);
                    } else {
                        throw exception("Unimplemented IPC message type: {}", static_cast<u16>(request.header->type));
                    }
            }
        } else {
            LOGW("svcSendSyncRequest called on closed handle: 0x{:X}", handle);
        }
        LOGV("====IPC End====");
    }
}
