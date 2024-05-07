#pragma once

// std headers
#include <math.h>
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
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libswscale/swscale.h"
}

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/graph.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/video_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/vcodec.pb.h"

#define IMG_STREAM_INFO_WAIT_TIMEOUT 30000000

using namespace aup::avaf;
using namespace std;

class X86EncCalculator : public CalculatorBase<VideoCodecOptions>
{
	bool need_i_img_stream_info_side_packet = false;
	PacketPtr<VideoStreamInfoPacket> i_img_stream_info; // input image stream video information
	PacketPtr<VideoStreamInfoPacket> o_vid_stream_info; // output packet stream video information
	mutex m;
	uint32_t send_img_packet_cnt = 0;
	uint32_t recv_vpacket_cnt    = 0;
	AVCodecContext* pVcodecCtx   = NULL;
	AVFrame* pFrame   = NULL;
	AVPacket* pPacket = NULL;
	unordered_map<timestamp_t, timestamp_t> pts_to_sts;
	timestamp_t last_retrieved_pts = timestamp_min;

protected:
	ErrorCode fill_contract(shared_ptr<Contract>& contract, string& err_str) override;

public:
	X86EncCalculator(const Node* node) : CalculatorBase(node) {}
	virtual ~X86EncCalculator();
	aup::avaf::ErrorCode initialize(std::string& err_str) override;
	ErrorCode execute();
};