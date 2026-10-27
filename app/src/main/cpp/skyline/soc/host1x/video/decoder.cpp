// SPDX-License-Identifier: MPL-2.0
#include "decoder.h"
#include "h264.h"
#include <cstring>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace skyline::soc::host1x::video {
namespace {
void CheckCodec(int result, const char *operation) {
    if (result<0) {
        char message[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result,message,sizeof(message));
        throw std::runtime_error(std::string("NVDEC ")+operation+": "+message);
    }
}
}
struct Decoder::Impl {
    AVCodecContext *context{};
    AVFrame *frame{av_frame_alloc()};
    AVPacket *packet{av_packet_alloc()};
    unsigned codec{}, width{}, height{};
    ~Impl() { avcodec_free_context(&context); av_frame_free(&frame); av_packet_free(&packet); }
    void Open(unsigned id, unsigned w, unsigned h) {
        if (!frame || !packet) throw std::bad_alloc();
        if (context && id==codec && width==w && height==h) return;
        avcodec_free_context(&context);
        const auto *decoder=avcodec_find_decoder(id==3 ? AV_CODEC_ID_H264 : AV_CODEC_ID_VP8);
        if (!decoder) throw std::runtime_error("NVDEC: decoder missing from bundled FFmpeg");
        context=avcodec_alloc_context3(decoder);
        if (!context) throw std::bad_alloc();
        context->thread_count=1; // One submitted picture must finish before signalling OpDone.
        context->thread_type=0;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context->err_recognition=AV_EF_EXPLODE;
        context->max_pixels=4096LL*4096;
        CheckCodec(avcodec_open2(context,decoder,nullptr),"open");
        codec=id; width=w; height=h;
    }
};
Decoder::Decoder() : impl(std::make_unique<Impl>()) {}
Decoder::~Decoder()=default;
void Decoder::Method(std::uint32_t method, std::uint32_t value, const Memory &memory) {
    if (method>=regs.size()) throw std::runtime_error("NVDEC: method outside register bank");
    regs[method]=value;
    if (method==0xc0) Execute(memory);
}
void Decoder::Execute(const Memory &memory) {
    const unsigned codec=regs[0x80];
    if (codec!=3 && codec!=5)
        throw std::runtime_error("NVDEC: codec unsupported (implemented: H264 progressive and VP8)");
    const auto pic=memory.Read(std::uint64_t(regs[0x101])<<8,codec==3 ? 0x2fc : 0xc0);
    if (U32(pic,0x30)&0x8001U) throw std::runtime_error("NVDEC: encrypted input unsupported");
    const auto size=U32(pic,0x48);
    if (!size || size>16*1024*1024) throw std::runtime_error("NVDEC: invalid bitstream length");
    unsigned width{},height{},index{};
    Bytes packet;
    if (codec==3) {
        packet=H264Parameters(pic);
        width=U32(pic,0x64)*16; height=U32(pic,0x68)*16;
        index=static_cast<unsigned>((U64(pic,0xb0)>>34)&0x7f);
        if (index>=17 || (U32(pic,0x6c)&3U) || ((U64(pic,0xb0)>>63)&1))
            throw std::runtime_error("NVDEC: invalid H264 output surface/layout");
    } else {
        width=U32(pic,0x38)&0xffff; height=U32(pic,0x38)>>16;
        index=3;
        if (pic[0x3e]&3U) throw std::runtime_error("NVDEC: unsupported VP8 surface layout");
        const auto firstPartition=U32(pic,0x40);
        if (firstPartition>size || firstPartition>0x7ffff || pic[0x3d]>3)
            throw std::runtime_error("NVDEC: invalid VP8 partition/header");
        const bool key=pic[0x3c]!=0;
        const auto tag=(firstPartition<<5) | 16U | (unsigned(pic[0x3d])<<1) | (key?0U:1U);
        packet={static_cast<std::uint8_t>(tag),static_cast<std::uint8_t>(tag>>8),static_cast<std::uint8_t>(tag>>16)};
        if (key) {
            packet.insert(packet.end(),{0x9d,0x01,0x2a,static_cast<std::uint8_t>(width),static_cast<std::uint8_t>(width>>8),
                                       static_cast<std::uint8_t>(height),static_cast<std::uint8_t>(height>>8)});
        }
    }
    if (!width || !height || width>4096 || height>4096)
        throw std::runtime_error("NVDEC: invalid picture dimensions");
    // T210 decoder surfaces use two GOBs vertically (16 rows).
    const Plane yPlane(width,height,false,1), uvPlane((width+1)/2*2,(height+1)/2,false,1);
    const auto yAddress=std::uint64_t(regs[0x10c+index])<<8;
    const auto uvAddress=std::uint64_t(regs[0x11d+index])<<8;
    memory.Check(yAddress,yPlane.Size()); memory.Check(uvAddress,uvPlane.Size());
    const auto statusAddress=std::uint64_t(regs[0x109])<<8;
    if (statusAddress) memory.Check(statusAddress,56); // nvdec_status_s
    auto stream=memory.Read(std::uint64_t(regs[0x102])<<8,size);
    packet.insert(packet.end(),stream.begin(),stream.end());
    impl->Open(codec,width,height);
    av_packet_unref(impl->packet);
    CheckCodec(av_new_packet(impl->packet,static_cast<int>(packet.size())),"allocate packet");
    std::memcpy(impl->packet->data,packet.data(),packet.size());
    av_frame_unref(impl->frame);
    CheckCodec(avcodec_send_packet(impl->context,impl->packet),"send picture");
    CheckCodec(avcodec_receive_frame(impl->context,impl->frame),"receive picture");
    const auto *frame=impl->frame;
    if (frame->format!=AV_PIX_FMT_YUV420P || frame->width!=static_cast<int>(width) || frame->height!=static_cast<int>(height) ||
        (frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags)
        throw std::runtime_error("NVDEC: decoded surface is corrupt or has an unexpected format/size");
    Bytes y(yPlane.Size()),uv(uvPlane.Size());
    for (unsigned row=0;row<height;++row)
        for (unsigned x=0;x<width;++x)
            y[yPlane.Offset(x,row)]=frame->data[0][static_cast<std::ptrdiff_t>(row)*frame->linesize[0]+x];
    for (unsigned row=0;row<(height+1)/2;++row)
        for (unsigned x=0;x<(width+1)/2;++x) {
            uv[uvPlane.Offset(2*x,row)]=frame->data[1][static_cast<std::ptrdiff_t>(row)*frame->linesize[1]+x];
            uv[uvPlane.Offset(2*x+1,row)]=frame->data[2][static_cast<std::ptrdiff_t>(row)*frame->linesize[2]+x];
        }
    memory.Write(yAddress,y); memory.Write(uvAddress,uv);
    if (statusAddress) {
        Bytes status(56);
        const auto macroblocks=((width+15)/16)*((height+15)/16);
        for (unsigned i=0;i<4;++i) status[i]=static_cast<std::uint8_t>(macroblocks>>(i*8));
        memory.Write(statusAddress,status); // zero error count/status, after complete surface writes
    }
}
}
