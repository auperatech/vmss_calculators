// declration header
#include "stream_mux.h"

// std headers
#include <algorithm>
#include <chrono>

// avaf headers
#include "aup/avaf/packets/video_packet.h"
#include "aup/avaf/thread_name.h"

// #define AUP_AVAF_DBG_ENABLE (1)
#include "aup/avaf/avaf_dbg.h"

using namespace std::chrono;

StreamMuxCalculator::StreamMuxCalculator(const Node* node) : CalculatorBase(node) { ffmpeg_init(); }

StreamMuxCalculator::~StreamMuxCalculator() { stream_close(); }

// input index 0: video packet
// input index 1: side packet of AVCodecContext
ErrorCode StreamMuxCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	if (contract->output_stream_names.size() != 0) {
		err_str = "node does not have any output streams";
		return ErrorCode::INVALID_CONTRACT;
	}
	auto sz = contract->input_stream_names.size();
	if (sz != 2) {
		err_str = "node must have exactly two inputs";
		return ErrorCode::INVALID_CONTRACT;
	}
	contract->sample_input_packets[0] = make_packet<VideoPacket>();
	contract->sample_input_packets[1] = make_packet<AVCodecContextPacket>();
	contract->input_attrs_arr[1].set_type(GraphConfig::Node::InputStreamAttributes::SIDE_PACKET);
	return ErrorCode::OK;
}

bool StreamMuxCalculator::stream_delay_open()
{
	ErrorCode ec = node->dequeue_block(1, enc_ctx);
	if (ec != ErrorCode::OK) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __PRETTY_FUNCTION__ << " Fail to recv the encode context from upstream:"
		                                      << node->input_streams[1].first);
		return false;
	};

	return stream_open();
}

ErrorCode StreamMuxCalculator::execute()
{
	AUP_AVAF_TRACE();
	PacketPtr<const VideoPacket> rvpkt = nullptr;

	if (write_cont_failed_no > 100 && options->mux().auto_reconnect()) {
		stream_close();
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
		                  __func__ << " ready to reconnect output stream " << output_url);
		ioWriteForceInterrupt = 0;
		if (stream_open()) {
			uint32_t sleepMs = std::min(5 + (++reconn_cont_failed_no / 5) * 5, (uint32_t)60) * 1000;
			for (uint32_t i = 0; i < sleepMs; i++) {
				usleep(1000);
			}
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " reconnect output stream " << output_url << " failed");
		} else {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
			                  __func__ << " reconnect output stream " << output_url << " done");
			reconn_cont_failed_no = 0;
		}
		write_cont_failed_no = 0;
	}
	auto ec = node->get_packet(0, rvpkt);
	if (ec != ErrorCode::OK) {
		return ec;
	}

	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  __func__ << " muxer receive a packet, len:" << rvpkt->len
	                           << " pts:" << rvpkt->get_pres_timestamp());

	if (pFmtCtx) {
		writeVpktCnt++;
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		av_init_packet(&(avpkt));
#if AUP_AVAF_PLATFORM_IS_U30_HOST || AUP_AVAF_PLATFORM_IS_KRIA_SOM
#pragma GCC diagnostic pop
#endif
		avpkt.size         = rvpkt->len;
		avpkt.data         = rvpkt->pBuf;
		avpkt.stream_index = vStreamIdx;
		avpkt.duration     = av_rescale_q((int64_t)vframeDuration, cq, pFmtCtx->streams[vStreamIdx]->time_base);
		avpkt.flags        = (rvpkt->picType == PicType::I) ? AV_PKT_FLAG_KEY : 0;
		avpkt.pos          = -1;
		avpkt.pts = av_rescale_q((int64_t)rvpkt->get_pres_timestamp(), cq, pFmtCtx->streams[vStreamIdx]->time_base);
		avpkt.dts = av_rescale_q((int64_t)rvpkt->dts, cq, pFmtCtx->streams[vStreamIdx]->time_base);

		if (pVBsfCtx) {
			av_bsf_send_packet(pVBsfCtx, &avpkt);
			av_bsf_receive_packet(pVBsfCtx, &avpkt);
		}

		if (avpkt.size > 0) {
			/* mux encoded frame */
			m.lock();
			auto ret = av_interleaved_write_frame(pFmtCtx, &(avpkt));
			m.unlock();
			av_packet_unref(&avpkt);
			if (ret) {
				av_log(NULL, AV_LOG_ERROR, "Error muxing video packet\n");
				write_cont_failed_no++;
			} else {
				write_cont_failed_no = 0;
			}
		}
	}
	return ErrorCode::OK;
}

ErrorCode StreamMuxCalculator::initialize(string& err_str)
{
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, __func__ << " stream mux options:");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        rtsp_transport = " << options->mux().rtsp_transport());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        auto_reconnect = " << options->mux().auto_reconnect());
	output_url = node->get_output_url();
	if (output_url.empty()) {
		output_url = options->mux().output_url();
	}
	if (output_url.empty()) {
		err_str = "output url is empty";
		return ErrorCode::ERROR;
	}
	if (!stream_delay_open()) {
		err_str = "issue openning stream";
		return ErrorCode::ERROR;
	}
	return ErrorCode::OK;
}

int ff_interrupt_cb(void* opaque)
{
	StreamMuxCalculator* pOwner = (StreamMuxCalculator*)opaque;

	if (pOwner->ioWriteForceInterrupt ||
	    (!pOwner->avioIsOpen && (duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() -
	                             pOwner->ioOpenStartTs) > FF_AVFORMATCTX_OPEN_WAIT)) {
		return -1;
	}

	return 0;
}

bool StreamMuxCalculator::stream_open()
{
	AVStream* pStream       = NULL;
	const AVOutputFormat* pOutFmt = NULL;

	if (!strncmp(output_url.c_str(), "rtmp", sizeof("rtmp") - 1)) {
		avformat_alloc_output_context2(&pFmtCtx, NULL, "flv", output_url.c_str());    // RTMP
	} else if (!strncmp(output_url.c_str(), "udp", sizeof("udp") - 1)) {
		avformat_alloc_output_context2(&pFmtCtx, NULL, "mpegts", output_url.c_str()); // udp
	} else if (!strncmp(output_url.c_str(), "http", sizeof("http") - 1)) {
		avformat_alloc_output_context2(&pFmtCtx, NULL, "flv", output_url.c_str());    // http
	} else if (!strncmp(output_url.c_str(), "rtsp", sizeof("rtsp") - 1)) {
		avformat_alloc_output_context2(&pFmtCtx, NULL, "rtsp", output_url.c_str());   // rtsp
	} else {
		avformat_alloc_output_context2(&pFmtCtx, NULL, NULL, output_url.c_str());
	}

	if (NULL == pFmtCtx) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " avformat_alloc_output_context2 failed");
		return false;
	}

	pOutFmt                              = pFmtCtx->oformat;
	pFmtCtx->interrupt_callback.callback = ff_interrupt_cb;
	pFmtCtx->interrupt_callback.opaque   = this;
	pFmtCtx->strict_std_compliance       = FF_COMPLIANCE_EXPERIMENTAL;
	ioOpenStartTs                        = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();

	pStream = avformat_new_stream(pFmtCtx, NULL);
	if (NULL == pStream) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " Failed allocating output stream");
		return false;
	}

	pStream->r_frame_rate   = enc_ctx->ctx.framerate;
	pStream->avg_frame_rate = pStream->r_frame_rate;
	avcodec_parameters_from_context(pStream->codecpar, &enc_ctx->ctx); // important setting

	if (!(pOutFmt->flags & AVFMT_NOFILE)) {
		auto ret = avio_open2(&(pFmtCtx->pb), output_url.c_str(), AVIO_FLAG_WRITE, NULL, NULL);
		if (ret < 0) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " Could not open output file " << output_url);
			return false;
		}
	}

	/* init muxer, write output file header */
	AVDictionary* avopts = NULL;
	if (strstr(output_url.c_str(), ".mp4") || strstr(output_url.c_str(), ".mov")) {
		av_dict_set(&avopts, "movflags", "empty_moov", AV_DICT_APPEND);
	}
	if (!strncmp(output_url.c_str(), "rtsp://", sizeof("rtsp://") - 1) &&
	    !strcasecmp(options->mux().rtsp_transport().c_str(), "tcp")) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
		                  __func__ << " force output " << output_url << " rtsp transport to tcp");
		av_dict_set(&avopts, "rtsp_transport", "tcp", 0);
	}
	auto ret = avformat_write_header(pFmtCtx, &avopts);
	if (ret < 0) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " Error occurred when write output file header");
		return false;
	}

	avioIsOpen = 1;
	av_dump_format(pFmtCtx, 0, output_url.c_str(), 1);
	return true;
}

void StreamMuxCalculator::stream_close()
{
	if (pFmtCtx) {
		if (pFmtCtx->oformat && !(pFmtCtx->oformat->flags & AVFMT_NOFILE) && avioIsOpen) {
			avio_close(pFmtCtx->pb);
		}
		avformat_free_context(pFmtCtx);
		pFmtCtx = NULL;
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, __func__ << " close output avformat done");
	}
	if (pVBsfCtx) {
		av_bsf_free(&pVBsfCtx);
		pVBsfCtx = NULL;
	}
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "stream_mux", StreamMuxCalculator, StreamMuxOptions,
                             "Aupera's stream mux calclulator.", {})
