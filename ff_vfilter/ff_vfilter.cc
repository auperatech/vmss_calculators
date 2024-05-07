// declaration header
#include "ff_vfilter.h"

// #define AUP_AVAF_DBG_ENABLE (1)
#include "aup/avaf/avaf_dbg.h"

FFVfilterCalculator::~FFVfilterCalculator() {}

ErrorCode FFVfilterCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	if (contract->input_stream_names.size() != 2 || contract->output_stream_names.size() != 2) {
		err_str = "input size and output size must be both 2";
		return ErrorCode::INVALID_CONTRACT;
	}
	contract->sample_input_packets[0] = make_packet<ImagePacket>();
	contract->sample_input_packets[1] = make_packet<VideoStreamInfoPacket>();
	contract->input_attrs_arr[1].set_type(GraphConfig::Node::InputStreamAttributes::SIDE_PACKET);
	contract->sample_output_packets[0] = make_packet<ImagePacket>();
	contract->sample_output_packets[1] = make_packet<VideoStreamInfoPacket>();
	return ErrorCode::OK;
}

ErrorCode FFVfilterCalculator::initialize(string& err_str)
{
	auto ec = node->dequeue_block(1, i_vid_strm_inf);
	if (ec != ErrorCode::OK) {
		err_str = "Issue getting input stream info";
		return ec;
	}
	o_vid_strm_inf         = make_packet<VideoStreamInfoPacket>();
	o_vid_strm_inf->w      = options->ow() ?: i_vid_strm_inf->w;
	o_vid_strm_inf->h      = options->oh() ?: i_vid_strm_inf->h;
	o_vid_strm_inf->pixfmt = options->opixfmt() ?: i_vid_strm_inf->pixfmt;
	o_vid_strm_inf->fps    = options->ofps() ?: i_vid_strm_inf->fps;
	auto src_img_allocator = ImagePacket::Allocator::new_normal_allocator(i_vid_strm_inf->w, i_vid_strm_inf->h,
	                                                                      i_vid_strm_inf->pixfmt, 1, ec);
	if (ec != ErrorCode::OK) {
		err_str = "Issue creating image allocator";
		return ec;
	}
	auto src_img = make_packet<ImagePacket>(0, false, src_img_allocator, ec);
	if (ec != ErrorCode::OK) {
		err_str = "Issue creating image";
		return ec;
	}
	int queue_sz = (options->queue_size() > 0) ? options->queue_size() : VF_QUEUE_SIZE_DEFAULT;
	if (queue_sz > VF_QUEUE_SIZE_MAXIMUM) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
		                  __func__ << " vfilter queue size auto modify to max allowed value:" << VF_QUEUE_SIZE_MAXIMUM);
		queue_sz = VF_QUEUE_SIZE_MAXIMUM;
	}
	oimg_allocator = ImagePacket::Allocator::new_normal_allocator(o_vid_strm_inf->w, o_vid_strm_inf->h,
	                                                              o_vid_strm_inf->pixfmt, queue_sz, ec);
	if (ec != ErrorCode::OK) {
		err_str = "frame allocator initialize failed";
		return ec;
	}
	vector<PacketPtr<ImagePacket>> dst_imgs;
	dst_imgs.emplace_back(make_packet<ImagePacket>(0, false, oimg_allocator, ec));
	if (ec != ErrorCode::OK) {
		err_str = "Issue creating dst image";
		return ec;
	}
	if ((ec = multi_output_scaler.initialize(src_img, dst_imgs)) != ErrorCode::OK) {
		err_str = "Issue creating multiscaler";
		return ec;
	}
	fps_ctrl_value_pts = make_unique<FpsControlValve>(i_vid_strm_inf->fps, o_vid_strm_inf->fps);
	if (!fps_ctrl_value_pts->initialize()) {
		err_str = "fps control valve initialize failed";
		return ErrorCode::ERROR;
	}
	fps_ctrl_value_sts = make_unique<FpsControlValve>(i_vid_strm_inf->fps, o_vid_strm_inf->fps);
	if (!fps_ctrl_value_sts->initialize()) {
		err_str = "fps control valve initialize failed";
		return ErrorCode::ERROR;
	}
	node->enqueue(1, o_vid_strm_inf);
	return ErrorCode::OK;
}

ErrorCode FFVfilterCalculator::execute()
{
	AUP_AVAF_DBG("execute started");
	PacketPtr<const ImagePacket> i_img_packet = nullptr;
	auto ec                                   = node->get_packet(0, i_img_packet);
	if (ec != ErrorCode::OK) {
		return ec;
	}
	AUP_AVAF_DBG("dequeue. pts:" << i_img_packet->get_pres_timestamp() << " sts:" << i_img_packet->get_sync_timestamp()
	                             << " now:" << get_now_us());
	int recycle_cnt = fps_ctrl_value_pts->calc_recycle_cnt();
	AUP_AVAF_DBG("recycle_cnt:" << recycle_cnt);
	fps_ctrl_value_sts->calc_recycle_cnt();
	for (int i = 0; i < recycle_cnt; i++) {
		AUP_AVAF_TRACE();
		auto pts = fps_ctrl_value_pts->calc_current_timestamp(i_img_packet->get_pres_timestamp());
		auto sts = fps_ctrl_value_sts->calc_current_timestamp(i_img_packet->get_sync_timestamp());
		vector<PacketPtr<ImagePacket>> oimgs;
		while (true) {
			oimgs.emplace_back(make_packet<ImagePacket>(sts, false, oimg_allocator, ec));
			if (ec == ErrorCode::OK) {
				oimgs.back()->set_pres_timestamp(pts);
				break;
			}
			oimgs.clear();
			usleep(100);
		}
		AUP_AVAF_TRACE();
		multi_output_scaler.do_scale(i_img_packet, oimgs);
		auto& oimg = oimgs[0];
		oimg->set_fps(o_vid_strm_inf->fps);
		if (node->enqueue(0, oimg) != ErrorCode::OK) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " vfrm(pts:" << oimg->get_pres_timestamp() << ") enqueue to "
			                           << node->output_streams[0].first << " failed");
		}
		AUP_AVAF_DBG("enqueue. sts:" << oimg->get_sync_timestamp() << " now:" << get_now_us());
		// TODO update logic so that image packet is not required to have an unsafe cast
		auto i_img_packet_unsafe = const_packet_cast<ImagePacket>(i_img_packet);
		i_img_packet_unsafe->set_pres_timestamp(
		    oimg->get_pres_timestamp()); // update current frame pts for next recycle
	}
	AUP_AVAF_DBG("execute finished");
	return ErrorCode::OK;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "ff_vfilter", FFVfilterCalculator, VideoFilterOptions,
                             "Aupera's vfilter using ffmpeg sws context.", {})
