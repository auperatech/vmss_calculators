#pragma once

#include "aup/avaf/config.h"

// std headers
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

// sdk headers
#include <boost/log/common.hpp>

// sdk headers (ffmpeg)
extern "C"
{
#include "libavcodec/avcodec.h"
#if AUP_AVAF_PLATFORM_IS_U30_HOST
#include "libavcodec/bsf.h"
#endif
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libswscale/swscale.h"
}

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/graph.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/av_codec_context_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/stream_mux.pb.h"

using namespace std;
using namespace aup::avaf;

#define FF_AVFORMATCTX_OPEN_WAIT 30000000 // 30s
#define FF_AVFORMATCTX_PROBESIZE 50000000 // 50M,max probe stream buffer size
#define FF_MAX_SUPPORT_FPS 90
#define EXTRADATA_MAX_LEN 256
#define ENC_CONTEXT_WAIT_TIMEOUT 30000000

class StreamMuxCalculator : public CalculatorBase<StreamMuxOptions>
{
	bool stream_open();
	void stream_close();
	bool stream_delay_open();
	PacketPtr<const AVCodecContextPacket> enc_ctx;
	mutex m;
	uint32_t writeVpktCnt = 0;
	string output_url;
	AVFormatContext* pFmtCtx            = NULL;
	int32_t vStreamIdx                  = 0;
	uint32_t vframeDuration             = 0;
	uint64_t ioOpenStartTs              = 0;
	AVBSFContext* pVBsfCtx              = NULL;
	const AVBitStreamFilter* pVBSFilter = NULL;
	bool avioIsOpen                     = false;
	bool ioWriteForceInterrupt          = false;
	AVPacket avpkt                      = {0};
	AVRational cq                       = {1, AV_TIME_BASE};
	uint32_t reconn_cont_failed_no      = 0;
	uint32_t write_cont_failed_no       = 0;

protected:
	ErrorCode fill_contract(shared_ptr<Contract>& contract, string& err_str) override;

public:
	StreamMuxCalculator(const Node* node);
	virtual ~StreamMuxCalculator();
	ErrorCode initialize(string& err_str) override;
	ErrorCode execute() override;

	friend int ff_interrupt_cb(void* opaque);
};

int ff_interrupt_cb(void* opaque);