#pragma once

// sdd header
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// sdk headers
#include <boost/log/common.hpp>

// sdk headers (ffmpeg)
extern "C"
{
#include <libavcodec/avcodec.h>
}

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/graph.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/vcodec.pb.h"

using namespace std;
using namespace aup::avaf;

#define DEC_QUEUE_SIZE_I_EXTRACT 3
#define DEC_QUEUE_SIZE_DEFAULT 12
#define DEC_QUEUE_SIZE_MAXIMUM 1000
#define STREAM_INFO_WAIT_TIMEOUT 30'000'000

class X86DecCalculator : public CalculatorBase<VideoCodecOptions>
{
	bool multi_output_scaler_init();
	PacketPtr<VideoStreamInfoPacket> i_vid_stream_info;
	// The output image streams information after multiple-scaling
	vector<PacketPtr<VideoStreamInfoPacket>> o_vid_stream_infos;
	mutex m;
	AVCodecContext* av_codec_ctx = NULL;
	uint32_t send_vpacket_cnt    = 0;
	uint32_t recv_img_packet_cnt = 0;
	vector<shared_ptr<ImagePacket::Allocator>> image_allocators;
	MultiOutputScaler multi_output_scaler;
	AVPacket send_vpacket;
	PacketPtr<ImagePacket> src_basic_image_packet;
	std::shared_ptr<ImagePacket::Allocator> src_basic_image_packet_allocator;
	timestamp_t last_pts        = timestamp_min;
	timestamp_t last_pts_now_us = timestamp_min;
	timestamp_t pts_offset      = 0;
	int frame_distance_us       = 0;

protected:
	ErrorCode fill_contract(shared_ptr<Contract>& contract, string& err_str) override;

public:
	X86DecCalculator(const Node* node) : CalculatorBase(node) { ffmpeg_init(); }
	virtual ~X86DecCalculator();
	ErrorCode initialize(string& err_str) override;
	ErrorCode execute() override;
};