#pragma once

// std headers
#include <math.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// sdk headers
#include <boost/log/common.hpp>

// sdk headers (ffmpeg)
extern "C"
{
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libswscale/swscale.h"
}

// framework headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/graph.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/vfilter.pb.h"

#define VF_QUEUE_SIZE_DEFAULT 12
#define VF_QUEUE_SIZE_MAXIMUM 64
#define IMG_STREAM_INFO_WAIT_TIMEOUT 10000000

using namespace std;
using namespace aup::avaf;

class FpsControlValve
{
public:
	FpsControlValve(float srcFps_, float dstFps_)
	{
		src_fps = srcFps_;
		dst_fps = dstFps_;
	}
	~FpsControlValve() {}

	bool initialize()
	{
		if (src_fps > 0 && dst_fps > 0) {
			if (src_fps == dst_fps) {
				pic_recycle_of_node = 1;
				step_ration         = 1;
			} else if (src_fps > dst_fps) {
				pic_recycle_of_node = 0;
				step_ration         = src_fps / (src_fps - dst_fps);
			} else {
				step_ration = src_fps / (dst_fps - src_fps);
				while ((step_ration * (++pic_recycle_of_node)) < 1.0) {
				}
				step_ration *= pic_recycle_of_node;
				pic_recycle_of_node++;
			}

			fps_ration   = src_fps / dst_fps;
			nextProFrmId = step_ration;
		} else {
			return false;
		}
		return true;
	}

	int calc_recycle_cnt()
	{
		current_frame_id++;
		if (current_frame_id >= nextProFrmId) {
			nextProFrmId += step_ration;
			return pic_recycle_of_node;
		} else {
			return 1;
		}
	}

	int64_t calc_current_timestamp(int64_t src_ts)
	{
		if (fps_ration == 1) {
			return src_ts;
		} // once source fps equal to destination fps, output ts will always follow source

		int64_t out_ts = 0;
		if (src_ts != INT64_MAX) {
			// once cumulative frame number larger than target fps, the source ts to growth ts if source ts
			// exceed growth_ts
			if (!fb_till_next_sync || (fb_till_next_sync > dst_fps && src_ts > (int64_t)floor(growth_ts + 5000))) {
				growth_ts         = (double)src_ts;
				out_ts            = src_ts;
				fb_till_next_sync = 1;
			} else {
				growth_ts += (double)(1000000.0 / dst_fps);
				out_ts = (int64_t)floor(growth_ts);
				fb_till_next_sync++;
			}
		} else {
			out_ts = (int64_t)floor(growth_ts);
			growth_ts += (double)(1000000 / dst_fps);
		}
		return out_ts;
	}

private:
	float src_fps           = 0;
	float dst_fps           = 0;
	int current_frame_id    = -1;
	int pic_recycle_of_node = -1; // recycle times of current pic for fps controling node,0 means pic dropped
	float fps_ration        = 1;
	float step_ration       = 1;
	float nextProFrmId      = -1; // next process frame id,accumulated by step_ration
	int fb_till_next_sync   = 0;  // frame number till next ts
	double growth_ts        = 0;  // benchmark growth tts used to generate new ts for next frame
};

class FFVfilterCalculator : public CalculatorBase<VideoFilterOptions>
{
	MultiOutputScaler multi_output_scaler;
	unique_ptr<FpsControlValve> fps_ctrl_value_pts;
	unique_ptr<FpsControlValve> fps_ctrl_value_sts;
	shared_ptr<ImagePacket::Allocator> oimg_allocator;
	PacketPtr<VideoStreamInfoPacket> i_vid_strm_inf; // input image stream video information
	PacketPtr<VideoStreamInfoPacket> o_vid_strm_inf; // output image stream video information
	mutex m;
	int srcLinesize[4] = {0};

protected:
	ErrorCode fill_contract(shared_ptr<Contract>& contract, string& err_str) override;

public:
	FFVfilterCalculator(const Node* node) : CalculatorBase(node) {}
	virtual ~FFVfilterCalculator();
	ErrorCode initialize(string& err_str) override;
	ErrorCode execute() override;
};