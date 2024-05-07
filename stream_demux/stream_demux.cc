// declaration header
#include "stream_demux.h"

// std headres
#include <algorithm>
#include <chrono>

// framework headers
#include "aup/avaf/thread_name.h"

using namespace std::chrono;

StreamDemuxCalculator::StreamDemuxCalculator(const Node* node) : CalculatorBase(node) { ffmpeg_init(); }

StreamDemuxCalculator::~StreamDemuxCalculator()
{
	running_ = false;
	AUP_AVAF_THREAD_JOIN_NOTERM(ingest_thread);
	AUP_AVAF_THREAD_JOIN_NOTERM(worker_thread_id);
	stream_close();
}

// index 0 is always the video packets
// index 1 if exists is going the be the side packet video stream info packet
ErrorCode StreamDemuxCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	if (contract->input_stream_names.size() != 0) {
		err_str = "node does not have any inputs";
		return ErrorCode::INVALID_CONTRACT;
	}
	auto sz = contract->output_stream_names.size();
	if (sz > 2 || sz < 1) {
		err_str = "node must have exactly one or two outputs.";
		return ErrorCode::INVALID_CONTRACT;
	}
	contract->sample_output_packets[0] = make_packet<VideoPacket>();
	if (sz == 2) {
		contract->sample_output_packets[1] = make_packet<VideoStreamInfoPacket>();
	}
	return ErrorCode::OK;
}

void StreamDemuxCalculator::packet_ingest_thread()
{
	AUP_AVAF_HANDLE_THREAD_NAME();
	int32_t ret      = -1;
	AVPacket avpkt   = {0};
	AVStream* stream = NULL;
	AVRational cq    = {1, AV_TIME_BASE};
	while (running_) {
		/* important: don't wait if input url is live stream,
		 * should keep receiving the packets to avoid network buffer excessive accumulation,
		 * and the packets in the beginning can be abandon to keep network packets fresh */
		if (node->get_graph_status() != GraphStatus::RUNNING && get_url_type(input_url) != UrlType::LIVE_STREAM) {
			usleep(10000);
			continue;
		}

		if (fmt_ctx && !b_read_eof) {
			ret = av_read_frame(fmt_ctx, &avpkt);
			if (ret < 0) {
				if (AVERROR_EOF == ret) {
					b_read_eof = true;
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
					                  __func__ << " read all packets, eof of current input stream");
					if (!options->demux().auto_reconnect()) {
						node->set_graph_status(GraphStatus::FINISHED);
					}
				} else {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
					                  __func__ << " error occurs in av_read_frame");
					continue;
				}
			}

			if (node->get_graph_status() != GraphStatus::RUNNING) {
				av_packet_unref(&(avpkt));
				continue;
			} // drop packets when graph is not running

			if (avpkt.flags != AV_PKT_FLAG_KEY && !got_1st_key_frame) {
				av_packet_unref(&(avpkt));
				continue;
			} else {
				got_1st_key_frame = true;
			}

			// enqueue vpkt, for h264/h265 vpkt, we convert it to annexb mode
			if (avpkt.size > 0 && avpkt.stream_index == v_stream_idx) {
				read_vpck_cnt++;
				m.lock();
				read_vpck_cnt_period++;
				m.unlock();

				stream = fmt_ctx->streams[v_stream_idx];

				if (av_bfs_ctx) {
					av_bsf_send_packet(av_bfs_ctx, &(avpkt));
					av_bsf_receive_packet(av_bfs_ctx, &(avpkt));
				}

				if (!avpkt.size) {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
					                  __func__ << " vbsf output packet size== 0,illegal,pts = " << avpkt.pts);
					av_packet_unref(&(avpkt));
					continue;
				}

				if ((avpkt.flags == AV_PKT_FLAG_KEY && options->demux().iframe_extract()) ||
				    !options->demux().iframe_extract()) {
					timestamp_t pts, dts;
					if (avpkt.pts == AV_NOPTS_VALUE) {
						pts = dts = v_frame_duration * read_vpck_cnt;
					} else {
						pts = av_rescale_q(avpkt.pts, stream->time_base, cq);
						dts = av_rescale_q(avpkt.dts, stream->time_base, cq);
					}
					if (last_pts == timestamp_max) {
						sync_timestamp_offset = get_now_us();
					} else if (pts < last_pts) {
						sync_timestamp_offset += last_pts - pts + 1;
					}
					last_pts = pts;
					bool success;
					auto vpacket = make_packet<VideoPacket>(pts + sync_timestamp_offset, dts,
					                                        avpkt.size + EXTRADATA_MAX_LEN, success);
					if (!success) {
						string er = "Issue in memory allocation.";
						throw runtime_error(er);
					}
					vpacket->set_pres_timestamp(pts);

					if (stream->codecpar->extradata_size > 0 && stream->codecpar->extradata_size < EXTRADATA_MAX_LEN &&
					    avpkt.flags == AV_PKT_FLAG_KEY) {
						memcpy(vpacket->pBuf, stream->codecpar->extradata, stream->codecpar->extradata_size);
						memcpy(vpacket->pBuf + stream->codecpar->extradata_size, avpkt.data, avpkt.size);
						vpacket->len = stream->codecpar->extradata_size + avpkt.size;
					} else {
						memcpy(vpacket->pBuf, avpkt.data, avpkt.size);
						vpacket->len = avpkt.size;
					}

					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
					                  __func__ << " [" << read_vpck_cnt << "]read one vpacket");
					m.lock();
					read_video_packets.push_back(vpacket);
					m.unlock();
				}
			}
			av_packet_unref(&avpkt);
		}

		usleep(1000);
	}
}

void StreamDemuxCalculator::worker_thread()
{
	AUP_AVAF_HANDLE_THREAD_NAME();
	uint64_t last_io_cruise_timestamp = UINT64_MAX;
	uint32_t reconnect_cont_failed_no = 0;
	while (running_) {
		if (node->get_graph_status() != GraphStatus::RUNNING) {
			usleep(10000);
			continue;
		}

		m.lock();
		if (read_video_packets.size() > 0) {
			ErrorCode ec;
			if ((ec = node->enqueue(0, read_video_packets.at(0))) != ErrorCode::OK) {
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
				                  __func__ << " vpacket(pts:" << read_video_packets[0]->get_pres_timestamp()
				                           << ") enqueue to " << node->output_streams[0].first
				                           << " failed. Error code:" << ec);
			}
			// std::cout <<"XXXX enqueue video_packet:" << *read_video_packets[0].get() << std::endl;
			read_video_packets.erase(read_video_packets.begin());
		}
		m.unlock();

		if (get_url_type(input_url) == UrlType::LIVE_STREAM) {
			if (fmt_ctx) {
				if (last_io_cruise_timestamp == UINT64_MAX) {
					last_io_cruise_timestamp =
					    duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
				}

				// check if many pkts lost,if so, means network is poor, consider to reconnect input stream
				if ((duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() -
				     last_io_cruise_timestamp) > IO_NETWORK_CRUISE_INTERVAL) {
					if (v_stream_idx >= 0 && options->demux().auto_reconnect() && !io_read_force_interrupt &&
					    (read_vpck_cnt_period <
					     (vid_stream_info_packet->fps / 8.0) *
					         ((duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() -
					           last_io_cruise_timestamp) /
					          1000000))) {
						io_read_force_interrupt = true;
						AUP_AVAF_LOG_NODE(
						    node, GraphConfig::LoggingFilter::SEVERITY_WARN,
						    __func__
						        << " input stream packet lost too much, force input stream interrupt and reconnect");
					}
					read_vpck_cnt_period = 0;
					last_io_cruise_timestamp =
					    duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
				}
			} else {
				read_vpck_cnt_period     = 0;
				last_io_cruise_timestamp = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
			}

			if (b_read_eof && options->demux().auto_reconnect()) {
				stream_close();
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
				                  __func__ << " ready to reconnect input stream " << input_url);
				io_read_force_interrupt = false;
				if (!stream_open()) {
					uint32_t sleepMs = std::min(5 + (++reconnect_cont_failed_no / 5) * 5, (uint32_t)60) * 1000;
					for (uint32_t i = 0; i < sleepMs && running_; i++) {
						usleep(1000);
					}
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
					                  __func__ << " reconnect input stream " << input_url << " failed");
				} else {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
					                  __func__ << " reconnect input stream " << input_url << " done");
					b_read_eof               = false;
					reconnect_cont_failed_no = 0;
				}
			}
		}

		usleep(1000);
	}
}

static int ff_interrupt_cb(void* opaque)
{
	StreamDemuxCalculator* owner = (StreamDemuxCalculator*)opaque;

	if (owner->io_read_force_interrupt || !owner->running_ ||
	    (!owner->is_av_io_open && (duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() -
	                               owner->io_open_start_ts) > FF_AVFORMATCTX_OPEN_WAIT)) {
		return -1;
	}

	return 0;
}

float StreamDemuxCalculator::get_authentic_fps()
{
	float dst_fps = 15;
	if (fmt_ctx && v_stream_idx < (int32_t)fmt_ctx->nb_streams) {
		AVStream* pStream = fmt_ctx->streams[v_stream_idx];
		if (NULL == pStream) {
			return dst_fps;
		}
		if (pStream->avg_frame_rate.num > 0 && pStream->avg_frame_rate.den > 0) {
			dst_fps = (float)pStream->avg_frame_rate.num / (float)pStream->avg_frame_rate.den;
		} else if (pStream->r_frame_rate.num > 0 && pStream->r_frame_rate.den > 0) {
			dst_fps = (float)pStream->r_frame_rate.num / (float)pStream->r_frame_rate.den;
		} else {
			pStream->avg_frame_rate.num = 15;
			pStream->avg_frame_rate.den = 1;
		}
		dst_fps = std::min((float)FF_MAX_SUPPORT_FPS, dst_fps);

		int ret                 = -1;
		uint32_t vid_packet_cnt = 0;
		uint64_t read_start     = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
		uint64_t first_vid_pts  = UINT64_MAX;
		std::list<int64_t> vid_pts_list;
		AVPacket avpkt = {0};
		AVRational cq  = {1, AV_TIME_BASE};
		while (1) {
			ret = av_read_frame(fmt_ctx, &avpkt);
			if (!ret && avpkt.stream_index == v_stream_idx) {
				if (avpkt.pts != AV_NOPTS_VALUE) {
					vid_pts_list.push_back(av_rescale_q(avpkt.pts, pStream->time_base, cq));
					if (first_vid_pts == UINT64_MAX) {
						first_vid_pts = avpkt.pts;
					}
				} else {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
					                  __func__ << " [" << vid_packet_cnt << "]current read pts is AV_NOPTS_VALUE");
				}
				vid_packet_cnt++;
			}
			av_packet_unref(&avpkt);

			if (vid_packet_cnt > 60 || (duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() -
			                            read_start) > 3000000) // max read vpkt is 60,max reading time is 3s
			{
				if (vid_pts_list.size() > 3) {
					vid_pts_list.sort(); // sort pts(avoid pts disorder if stream has B frame)
					std::list<int64_t>::iterator iter;
					std::list<int64_t>::iterator iter_;
					std::list<int64_t> intervalList;
					for (iter = vid_pts_list.begin(), iter_ = ++(vid_pts_list.begin()); iter_ != vid_pts_list.end();) {
						intervalList.push_back((int64_t)fabs(*iter - *iter_));
						iter++;
						iter_++;
					}
					intervalList.sort();

					int64_t last_pts_interval = INT64_MAX;
					std::list<std::pair<int64_t, uint32_t>> interval_cnt_map_list;
					for (iter = intervalList.begin(); iter != intervalList.end(); iter++) {
						if ((*iter) != last_pts_interval) {
							interval_cnt_map_list.push_back(std::make_pair((*iter), 1));
						} else {
							interval_cnt_map_list.back().second++;
						}
						last_pts_interval = (*iter);
					}

					auto most_map = *std::max_element(interval_cnt_map_list.begin(), interval_cnt_map_list.end(),
					                                  [](const auto& a, const auto& b) { return a.second < b.second; });

					float auth_fps = std::min((float)1000000 / (float)most_map.first, (float)FF_MAX_SUPPORT_FPS);
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
					                  __func__ << " video pts list size:" << vid_pts_list.size()
					                           << ", readFps:" << dst_fps << ", authentic FPS:" << auth_fps
					                           << ", occurrences:" << most_map.second);
					if (fabsf(auth_fps - dst_fps) > dst_fps * 0.1) {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
						                  __func__ << " correct fps from " << dst_fps << " to " << auth_fps);
						dst_fps = auth_fps;
					}
				}
				break;
			}
		}

		if (get_url_type(input_url) == UrlType::NATIVE_VIDEO) {
			if (first_vid_pts != UINT64_MAX) {
				ret = av_seek_frame(fmt_ctx, v_stream_idx, first_vid_pts, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD);
			} else {
				ret = avformat_seek_file(fmt_ctx, v_stream_idx, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_BYTE);
			}
			if (ret < 0) {
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
				                  __func__ << " file " << input_url << " seek back to the beginning failed");
			}
		}
	}
	return dst_fps;
}

ErrorCode StreamDemuxCalculator::initialize(string& err_str)
{
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, __func__ << " stream demux options:");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        rtsp_transport = " << options->demux().rtsp_transport());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        force_fps = " << options->demux().force_fps());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        iframe_extract = " << options->demux().iframe_extract());
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "        auto_reconnect = " << options->demux().auto_reconnect());

	running_ = true;

	input_url = node->get_input_url();
	if (!input_url.size()) {
		input_url = options->demux().input_url();
	}
	if (!input_url.size()) {
		err_str = "Cannot get input url";
		return ErrorCode::ERROR;
	}
	vid_stream_info_packet = make_packet<VideoStreamInfoPacket>();

	if (!stream_open()) {
		err_str = "issue opennig stream";
		return ErrorCode::ERROR;
	}
	vid_stream_info_packet->iframe_extract = options->demux().iframe_extract();

	if (node->output_streams.size() > 1) {
		auto ec = node->enqueue(1, vid_stream_info_packet);
		if (ec != ErrorCode::OK) {
			err_str = "issue enqueueing side packet: " + get_ec_desc(ec);
			return ec;
		}
	}
	if (options->demux().is_dummy()) {
		return ErrorCode::OK;
	}
	worker_thread_id = thread([&] { this->worker_thread(); });
	ingest_thread    = thread([&] { this->packet_ingest_thread(); });

	return ErrorCode::OK;
}

bool StreamDemuxCalculator::stream_open()
{
	AVStream* stream = NULL;
	fmt_ctx          = avformat_alloc_context();
	if (NULL == fmt_ctx) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " avformat_alloc_context failed");
		return false;
	}

	io_open_start_ts = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
	fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;
	fmt_ctx->interrupt_callback.callback = ff_interrupt_cb;
	fmt_ctx->interrupt_callback.opaque   = this;

	AVDictionary* avOpts = NULL;
	if (!strncmp(input_url.c_str(), "rtsp://", strlen("rtsp://")) && options->demux().rtsp_transport() == "tcp") {
		av_dict_set(&avOpts, "rtsp_transport", "tcp", 0);
	}

	if (avformat_open_input(&fmt_ctx, input_url.c_str(), NULL, &avOpts)) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " Can not probe input url:" << input_url.c_str());
		avformat_free_context(fmt_ctx);
		fmt_ctx = NULL;
		return false;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " find stream info failed:" << input_url.c_str());
		return false;
	}

	for (int i = 0; i < (int32_t)fmt_ctx->nb_streams; i++) {
		stream = fmt_ctx->streams[i];
		if (stream->codecpar->codec_id == AV_CODEC_ID_NONE) {
			continue;
		}
		if ((stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)) {
			if (!stream->avg_frame_rate.num || !stream->avg_frame_rate.den) {
				if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
					stream->avg_frame_rate = stream->r_frame_rate;
				} else {
					stream->avg_frame_rate.num = 25;
					stream->avg_frame_rate.den = 1;
				}
			}

			v_stream_idx = i;
			break;
		}
	}

	if (v_stream_idx >= 0) {
		stream                    = fmt_ctx->streams[v_stream_idx];
		vid_stream_info_packet->w = stream->codecpar->width;
		vid_stream_info_packet->h = stream->codecpar->height;
		vid_stream_info_packet->fps =
		    (options->demux().force_fps() > 0) ? options->demux().force_fps() : get_authentic_fps();
		// TODO Update approach to avoid using depricated function
#if !AUP_AVAF_PLATFORM_IS_U30_HOST
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
		vid_stream_info_packet->max_bframes = stream->codec->has_b_frames;
#pragma GCC diagnostic pop
#endif
		v_frame_duration = (int32_t)roundf((float)1000000 / vid_stream_info_packet->fps);

		switch (stream->codecpar->codec_id) {
			case AV_CODEC_ID_H264:
				vid_stream_info_packet->codec_type = CODEC_TYPE_H264;
				av_bit_stream_filter               = av_bsf_get_by_name("h264_mp4toannexb");
				break;
			case AV_CODEC_ID_HEVC:
				vid_stream_info_packet->codec_type = CODEC_TYPE_H265;
				av_bit_stream_filter               = av_bsf_get_by_name("hevc_mp4toannexb");
				break;
			default:
				av_bit_stream_filter = NULL;
				break;
		}

		if (av_bit_stream_filter) {
			if (av_bsf_alloc(av_bit_stream_filter, &(av_bfs_ctx))) {
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR, __func__ << " av bsf alloc failed");
				return false;
			}
			av_bfs_ctx->par_in->codec_id       = stream->codecpar->codec_id;
			av_bfs_ctx->par_in->extradata_size = stream->codecpar->extradata_size;
			av_bfs_ctx->par_in->extradata      = (uint8_t*)calloc(1, stream->codecpar->extradata_size);
			if (NULL == av_bfs_ctx->par_in->extradata) {
				return -1;
			}
			avcodec_parameters_copy(av_bfs_ctx->par_in, stream->codecpar);
			av_bsf_init(av_bfs_ctx);
		}
	} else {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " no available video stream found");
		return false;
	}

	is_av_io_open = 1;
	av_dump_format(fmt_ctx, 0, input_url.c_str(), 0);
	return true;
}

void StreamDemuxCalculator::stream_close()
{
	if (fmt_ctx) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = NULL;
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, __func__ << " close input avformat done");
	}
	if (av_bfs_ctx) {
		av_bsf_free(&av_bfs_ctx);
		av_bfs_ctx = NULL;
	}
}

AUP_AVAF_REGISTER_CALCULATOR_EXT("Aupera", "stream_demux", StreamDemuxCalculator, StreamMuxOptions, false,
                                 "Aupera's stream demux calclulator.", {})
