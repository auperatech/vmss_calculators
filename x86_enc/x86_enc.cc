// declaration header
#include "x86_enc.h"

// std headers
#include <chrono>

// avaf headers
#include "aup/avaf/packets/av_codec_context_packet.h"
#include "aup/avaf/thread_name.h"

// #define AUP_AVAF_DBG_ENABLE (1)
#include "aup/avaf/avaf_dbg.h"

using namespace chrono;

X86EncCalculator::~X86EncCalculator()
{
	if (pFrame) {
		av_frame_free(&pFrame);
	}
	if (pPacket) {
		av_packet_free(&pPacket);
	}
	if (pVcodecCtx) {
		if (avcodec_is_open(pVcodecCtx)) {
			avcodec_free_context(&pVcodecCtx);
		} else {
			if (pVcodecCtx->extradata) {
				av_free(pVcodecCtx->extradata);
			}
			av_free(pVcodecCtx);
		}
	}
}

ErrorCode X86EncCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	if (contract->output_stream_names.size() != 2) {
		err_str = "node must have exactly two outputs";
		return ErrorCode::INVALID_CONTRACT;
	}
	need_i_img_stream_info_side_packet = !options->enc().w() || !options->enc().h() || !options->enc().fps();
	if (need_i_img_stream_info_side_packet && contract->input_stream_names.size() != 2) {
		err_str = "node requires side packets hence needs exactly two inputs streams";
		return ErrorCode::INVALID_CONTRACT;
	}
	if (!need_i_img_stream_info_side_packet && contract->input_stream_names.size() != 1) {
		err_str = "node does not require side packet so it needs exactly one input stream";
		return ErrorCode::INVALID_CONTRACT;
	}
	contract->sample_input_packets[0] = make_packet<ImagePacket>();
	if (need_i_img_stream_info_side_packet) {
		contract->sample_input_packets[1] = make_packet<VideoStreamInfoPacket>();
		contract->input_attrs_arr[1].set_type(contract->input_attrs_arr[1].SIDE_PACKET);
	}
	contract->sample_output_packets[0] = make_packet<VideoPacket>();
	contract->sample_output_packets[1] = make_packet<AVCodecContextPacket>();
	return ErrorCode::OK;
}

static int32_t fps_to_av_rational(float fps, AVRational* av_rational_fps)
{
	memset(av_rational_fps, 0, sizeof(AVRational));
	if (!(fps - roundf(fps))) {
		av_rational_fps->num = (int32_t)fps;
		av_rational_fps->den = 1;
	} else {
		float minOffset       = 60;
		int32_t idOfClosestTB = -1;
		av_rational_fps->num  = (float)1000 * ceilf(fps);
		for (int i = 0; i < 1000; i++) {
			float ifps    = (float)av_rational_fps->num / (float)(1000 + i);
			float iOffset = (std::max(ifps, fps) - std::min(ifps, fps));
			if (iOffset < minOffset) {
				minOffset     = iOffset;
				idOfClosestTB = i;
			}
		}
		av_rational_fps->den = 1000 + idOfClosestTB;
	}
	return 0;
}

ErrorCode X86EncCalculator::initialize(std::string& str_err)
{
	AVCodecID encId = AV_CODEC_ID_NONE;
	switch (options->enc().codec_type()) {
		case CODEC_TYPE_H264:
			encId = AV_CODEC_ID_H264;
			break;
		case CODEC_TYPE_H265:
			encId = AV_CODEC_ID_H265;
			break;
		case CODEC_TYPE_MPEG4:
			encId = AV_CODEC_ID_MPEG4;
			break;
		default:
			str_err = " Not setting correct codec type, only support x86_enc_h264/x86_enc_h265/x86_enc_mpeg4";
			return ErrorCode::ERROR;
	}

	if (need_i_img_stream_info_side_packet) {
		auto ec = node->dequeue_block(1, i_img_stream_info);
		if (ec != ErrorCode::OK) {
			str_err = "Issue reading side packet for stream info: " + to_string(ec);
			return ec;
		}
		if (i_img_stream_info->pixfmt != PIXFMT_I420) {

			str_err = "X86 encoder only accept I420 input pixfmt";
			return ErrorCode::ERROR;
		}
	}

	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, __func__ << " video codec encoder options:");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        name = " << CodecType_Name(options->enc().codec_type()));
	AUP_AVAF_LOG_NODE(
	    node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	    "        width = " << to_string((options->enc().w() > 0) ? options->enc().w() : i_img_stream_info->w));
	AUP_AVAF_LOG_NODE(
	    node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	    "        height = " << to_string((options->enc().h() > 0) ? options->enc().h() : i_img_stream_info->h));
	AUP_AVAF_LOG_NODE(
	    node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	    "        fps = " << to_string((options->enc().fps() > 0) ? options->enc().fps() : i_img_stream_info->fps));
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        queue size = " << options->enc().queue_size());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        b_adapt = " << options->enc().b_adapt());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        bframes = " << options->enc().bframes());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        gop_size = " << options->enc().gop_size());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        gop_mode = " << options->enc().gop_mode());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        bitrate = " << options->enc().bitrate());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        rc_mode = " << options->enc().rc_mode());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        threads = " << options->enc().threads());

	const AVCodec* pCodec = avcodec_find_encoder(encId);
	if (NULL == pCodec) {
		str_err = " Can not find encoder: " + string(avcodec_get_name(encId));
		return ErrorCode::ERROR;
	}
	pVcodecCtx = avcodec_alloc_context3(pCodec);
	if (NULL == pVcodecCtx) {
		str_err = "Fail to allocate encoder context";
		return ErrorCode::ERROR;
	}
	pVcodecCtx->codec_id     = encId;
	pVcodecCtx->codec_type   = AVMEDIA_TYPE_VIDEO;
	pVcodecCtx->pix_fmt      = AV_PIX_FMT_YUV420P;
#if AUP_AVAF_PLATFORM_IS_U30_HOST
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	pVcodecCtx->frame_number = 1;
#if AUP_AVAF_PLATFORM_IS_U30_HOST
#pragma GCC diagnostic pop
#endif
	pVcodecCtx->width        = (options->enc().w() > 0) ? options->enc().w() : i_img_stream_info->w;
	pVcodecCtx->height       = (options->enc().h() > 0) ? options->enc().h() : i_img_stream_info->h;
	fps_to_av_rational((options->enc().fps() > 0) ? options->enc().fps() : i_img_stream_info->fps,
	                   &pVcodecCtx->framerate);
	pVcodecCtx->time_base = av_inv_q(pVcodecCtx->framerate);
	//
	pVcodecCtx->gop_size = (options->enc().gop_size() > 0) ? options->enc().gop_size()
	                                                       : (pVcodecCtx->framerate.num / pVcodecCtx->framerate.den);
	//
	pVcodecCtx->max_b_frames = options->enc().bframes();
	// TODO Update approach to avoid using depricated function
#if !AUP_AVAF_PLATFORM_IS_U30_HOST
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	pVcodecCtx->b_frame_strategy = options->enc().b_adapt();
#pragma GCC diagnostic pop
#endif
	if (options->enc().bitrate() > 100000) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  __func__ << " enc use CBR ratecontrol mode target bitrate: "
		                           << to_string(options->enc().bitrate() / 1000) << " kbit/s");
		pVcodecCtx->bit_rate           = options->enc().bitrate();
		pVcodecCtx->rc_min_rate        = pVcodecCtx->bit_rate;
		pVcodecCtx->rc_max_rate        = pVcodecCtx->bit_rate;
		pVcodecCtx->bit_rate_tolerance = pVcodecCtx->bit_rate;
		pVcodecCtx->rc_buffer_size     = pVcodecCtx->bit_rate;
	} else if (!strcasecmp(options->enc().rc_mode().c_str(), "CRF")) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  __func__ << " enc use default ratecontrol mode crf(=23)");
		av_opt_set_int(pVcodecCtx->priv_data, "crf", 23, AV_OPT_SEARCH_CHILDREN);
	}
	pVcodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	pVcodecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
	pVcodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(pVcodecCtx, pCodec, NULL)) {
		str_err = "Could not open x86 encoder for " + string(avcodec_get_name(pVcodecCtx->codec_id));
		return ErrorCode::ERROR;
	}
	auto av_codec_context_packet = make_packet<AVCodecContextPacket>(pVcodecCtx);
	node->enqueue(1, av_codec_context_packet);
	if ((pFrame = av_frame_alloc()) == NULL) {
		str_err = "Issue allocating av_frame";
		return ErrorCode::NO_MEMORY;
	}
	if ((pPacket = av_packet_alloc()) == NULL) {
		str_err = "Issue allocating av_packet";
		return ErrorCode::NO_MEMORY;
	}
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	av_init_packet(pPacket);
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic pop
#endif
	return ErrorCode::OK;
}

ErrorCode X86EncCalculator::execute()
{
	AUP_AVAF_DBG("execute start");
	PacketPtr<const ImagePacket> vfrm;
	auto ec = node->get_packet(0, vfrm);
	if (ec != ErrorCode::OK) {
		return ec;
	}
	send_img_packet_cnt++;
	vfrm->get_ffmpeg_avframe(pFrame);
	// send the frame to encoder
	if (avcodec_send_frame(pVcodecCtx, pFrame)) {

		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " send " << send_img_packet_cnt << " vfrm failed, resolution:" << pFrame->width
		                           << "x" << pFrame->height << ", pts:" << vfrm->get_pres_timestamp());

	} else {

		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  __func__ << " send " << send_img_packet_cnt << " vfrm done, resolution:" << pFrame->width
		                           << "x" << pFrame->height << ", pts:" << vfrm->get_pres_timestamp());

		pts_to_sts[vfrm->get_pres_timestamp()] = vfrm->get_sync_timestamp();
	}
	// try to receive the encoded video packets
	while (true) {
		auto recv_packet_code = avcodec_receive_packet(pVcodecCtx, pPacket);
		if (recv_packet_code) {
			return ErrorCode::OK;
		}
		if (pPacket->size <= 0) {
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
			av_init_packet(pPacket);
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic pop
#endif
			continue;
		}
		recv_vpacket_cnt++;

		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  __func__ << " recv " << recv_vpacket_cnt << " vpkt done, len:" << pPacket->size
		                           << ", pts:" << pPacket->pts << ",dts:" << pPacket->dts);

		bool success;
		auto vpkt = make_packet<VideoPacket>(pts_to_sts[pPacket->pts], pPacket->dts, (uint32_t)pPacket->size, success);
		if (last_retrieved_pts != pPacket->pts) {
			pts_to_sts.erase(last_retrieved_pts);
		}
		last_retrieved_pts = pPacket->pts;
		vpkt->len          = pPacket->size;
		vpkt->picType      = (pPacket->flags & AV_PKT_FLAG_KEY) ? PicType::I : PicType::NONE;
		if (success) {
			vpkt->set_pres_timestamp(pPacket->pts);
			memcpy(vpkt->pBuf, pPacket->data, pPacket->size);
			if (node->enqueue(0, vpkt) != ErrorCode::OK) {
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
				                  __func__ << " vpacket(pts:" << vpkt->get_pres_timestamp() << ") enqueue to "
				                           << node->output_streams[0].first << " failed");
			}
		}

		av_packet_unref(pPacket);
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		av_init_packet(pPacket);
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic pop
#endif
	}
	return ErrorCode::OK;
}
AUP_AVAF_REGISTER_CALCULATOR("Aupera", "x86_enc", X86EncCalculator, VideoCodecOptions,
                             "Aupera's x86 encoder calculator.", {})
