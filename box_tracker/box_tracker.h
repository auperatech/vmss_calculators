#pragma once

// std headers
#include <chrono>
#include <memory>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// sdk headers
#include <opencv2/opencv.hpp>

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/graph.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/uint64_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/box_tracker.pb.h"
#include "aup/avaf/packets/track_packet.h"

// local headers
#include "detector.hpp"

using namespace cv;
using namespace std;
using namespace aup::avaf;
using namespace aup::detect;
using namespace aup::tracker;

class BoxTrackerCalculator : public CalculatorBase<BoxTrackerOptions>
{
	uint32_t detect_interval_stream_idx;
	PacketPtr<const UInt64Packet> detect_interval;
	vector<AupMultiTracker::track_obj_t> get_objs_to_track(PacketPtr<const DetectionPacket> detections);
	shared_ptr<AupMultiTracker> tracker = NULL;
	uint64_t minarea_rejected_count     = 0;
	uint64_t maxarea_rejected_count     = 0;

protected:
	ErrorCode fill_contract(shared_ptr<Contract>& contract, std::string& err_str) override;

public:
	BoxTrackerCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node) {}
	virtual ~BoxTrackerCalculator();
	ErrorCode initialize(string& err_str) override;
	ErrorCode execute() override;
};