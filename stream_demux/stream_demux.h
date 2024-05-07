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
#include "aup/avaf/packets/video_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/stream_mux.pb.h"

using namespace std;
using namespace aup::avaf;

#define FF_AVFORMATCTX_OPEN_WAIT 30000000 // 30s
#define FF_AVFORMATCTX_PROBESIZE 50000000 // 50M,max probe stream buffer size
#define FF_MAX_SUPPORT_FPS 90
#define EXTRADATA_MAX_LEN 256
#define IO_NETWORK_CRUISE_INTERVAL 10000000

class StreamDemuxCalculator : public CalculatorBase<StreamMuxOptions>
{
protected:
	ErrorCode fill_contract(shared_ptr<Contract>& contract, std::string& err_str) override;
	timestamp_t last_pts              = timestamp_max;
	timestamp_t sync_timestamp_offset = 0;

public:
	StreamDemuxCalculator(const Node* node);
	virtual ~StreamDemuxCalculator();
	void worker_thread();
	void packet_ingest_thread();
	ErrorCode initialize(string& err_str) override;
	bool stream_open();
	void stream_close();
	float get_authentic_fps();
	thread worker_thread_id;
	thread ingest_thread;
	bool running_ = false;
	PacketPtr<VideoStreamInfoPacket> vid_stream_info_packet;
	mutex m;
	uint32_t read_vpck_cnt        = 0;
	uint32_t read_vpck_cnt_period = 0;
	vector<PacketPtr<VideoPacket>> read_video_packets;
	string input_url;
	AVFormatContext* fmt_ctx                      = NULL;
	bool b_read_eof                               = false;
	bool got_1st_key_frame                        = false;
	int32_t v_stream_idx                          = -1;
	uint32_t v_frame_duration                     = 0;
	uint64_t io_open_start_ts                     = 0;
	AVBSFContext* av_bfs_ctx                      = NULL;
	const AVBitStreamFilter* av_bit_stream_filter = NULL;
	bool is_av_io_open                            = false;
	bool io_read_force_interrupt                  = false;
};