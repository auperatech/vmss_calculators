#ifndef NOTIFICATION_MESSAGE_H
#define NOTIFICATION_MESSAGE_H

#include "aup/avaf/calculator.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/json_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/notification_message.pb.h"

#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace aup::avaf;

class NotificationMessageCalculator : public CalculatorBase<NotificationMessageOptions>
{
public:
	NotificationMessageCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node) {}
	virtual ~NotificationMessageCalculator();

	ErrorCode execute() override;
	ErrorCode initialize(std::string& err_str) override;

protected:
	ErrorCode fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str) override;

private:
	// curl related variables
	CURL* curl = NULL;
	struct ReadData
	{
		explicit ReadData(const char* str)
		{
			source = str;
			size   = strlen(str);
		}
		const char* source;
		size_t size;
	};
	static size_t read_function(char* buffer, size_t size, size_t nitems, ReadData* data);
	std::vector<std::string> receivers;

	struct LocalDetectOptions
	{
		unsigned int min_objects;
		float min_confidence;
		unsigned int roi_x;
		unsigned int roi_y;
		unsigned int roi_w;
		unsigned int roi_h;
		unsigned int object_id;
	};

	struct LocalTrackOptions
	{
		unsigned int min_objects;
		float min_confidence;
		unsigned int roi_x;
		unsigned int roi_y;
		unsigned int roi_w;
		unsigned int roi_h;
		unsigned int object_id;
		unsigned int min_track_age;
		unsigned int max_track_age;
	};

	struct LocalCrowdFlowOptions
	{
		unsigned int min_total_entering;
		unsigned int min_total_exiting;
		unsigned int min_total_persons;
		unsigned int max_total_persons;
		float min_crowd_density;
		float max_crowd_density;
	};

	// jq related variables
	struct Trigger
	{
		enum class Type
		{
			PACKET     = 0,
			JQ         = 1,
			DETECTION  = 2,
			TRACK      = 3,
			CROWD_FLOW = 4,
		};
		// Constructor
		Trigger(Trigger::Type type, int consecutive_packet, const std::string& jq_query, const std::string& title,
		        const std::string& body, bool attach);
		Trigger(Trigger::Type type, int consecutive_packet, const NotificationMessageOptions_Trigger_ManualDetectOptions man_options, const std::string& title,
		        const std::string& body, bool attach);
		Trigger(Trigger::Type type, int consecutive_packet, const NotificationMessageOptions_Trigger_ManualTrackOptions manual_options, 
				const std::string& title, const std::string& body, bool attach);
		Trigger(Trigger::Type type, int consecutive_packet, const NotificationMessageOptions_Trigger_ManualCrowdFlowOptions manual_options, 
				const std::string& title, const std::string& body, bool attach);
		// Equality comparison operator
		bool operator==(const Trigger& other) const;

		Trigger::Type type             = Trigger::Type::PACKET;
		int trigger_consecutive_packet = 1;
		std::string jq_query_string    = "";
		LocalDetectOptions det_options;
		LocalTrackOptions track_options;
		LocalCrowdFlowOptions crowd_options;
		std::string notification_title = "";
		std::string notification_body  = "";
		bool attach_json               = false;
	};

	// Custom hash convert function
	struct TriggerHash
	{
		size_t operator()(const Trigger& t) const;
	};

	bool is_email_protocol = true;
	std::unordered_map<Trigger, std::pair<int, nlohmann::json>, TriggerHash> triggers;

	// threads related variables
	bool all_threads_running_ = false;
	std::vector<std::unique_ptr<std::thread>> notification_threads_;
	std::queue<std::pair<
		nlohmann::json,
		std::unordered_map<NotificationMessageCalculator::Trigger, std::pair<int, nlohmann::json>,
											 NotificationMessageCalculator::TriggerHash>::iterator>>
		notification_queue_;
	mutable std::mutex notification_q_lock_;
	std::vector<std::condition_variable> notification_q_conditions_;
	int notification_q_size_ = 1;
	int notification_q_idx_  = 0;
	std::string task_id_     = "";
	std::string notify       = "";

	// functions
	bool check_manual_detect_options(const LocalDetectOptions options, nlohmann::json query);
	bool check_manual_track_options(const LocalTrackOptions options, nlohmann::json query);
	bool check_manual_crowd_options(const LocalCrowdFlowOptions options, nlohmann::json query);
	nlohmann::json generate_notification_string(const nlohmann::json& j_packet,
	                             std::unordered_map<Trigger, std::pair<int, nlohmann::json>, TriggerHash>::iterator);
	void notification_worker(const int thread_idx);
	std::string execute_command(const char* cmd);
};

#endif // NOTIFICATION_MESSAGE_H
