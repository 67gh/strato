#include <cassert>
#include <cstdint>
#include <string>
#include <iostream>
using u32=uint32_t;
struct Result{u32 code;};
namespace type {struct KSession {};}
namespace ipc {struct Header{u32 type;};struct Payload{u32 value;};struct IpcRequest{bool isTipc;Header*header;Payload*payload;};struct IpcResponse{};}
u32 seen{};
namespace diagnostics{void Unsupported(const std::string&,u32 command,const char*){seen=command;}}
#define LOGW(...)
struct StubService{std::string serviceName="test";Result StubFunction(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);};
    Result StubService::StubFunction(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const u32 command{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};
        diagnostics::Unsupported(serviceName, command, "stub service; result=0xF601");
        LOGW("[STUB] Service '{}' cmd=0x{:X} tipc={} result=0xF601", serviceName, command, request.isTipc);
        return Result{0xF601};
    }
int main(){StubService service;type::KSession session;ipc::IpcResponse response;ipc::Header head{27};ipc::Payload payload{42};ipc::IpcRequest tipc{true,&head,nullptr},hipc{false,&head,&payload};assert(service.StubFunction(session,tipc,response).code==0xF601&&seen==27);assert(service.StubFunction(session,hipc,response).code==0xF601&&seen==42);std::cout<<"PASS production StubFunction: TIPC null payload, HIPC payload, unchanged 0xF601 result\n";}