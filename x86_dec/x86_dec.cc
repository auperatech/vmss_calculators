// declaration headers
#include "x86_dec.h"

// std headres
#include <chrono>

// avaf headres
#include "aup/avaf/packets/video_packet.h"

// #define AUP_AVAF_DBG_ENABLE (1)
#include "aup/avaf/avaf_dbg.h"

using namespace std::chrono;

X86DecCalculator::~X86DecCalculator()
{
	if (av_codec_ctx) {
		avcodec_free_context(&av_codec_ctx);
	}
}

ErrorCode X86DecCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	if (contract->input_stream_names.size() != 2 || contract->output_stream_names.size() < 2 ||
	    contract->output_stream_names.size() % 2 != 0 ||
	    (int)contract->output_stream_names.size() / 2 != options->dec().opixfmt_size()) {
		err_str = "node must have positive even number of outputs and exactly two inputs";
		return ErrorCode::INVALID_CONTRACT;
	}
	contract->sample_input_packets[0] = make_packet<VideoPacket>();
	auto video_stream_info_packet     = make_packet<VideoStreamInfoPacket>();
	contract->sample_input_packets[1] = video_stream_info_packet;
	contract->input_attrs_arr[1].set_type(GraphConfig::Node::InputStreamAttributes::SIDE_PACKET);
	auto image_packet = make_packet<ImagePacket>();
	for (int i = 0; i < (int)contract->sample_output_packets.size() / 2; i++) {
		contract->sample_output_packets[i] = image_packet;
	}
	for (int i = (int)contract->sample_output_packets.size() / 2; i < (int)contract->sample_output_packets.size();
	     i++) {
		contract->sample_output_packets[i] = video_stream_info_packet;
	}
	return ErrorCode::OK;
}

ErrorCode X86DecCalculator::execute()
{
	PacketPtr<const VideoPacket> vpacket = nullptr;
	ErrorCode ec;
	if (node->get_packet(0, vpacket) == ErrorCode::OK) {
		AUP_AVAF_DBG("dequeue. sts:" << vpacket->get_sync_timestamp() << " now:" << get_now_us());
		send_vpacket_cnt++;
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		av_init_packet(&send_vpacket);
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic pop
#endif
		send_vpacket.pts  = vpacket->get_pres_timestamp();
		send_vpacket.dts  = vpacket->dts;
		send_vpacket.size = vpacket->len;
		send_vpacket.data = vpacket->pBuf;
		send_vpacket.pos  = -1;
		if (avcodec_send_packet(av_codec_ctx, &send_vpacket)) {

			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " send " << send_vpacket_cnt << " vpkt failed, len:" << send_vpacket.size
			                           << ", pts:" << send_vpacket.pts);

		} else {

			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
			                  __func__ << " send " << send_vpacket_cnt << " vpkt done, len:" << send_vpacket.size
			                           << ", pts:" << send_vpacket.pts);
		}
		av_packet_unref(&send_vpacket);
	}

	// try to receive decoded vpics
	while (node->get_graph_status() == GraphStatus::RUNNING) {
		AVFrame* recv_avframe = av_frame_alloc();
		if (!recv_avframe) {
			return ErrorCode::NO_MEMORY;
		}
		// TODO handle INVAL error case (here assumption is that only error is AGAIN)
		if (!avcodec_receive_frame(av_codec_ctx, recv_avframe)) {
			recv_img_packet_cnt++;

			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
			                  __func__ << " recv " << recv_img_packet_cnt << " frame, pts:" << recv_avframe->pts);

			src_basic_image_packet->fill_with_ffmpeg_avframe(recv_avframe);
			src_basic_image_packet->set_pres_timestamp(recv_avframe->pts);

			vector<PacketPtr<ImagePacket>> calc_output_img_packets;
			/* allocate output vframes and do conversion with corresponding resolution and pixfmt */
			timestamp_t this_pts        = recv_avframe->pts + pts_offset;
			timestamp_t this_pts_now_us = get_now_us();
			if (this_pts <= last_pts) {
				timestamp_t pts_offset_change =
				    ((this_pts_now_us - last_pts_now_us) / frame_distance_us) * frame_distance_us;
				pts_offset += pts_offset_change;
				this_pts += pts_offset_change;
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
				                  AUP_AVAF_TERM_COLOR_FG_MAGENTA "PTS value dropped. Decoder will increase PTS offset "
				                                                 "value accordingly" AUP_AVAF_TERM_FORMAT_RESET_ALL);
			}
			if (this_pts <= last_pts) {
				timestamp_t pts_offset_change = last_pts - this_pts + frame_distance_us;
				pts_offset += pts_offset_change;
				this_pts += pts_offset_change;
			}
			bool image_packet_dropped = false;
			for (auto& allocator : image_allocators) {
				PacketPtr<ImagePacket> img_packet;
				while (node->get_graph_status() == GraphStatus::RUNNING) {
					img_packet = make_packet<ImagePacket>(this_pts, false, allocator, ec);
					if (ec == ErrorCode::OK) {
						calc_output_img_packets.push_back(img_packet);
						break;
					}
					if (options->drop_packet_on_full_data_stream()) {
						image_packet_dropped = true;
						av_frame_free(&recv_avframe);
						break;
					}
					usleep(100);
				}
				if (image_packet_dropped) {
					break;
				}
				if (node->get_graph_status() != GraphStatus::RUNNING) {
					return ErrorCode::OK;
				}
				img_packet->set_pres_timestamp(this_pts);
			}
			if (image_packet_dropped) {
				continue;
			}
			last_pts        = this_pts;
			last_pts_now_us = this_pts_now_us;
			multi_output_scaler.do_scale(src_basic_image_packet, calc_output_img_packets);
			for (uint32_t i = 0; i < (uint32_t)calc_output_img_packets.size(); i++) {
				auto& img_packet = calc_output_img_packets.at(i);
				img_packet->set_fps(i_vid_stream_info->fps);
				cv::Size img_dims;
				img_packet->get_dims(img_dims);
				if ((ec = node->enqueue(i, img_packet)) != ErrorCode::OK) {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
					                  "img_packet(pts:" << img_packet->get_pres_timestamp() << ") enqueue to "
					                                    << node->output_streams[i].first << " failed: " << ec);
				}
				AUP_AVAF_DBG("enqueue. sts:" << img_packet->get_sync_timestamp() << " now:" << get_now_us());
			}

			av_frame_free(&recv_avframe);
		} else {
			av_frame_free(&recv_avframe);
			break;
		}
	}
	return ErrorCode::OK;
}

ErrorCode X86DecCalculator::initialize(string& err_str)
{
	if (!options->dec().ow_size() || !options->dec().oh_size() || !options->dec().opixfmt_size()) {
		err_str = "Output width and height and pixfmt must be specified";
		return ErrorCode::ERROR;
	}

	if (options->dec().ow_size() != options->dec().opixfmt_size() ||
	    options->dec().oh_size() != options->dec().opixfmt_size()) {
		err_str = "Output width and height and pixfmt vector size not the same.";
		return ErrorCode::ERROR;
	}

	if ((int)node->output_streams.size() != options->dec().ow_size() * 2) {
		err_str = "Output image stream count not match with node required";
		return ErrorCode::ERROR;
	}

	// wait until demux node send out the side message(input stream info)
	auto ec = node->dequeue_block(1, i_vid_stream_info);
	if (ec != ErrorCode::OK) {
		err_str = "Could not dequeue side packet.";
		return ec;
	}
	frame_distance_us = (int)(1'000'000.f / i_vid_stream_info->fps);

	if (!i_vid_stream_info->w || !i_vid_stream_info->h) {
		err_str = "Fail to receive video stream info from side node.";
		return ErrorCode::ERROR;
	}

	AVCodecID codecId = AV_CODEC_ID_NONE;
	switch (i_vid_stream_info->codec_type) {
		case CODEC_TYPE_H264:
			codecId = AV_CODEC_ID_H264;
			break;
		case CODEC_TYPE_H265:
			codecId = AV_CODEC_ID_HEVC;
			break;
		default:
			err_str = "X86 only support h264 or h265 decoder type";
			return ErrorCode::ERROR;
	}

	o_vid_stream_infos.resize(options->dec().ow_size());
	for (auto& entry : o_vid_stream_infos) {
		entry = make_packet<VideoStreamInfoPacket>(*i_vid_stream_info.get());
	}
	string owstr      = "";
	string ohstr      = "";
	string opixfmtstr = "";
	for (auto i = 0; i < options->dec().ow_size(); i++) {
		o_vid_stream_infos[i]->w = options->dec().ow()[i] ?: i_vid_stream_info->w;
		o_vid_stream_infos[i]->h = options->dec().oh()[i] ?: i_vid_stream_info->h;
		o_vid_stream_infos[i]->pixfmt =
		    options->dec().opixfmt()[i] ? static_cast<PixFmt>(options->dec().opixfmt()[i]) : i_vid_stream_info->pixfmt;
		owstr += to_string(o_vid_stream_infos[i]->w) + " ";
		ohstr += to_string(o_vid_stream_infos[i]->h) + " ";
		opixfmtstr += to_string(o_vid_stream_infos[i]->pixfmt) + " ";
		if ((ec = node->enqueue(i + options->dec().ow_size(), o_vid_stream_infos[i])) != ErrorCode::OK) {
			err_str = "Failure sending side packet " + to_string(i);
			return ec;
		}
	}
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, __func__ << " video codec decoder options:");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, "        output width = " << owstr);
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, "        output height = " << ohstr);
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, "        output pixfmt = " << opixfmtstr);
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        queue size = " << options->dec().queue_size());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        low_latency = " << options->dec().low_latency());

	// configure the context for x86 decoder
	av_codec_ctx = avcodec_alloc_context3(NULL);
	if (NULL == av_codec_ctx) {
		err_str = "X86 decoder context allocate failed";
		return ErrorCode::ERROR;
	}
	// set necessary field for decoder context
	av_codec_ctx->framerate.num = floorf(i_vid_stream_info->fps);
	av_codec_ctx->framerate.den = 1;
	av_codec_ctx->width         = i_vid_stream_info->w;
	av_codec_ctx->height        = i_vid_stream_info->h;
	av_codec_ctx->time_base     = av_inv_q(av_codec_ctx->framerate);
	// find the decoder and open it
	const AVCodec* pCodec = avcodec_find_decoder(codecId);
	if (NULL == pCodec) {
		err_str = "Fail to find X86 decoder:" + string(avcodec_get_name(codecId));
		return ErrorCode::ERROR;
	}
	if (avcodec_open2(av_codec_ctx, pCodec, NULL)) {
		err_str = "Fail to open X86 decoder:" + string(avcodec_get_name(codecId));
		return ErrorCode::ERROR;
	}

	int queue_sz = (options->dec().queue_size() > 0)
	                   ? options->dec().queue_size()
	                   : ((i_vid_stream_info->iframe_extract) ? DEC_QUEUE_SIZE_I_EXTRACT : DEC_QUEUE_SIZE_DEFAULT);
	if (queue_sz > DEC_QUEUE_SIZE_MAXIMUM) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
		                  __func__ << " decode queue size auto modify to max allowed value:" << DEC_QUEUE_SIZE_MAXIMUM);
		queue_sz = DEC_QUEUE_SIZE_MAXIMUM;
	}
	// initialize the alloctors
	for (int32_t i = 0; i < (int32_t)o_vid_stream_infos.size(); i++) {
		ErrorCode ec = ErrorCode::OK;
		image_allocators.push_back(ImagePacket::Allocator::new_normal_allocator(
		    o_vid_stream_infos[i]->w, o_vid_stream_infos[i]->h, o_vid_stream_infos[i]->pixfmt, queue_sz, ec));
		if (ec != ErrorCode::OK) {
			err_str = "frame allocator initialize failed.";
			return ErrorCode::ERROR;
		}
	}

	if ((int32_t)o_vid_stream_infos.size() > 0 && !multi_output_scaler_init()) {
		err_str = "multi-output scaler initialize failed";
		return ErrorCode::ERROR;
	}
	src_basic_image_packet_allocator =
	    ImagePacket::Allocator::new_normal_allocator(av_codec_ctx->width, av_codec_ctx->height, PIXFMT_I420, 1, ec);
	if (ec != ErrorCode::OK) {
		err_str = "Issue creating src image allocator";
		return ec;
	}
	src_basic_image_packet = make_packet<ImagePacket>(0, false, src_basic_image_packet_allocator, ec);
	if (ec != ErrorCode::OK) {
		err_str = "Issue creating src image";
		return ec;
	}

	return ErrorCode::OK;
}

bool X86DecCalculator::multi_output_scaler_init()
{
	ErrorCode ec = ErrorCode::OK;
	auto src_image_allocator =
	    ImagePacket::Allocator::new_normal_allocator(av_codec_ctx->width, av_codec_ctx->height, PIXFMT_I420, 1, ec);
	if (ec != ErrorCode::OK) {
		return false;
	}
	auto src_img = make_packet<ImagePacket>(0, false, src_image_allocator, ec);
	if (ec != ErrorCode::OK) {
		return false;
	}
	vector<PacketPtr<ImagePacket>> dstVFrms;
	for (uint32_t i = 0; i < (uint32_t)image_allocators.size(); i++) {
		dstVFrms.emplace_back(make_packet<ImagePacket>(0, false, image_allocators.at(i), ec));
		if (ec != ErrorCode::OK) {
			return false;
		}
	}
	{
		cv::Size dims;
		src_img->get_dims(dims);
		for (auto& img : dstVFrms) {
			img->get_dims(dims);
		}
	}
	if (multi_output_scaler.initialize(src_img, dstVFrms) != ErrorCode::OK) {
		return false;
	}
	return true;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "x86_dec", X86DecCalculator, VideoCodecOptions,
                             "Aupera's x86 decoder calculator.", {})
