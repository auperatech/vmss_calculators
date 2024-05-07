#include "box_tracker.h"
#include "aup/avaf/thread_name.h"

// #define AUP_AVAF_DBG_ENABLE (1)
#include "aup/avaf/avaf_dbg.h"

BoxTrackerCalculator::~BoxTrackerCalculator() {}

ErrorCode BoxTrackerCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	uint32_t sz_input  = (uint32_t)contract->input_stream_names.size();
	uint32_t sz_output = (uint32_t)contract->output_stream_names.size();
	if (sz_input != 2) {
		err_str = "input size must be exactly 2";
		return ErrorCode::INVALID_CONTRACT;
	}
	if (sz_output != 1) {
		err_str = "output size cannot be greater than 1";
		return ErrorCode::INVALID_CONTRACT;
	}
	detect_interval_stream_idx = sz_input - 1;
	vector<aup::detect::Detector::DetectedObject> res;
	contract->sample_input_packets[0]                          = make_packet<DetectionPacket>(0, 0, 0, res, 0);
	contract->sample_input_packets[detect_interval_stream_idx] = make_packet<UInt64Packet>(0);
	contract->input_attrs_arr[detect_interval_stream_idx].set_type(
	    GraphConfig::Node::InputStreamAttributes::SIDE_PACKET);
	std::pair<std::vector<AupMultiTracker::track_obj_t>, std::vector<AupMultiTracker::track_obj_t>> tmp;
	contract->sample_output_packets[0] = make_packet<TrackPacket>(0, 0, 0, tmp, 0);

	return ErrorCode::OK;
}

ErrorCode BoxTrackerCalculator::execute()
{
	PacketPtr<const DetectionPacket> detections = nullptr;
	ErrorCode ec;

	if ((ec = node->get_packet(0, detections)) != ErrorCode::OK) {
		return ec;
	}

	try {
		if ((detections->frame_number % detect_interval->get_value()) == 0) {
			vector<AupMultiTracker::track_obj_t> objects_to_track = get_objs_to_track(detections);
			tracker->add(objects_to_track);
		} else {
			tracker->update();
		}
	} catch (exception& e) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_FATAL, "Tracker library exception: " << e.what());
		return ErrorCode::ERROR;
	}
	pair<vector<AupMultiTracker::track_obj_t>, vector<AupMultiTracker::track_obj_t>> live_and_deleted_tracks_pair =
	    tracker->getObjects();

	auto tracks = make_packet<TrackPacket>(detections->get_sync_timestamp(), detections->swidth, detections->sheight,
	                                       live_and_deleted_tracks_pair, detections->frame_number);

	ec = node->enqueue(0, tracks);
	if (ec != ErrorCode::OK) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_FATAL, "Enqueue error");
		return ec;
	}

	return ErrorCode::OK;
}

ErrorCode BoxTrackerCalculator::initialize(string& err_str)
{
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "box tracker options: " << options->DebugString());

	// wait until box_detector node send out the side message(detect interval)
	ErrorCode ec;
	if ((ec = node->dequeue_block(detect_interval_stream_idx, detect_interval)) != ErrorCode::OK) {
		err_str = "box_tracker_initialize did not receive detect interval. code: " + to_string(ec);
		return ec;
	}

	if (options->max_object_area_th() < 0) {
		err_str = " max_object_area_th must be >= 0, this is max_object_area_th in pixel_width*pixel_height";
		return ErrorCode::ERROR;
	} else if (options->max_object_area_th() == 0) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  "\033[34m"
		                      << "        max_object_area_th is set to zero which means there is no upper bound for "
		                         "accepted detection size.\033[0m");
	}

	if (options->fixed_box_size().w() > 0 && options->fixed_box_size().h() > 0) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  "\033[34m"
		                      << "        Tracking position distance will be computed with a fixed box size of ["
		                      << options->fixed_box_size().w() << ", " << options->fixed_box_size().h() << "].\033[0m");
	} else {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  "\033[34m"
		                      << "        fixed_box_size is empty, the position distance in the tracker will be "
		                         "calculated normally.\033[0m");
	}

	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m"
	                      << "        box_tracker received detect_interval = " << detect_interval
	                      << " on side packet.\033[0m");

	/**********************************************************************************************/
	// tracker setup
	/**********************************************************************************************/
	tracker = AupMultiTracker::create(AupMultiTracker::TrackerConfig{
	    .max_keep_alive         = options->max_keep_alive(),
	    .tracker_type           = options->tracker_type(),
	    .min_hits               = options->min_hits(),
	    .affinity_threshold     = options->affinity_threshold(),
	    .shape_weight           = options->shape_weight(),
	    .position_weight        = options->position_weight(),
	    .appearance_weight      = options->appearance_weight(),
	    .shape_dist_max         = options->shape_dist_max(),
	    .position_dist_max      = options->position_dist_max(),
	    .use_exp_cost           = options->use_exp_cost(),
	    .disable_tracker_update = options->disable_tracker_update(),
	    .fixed_box_size         = cv::Size2d(options->fixed_box_size().w(), options->fixed_box_size().h())});
	/**********************************************************************************************/

	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m"
	                      << "        box_tracker initialzied with input size: " << node->input_streams.size()
	                      << ", output size: " << node->output_streams.size() << "\033[0m");

	return (tracker != NULL) ? ErrorCode::OK : ErrorCode::ERROR;
}

vector<AupMultiTracker::track_obj_t>
BoxTrackerCalculator::get_objs_to_track(PacketPtr<const DetectionPacket> detections)
{
	vector<AupMultiTracker::track_obj_t> objects_2_track;
	for (size_t i = 0; i < detections->detections.size(); i++) {
		string name = "det_sts-" + to_string(detections->get_sync_timestamp()) + "_fn-" +
		              to_string(detections->frame_number) + "_ind-" + to_string(i);
		if ((detections->detections[i].rect.width * detections->detections[i].rect.height) <
		    options->min_object_area_th()) {
			minarea_rejected_count++;
		} else if (options->max_object_area_th() > 0 &&
		           detections->detections[i].rect.area() > options->max_object_area_th()) {
			maxarea_rejected_count++;
		} else {
			objects_2_track.push_back(AupMultiTracker::track_obj_t{.class_id   = detections->detections[i].class_id,
			                                                       .confidence = detections->detections[i].confidence,
			                                                       .rect       = detections->detections[i].rect,
			                                                       .meta_name  = name});
		}
	}
	return objects_2_track;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "box_tracker", BoxTrackerCalculator, BoxTrackerOptions,
                             "Aupera's box tracker calculator.", {})
