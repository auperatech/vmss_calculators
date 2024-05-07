#include "notification_message.h"

using namespace cv;
using namespace std;
using json = nlohmann::json;

#define THROUGHPUT_NOTIFICATION 14
#define LPR_MLOPS_NOTIFICATION 15

/* ------- beginning of jq parser work, may be used later ------- */

// int NotificationMessageCalculator::run_rgx(string query, std::smatch &match, std::regex rgx)
// {
//   if (!std::regex_search(query, match, rgx)) {
//     return -1;
//   }
//   return match.size() - 1; // return number of matched groups
// }

// int NotificationMessageCalculator::jq_parser(string jq_query, json target, string& err_str)
// {
//   std::regex quote_rgx("'(.*)'");
//   std::regex select_rgx("\\s?select\\((.*)\\)\\s?");
//   std::regex map_rgx("\\s?map\\((.*)\\)\\s?");
//   std::regex to_entries_rgx("\\s?(to_entries)\\s?");
//   std::regex length_rgx("\\s?(length)\\s?");
//   std::regex entry_rgx("\\s?\\.([^\\s]*)\\s?");
//   std::regex gt_rgx("\\s?([^\\s]*)\\s?(>)\\s?([^\\s]*)\\s?");
//   std::regex gte_rgx("\\s?([^\\s]*)\\s?(>=)\\s?([^\\s]*)\\s?");
//   std::regex lt_rgx("\\s?([^\\s]*)\\s?(<)\\s?([^\\s]*)\\s?");
//   std::regex lte_rgx("\\s?([^\\s]*)\\s?(<=)\\s?([^\\s]*)\\s?");
//   std::regex eq("\\s?([^\\s]*)\\s?(==)\\s?([^\\s]*)\\s?");

//   std::smatch match;
//   if (run_rgx(jq_query, match, quote_rgx) != 1) {
//     err_str = "Invalid jq string";
//     return 1;
//   }
//   string quoteless = match[1];

// }

// Trigger Constructor
NotificationMessageCalculator::Trigger::Trigger(NotificationMessageCalculator::Trigger::Type type,
                                                int consecutive_packet, const std::string& jq_query,
                                                const std::string& title, const std::string& body, bool attach)
    : type(type), trigger_consecutive_packet(consecutive_packet), jq_query_string(jq_query), notification_title(title),
      notification_body(body), attach_json(attach)
{
}

NotificationMessageCalculator::Trigger::Trigger(NotificationMessageCalculator::Trigger::Type type, 
                                                int consecutive_packet, const NotificationMessageOptions_Trigger_ManualDetectOptions manual_options, 
												const std::string& title, const std::string& body, bool attach)
	: type(type), trigger_consecutive_packet(consecutive_packet), notification_title(title), notification_body(body), attach_json(attach)
{
	det_options.min_objects = manual_options.min_objects();
	det_options.min_confidence = manual_options.min_confidence();
	det_options.roi_x = manual_options.roi_x();
	det_options.roi_y = manual_options.roi_y();
	det_options.roi_w = manual_options.roi_w();
	det_options.roi_h = manual_options.roi_h(); 
	det_options.object_id = manual_options.object_id();
}

NotificationMessageCalculator::Trigger::Trigger(NotificationMessageCalculator::Trigger::Type type, 
                                                int consecutive_packet, const NotificationMessageOptions_Trigger_ManualTrackOptions manual_options, 
												const std::string& title, const std::string& body, bool attach)
	: type(type), trigger_consecutive_packet(consecutive_packet), notification_title(title), notification_body(body), attach_json(attach)
{
	track_options.min_objects = manual_options.min_objects();
	track_options.min_confidence = manual_options.min_confidence();
	track_options.roi_x = manual_options.roi_x();
	track_options.roi_y = manual_options.roi_y();
	track_options.roi_w = manual_options.roi_w();
	track_options.roi_h = manual_options.roi_h(); 
	track_options.object_id = manual_options.object_id();
	track_options.min_track_age = manual_options.min_track_age(); 
	track_options.max_track_age = manual_options.max_track_age();
}

NotificationMessageCalculator::Trigger::Trigger(NotificationMessageCalculator::Trigger::Type type, 
                                                int consecutive_packet, const NotificationMessageOptions_Trigger_ManualCrowdFlowOptions manual_options, 
												const std::string& title, const std::string& body, bool attach)
	: type(type), trigger_consecutive_packet(consecutive_packet), notification_title(title), notification_body(body), attach_json(attach)
{
	crowd_options.min_total_entering = manual_options.min_total_entering();
	crowd_options.min_total_exiting = manual_options.min_total_exiting();
	crowd_options.min_total_persons = manual_options.min_total_persons();
	crowd_options.max_total_persons = manual_options.max_total_persons();
	crowd_options.min_crowd_density = manual_options.min_crowd_density();
	crowd_options.max_crowd_density = manual_options.max_crowd_density();
}

// Trigger comparison operator
bool NotificationMessageCalculator::Trigger::operator==(const NotificationMessageCalculator::Trigger& other) const
{
	return type == other.type && trigger_consecutive_packet == other.trigger_consecutive_packet &&
	       jq_query_string == other.jq_query_string && notification_title == other.notification_title &&
	       notification_body == other.notification_body && attach_json == other.attach_json;
}

// Custom hash convert function
size_t NotificationMessageCalculator::TriggerHash::operator()(const NotificationMessageCalculator::Trigger& t) const
{
	return std::hash<int>()(static_cast<int>(t.type)) ^ std::hash<int>()(t.trigger_consecutive_packet) ^
	       std::hash<std::string>()(t.jq_query_string) ^ std::hash<std::string>()(t.notification_title) ^
	       std::hash<std::string>()(t.notification_body) ^ std::hash<bool>()(t.attach_json);
}

NotificationMessageCalculator::~NotificationMessageCalculator()
{
	// Clean up
	if (curl) {
		curl_easy_cleanup(curl);
		curl_global_cleanup();
	}

	// release all the threads
	{
		unique_lock<mutex> lock(notification_q_lock_);
		all_threads_running_ = false;
	}
	for (auto& notif : notification_q_conditions_) {
		notif.notify_all();
	}
	for (auto& notification_thread : notification_threads_) {
		if (notification_thread) {
			AUP_AVAF_THREAD_JOIN_NOTERM(*notification_thread);
		}
	}
}

ErrorCode NotificationMessageCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	uint32_t sz_input = (uint32_t)contract->input_stream_names.size();
	if (sz_input != 1) {
		err_str = "node must have exactly one input.";
		return ErrorCode::INVALID_CONTRACT;
	}
	vector<json> res;
	contract->sample_input_packets[0] = make_packet<JsonPacket>(0, res);

	return ErrorCode::OK;
}

ErrorCode NotificationMessageCalculator::initialize(std::string& err_str)
{
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m" << __func__ << " notification_message options:"
	                             << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " message_type = " << options->message_type() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " server_url = " << options->server_url() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " sender_username = " << options->sender_username() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " sender_password = " << options->sender_password() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " sender = " << options->sender() << "\033[0m");
	for (int i = 0; i < options->receiver_size(); i++) {
		// Add multiple receiver address to vector
		receivers.push_back(options->receiver(i));
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  "\033[36m"
		                      << " receiver " << i << " = " << options->receiver(i) << "\033[0m");
	}

	for (int i = 0; i < options->trigger_size(); i++) {
		// Trigger initialize and store it into map triggers
		Trigger::Type trig_type;
		if(options->trigger(i).trigger_type() == NotificationMessageOptions_Trigger_Type::NotificationMessageOptions_Trigger_Type_PACKET) {
			trig_type = Trigger::Type::PACKET;
			Trigger temp_trigger(trig_type, options->trigger(i).trigger_consecutive_packet(),
							options->trigger(i).jq_query_string(), options->trigger(i).notification_title(),
							options->trigger(i).notification_body(), options->trigger(i).attach_json());
			triggers[temp_trigger] = std::make_pair(0, json::object());
		} else if (options->trigger(i).trigger_type() == NotificationMessageOptions_Trigger_Type::NotificationMessageOptions_Trigger_Type_JQ) {
			trig_type = Trigger::Type::JQ;
			Trigger temp_trigger(trig_type, options->trigger(i).trigger_consecutive_packet(),
							options->trigger(i).jq_query_string(), options->trigger(i).notification_title(),
							options->trigger(i).notification_body(), options->trigger(i).attach_json());
			triggers[temp_trigger] = std::make_pair(0, json::object());
		} else if (options->trigger(i).trigger_type() == NotificationMessageOptions_Trigger_Type::NotificationMessageOptions_Trigger_Type_JSON_DETECTION) {
			trig_type = Trigger::Type::DETECTION;
			if(!options->trigger(i).has_manual_detect_options()) {
				err_str = "Specified type DETECTION but no manual_detect_options";
				return ErrorCode::ERROR;
			}
			Trigger temp_trigger(trig_type, options->trigger(i).trigger_consecutive_packet(),
							options->trigger(i).manual_detect_options(), options->trigger(i).notification_title(),
							options->trigger(i).notification_body(), options->trigger(i).attach_json());
			triggers[temp_trigger] = std::make_pair(0, json::object());
		} else if (options->trigger(i).trigger_type() == NotificationMessageOptions_Trigger_Type::NotificationMessageOptions_Trigger_Type_JSON_TRACK) {
			trig_type = Trigger::Type::TRACK;
			if(!options->trigger(i).has_manual_track_options()) {
				err_str = "Specified type TRACK but no manual_track_options";
				return ErrorCode::ERROR;
			}
			Trigger temp_trigger(trig_type, options->trigger(i).trigger_consecutive_packet(),
							options->trigger(i).manual_track_options(), options->trigger(i).notification_title(),
							options->trigger(i).notification_body(), options->trigger(i).attach_json());
			triggers[temp_trigger] = std::make_pair(0, json::object());
		} else if (options->trigger(i).trigger_type() == NotificationMessageOptions_Trigger_Type::NotificationMessageOptions_Trigger_Type_JSON_CROWD_FLOW) {
			trig_type = Trigger::Type::CROWD_FLOW;
			if(!options->trigger(i).has_manual_crowd_options()) {
				err_str = "Specified type CROWD_FLOW but no manual_crowd_options";
				return ErrorCode::ERROR;
			}
			Trigger temp_trigger(trig_type, options->trigger(i).trigger_consecutive_packet(),
							options->trigger(i).manual_crowd_options(), options->trigger(i).notification_title(),
							options->trigger(i).notification_body(), options->trigger(i).attach_json());
			triggers[temp_trigger] = std::make_pair(0, json::object());
		} else {
			err_str = "Unknown trigger type";
			return ErrorCode::ERROR;
		}
		if (trig_type == Trigger::Type::JQ || trig_type == Trigger::Type::PACKET) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  "\033[36m"
		                      << " trigger " << i << " info: "
		                      << "type = " << static_cast<int>(trig_type) << ", "
		                      << "trigger_consecutive_packet = " << options->trigger(i).trigger_consecutive_packet() << ", "
		                      << "jq_query_string = " << options->trigger(i).jq_query_string() << ", "
		                      << "notification_title = " << options->trigger(i).notification_title() << ", "
		                      << "notification_body = " << options->trigger(i).notification_body() << ", "
		                      << "attach_json = " << options->trigger(i).attach_json() << "\033[0m");
		} else {
			std::string manual_options;
			if (trig_type == Trigger::Type::DETECTION) {
				manual_options = options->trigger(i).manual_detect_options().DebugString();
			} else if (trig_type == Trigger::Type::TRACK) {
				manual_options = options->trigger(i).manual_track_options().DebugString();
			} else if (trig_type == Trigger::Type::CROWD_FLOW) {
				manual_options = options->trigger(i).manual_crowd_options().DebugString();
			}
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
							"\033[36m"
								<< " trigger " << i << " info: "
								<< "type = " << static_cast<int>(trig_type) << ", "
								<< "trigger_consecutive_packet = " << options->trigger(i).trigger_consecutive_packet() << ", "
								<< "manual_options = {\n" << manual_options << "}, "
								<< "notification_title = " << options->trigger(i).notification_title() << ", "
								<< "notification_body = " << options->trigger(i).notification_body() << ", "
								<< "attach_json = " << options->trigger(i).attach_json() << "\033[0m");
		}
	}

	// check task_id
	is_email_protocol =
	    options->message_type() == NotificationMessageOptions_MessageType::NotificationMessageOptions_MessageType_SMS
	        ? false
	        : true;
	task_id_ = node->get_task_id();
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " task_id = " << task_id_ << "\033[0m");
	if (task_id_.empty()) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " The graph-level task_id in pbtxt is empty, please set the value of it. ");
		err_str = "The graph-level task_id in pbtxt is empty, please set the value of it. ";
		return ErrorCode::ERROR;
	}

	if (options->sender().empty() || options->receiver(0).empty() || options->server_url().empty() ||
	    options->sender_username().empty() || options->sender_password().empty()) {
		AUP_AVAF_LOG_NODE(
		    node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		    __func__
		        << " The notification sender/receiver/username/password/server_url may be empty, please check them. ");
		err_str = "The notification sender/receiver/username/password/server_url may be empty, please check them. ";
		return ErrorCode::ERROR;
	}

	// Initialize curl
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();

	// Initialize working threads
	all_threads_running_ = true;
	std::vector<std::condition_variable> temp(notification_q_size_);
	notification_q_conditions_.swap(temp);
	for (int i = 0; i < notification_q_size_; i++) {
		notification_threads_.push_back(std::make_unique<std::thread>([this, i] { notification_worker(i); }));
	}

	return ErrorCode::OK;
}

size_t NotificationMessageCalculator::read_function(char* buffer, size_t size, size_t nitems, ReadData* data)
{
	size_t len = size * nitems;
	if (len > data->size) {
		len = data->size;
	}
	memcpy(buffer, data->source, len);
	data->source += len;
	data->size -= len;
	return len;
}

void NotificationMessageCalculator::notification_worker(const int thread_idx)
{
	while (all_threads_running_) {
		std::unique_lock<std::mutex> lock(notification_q_lock_);
		notification_q_conditions_.at(thread_idx).wait(lock, [this]() {
			return !notification_queue_.empty() || !all_threads_running_;
		});

		if (notification_queue_.empty()) {
			continue;
		}

		auto query = notification_queue_.front();
		notification_queue_.pop();

		lock.unlock();

		json cur_notif = generate_notification_string(query.first, query.second);
		if (cur_notif.empty()) {
			continue;
		}

		try {
			CURLcode res = CURLE_OK;
			curl_easy_setopt(curl, CURLOPT_URL, options->server_url().c_str());
			curl_easy_setopt(curl, CURLOPT_USERNAME, options->sender_username().c_str());
			curl_easy_setopt(curl, CURLOPT_PASSWORD, options->sender_password().c_str());

			if (is_email_protocol) {
				// Specify the email sender and recipient
				curl_easy_setopt(curl, CURLOPT_MAIL_FROM, options->sender().c_str());
				curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
				struct curl_slist* recipients = NULL;
				for (const auto& receiver : receivers) {
					recipients = curl_slist_append(recipients, receiver.c_str());
					curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

					// Set the function and data to read from
					std::string payload_text =
					    std::string("Date: Mon, 29 Nov 2010 21:54:29 +1100\r\n") + std::string("To: ") + receiver +
					    std::string("\r\n") + std::string("From: ") + options->sender() + std::string("\r\n") +
					    std::string("Subject:") + cur_notif.at("notification_title").get<std::string>() +
					    std::string("\r\n") + std::string("\r\n") + std::string(cur_notif.dump()) + std::string("\r\n");
					ReadData data(payload_text.c_str());
					curl_easy_setopt(curl, CURLOPT_READDATA, &data);
					curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_function);
					curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
					curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);

					res = curl_easy_perform(curl);
					if (res != CURLE_OK) {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
						                  __func__ << " Failed to send email notification: "
						                           << curl_easy_strerror(res));
						all_threads_running_ = false;
					} else {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_TRACE,
						                  __func__ << " Success to send email notification to " << receiver);
					}
				}
				// Clean up
				curl_slist_free_all(recipients);

			} else {
				for (const auto& receiver : receivers) {
					// Setup data to send SMS notification
					std::string payload_text = std::string("To=") + receiver + std::string("&") + std::string("From=") +
					                           options->sender() + std::string("&") + std::string("Body=") +
					                           cur_notif.dump() + std::string("\r\n");
					curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_text.c_str());
					curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
					curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
					res = curl_easy_perform(curl);
					if (res != CURLE_OK) {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
						                  __func__ << " Failed to send SMS notification: " << curl_easy_strerror(res));
						all_threads_running_ = false;
					} else {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_TRACE,
						                  __func__ << " Success to send SMS notification to " << receiver);
					}
				}
			}
		} catch (std::exception& e) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " Exception throw when in sending email/SMS notification: " << e.what());
		}
	}
}

bool NotificationMessageCalculator::check_manual_detect_options(const LocalDetectOptions options, json query)
{
	unsigned int pass_count = 0;
	if (query["item_count"] < options.min_objects) {
		return false;
	}
	for (auto& it : query["items"].items()) {
		if (options.min_confidence > 0.0) {
			if (query["items"][it.key()]["confidence"] < options.min_confidence) {
				continue;
			}
		}
		if (query["items"][it.key()]["x"] < options.roi_x || (unsigned int)(query["items"][it.key()]["x"]) + (unsigned int)(query["items"][it.key()]["width"]) > options.roi_x + options.roi_w) {
			continue;
		}
		if (query["items"][it.key()]["y"] < options.roi_y || (unsigned int)(query["items"][it.key()]["y"]) + (unsigned int)(query["items"][it.key()]["height"]) > options.roi_y + options.roi_h) {
			continue;
		}
		if (options.object_id > 0) {
			if (query["items"][it.key()]["box_id"] != options.object_id) {
				continue;
			}
		}
		pass_count++;
	}
	if (pass_count < options.min_objects) {
		return false;
	}
	return true;
}

bool NotificationMessageCalculator::check_manual_track_options(const LocalTrackOptions options, json query)
{
	unsigned int pass_count = 0;
	if (query["item_count"] < options.min_objects) {
		return false;
	}
	for (auto& it : query["items"].items()) {
		if (options.min_confidence > 0.0) {
			if (query["items"][it.key()]["confidence"] < options.min_confidence) {
				continue;
			}
		}
		if (query["items"][it.key()]["x"] < options.roi_x || (unsigned int)(query["items"][it.key()]["x"]) + (unsigned int)(query["items"][it.key()]["width"]) > options.roi_x + options.roi_w) {
			continue;
		}
		if (query["items"][it.key()]["y"] < options.roi_y || (unsigned int)(query["items"][it.key()]["y"]) + (unsigned int)(query["items"][it.key()]["height"]) > options.roi_y + options.roi_h) {
			continue;
		}
		if (options.object_id > 0) {
			if (query["items"][it.key()]["box_id"] != options.object_id) {
				continue;
			}
		}
		if (options.min_track_age > 0) {
			if (query["items"][it.key()]["track_age"] < options.min_track_age) {
				continue;
			}
		}
		if (options.max_track_age > 0) {
			if (query["items"][it.key()]["track_age"] > options.max_track_age) {
				continue;
			}
		}
		pass_count++;
	}
	if (pass_count < options.min_objects) {
		return false;
	}
	return true;
}

bool NotificationMessageCalculator::check_manual_crowd_options(const LocalCrowdFlowOptions options, json query)
{
	if (options.min_total_persons > 0) {
		if (query["total_persons_detected"] < options.min_total_persons) {
			return false;
		}
	}
	if (options.max_total_persons > 0) {
		if (query["total_persons_detected"] > options.max_total_persons) {
			return false;
		}
	}
	if (options.min_total_entering > 0) {
		if (query["total_persons_entering"] < options.min_total_entering) {
			return false;
		}
	}
	if (options.min_total_exiting > 0) {
		if (query["total_persons_exiting"] < options.min_total_exiting) {
			return false;
		}
	}
	if (options.min_crowd_density > 0.0) {
		if (query["crowd_density"] < options.min_crowd_density) {
			return false;
		}
	}
	if (options.max_crowd_density > 0.0) {
		if (query["crowd_density"] > options.max_crowd_density) {
			return false;
		}
	}
	return true;
}

std::string NotificationMessageCalculator::execute_command(const char* cmd)
{
	std::array<char, 512> buffer;
	std::string result;
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
	if (!pipe) {
		throw std::runtime_error("popen() failed!");
	}
	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
		result += buffer.data();
	}
	return result;
}

nlohmann::json NotificationMessageCalculator::generate_notification_string(
    const nlohmann::json& j_packet,
    std::unordered_map<NotificationMessageCalculator::Trigger, std::pair<int, nlohmann::json>,
                       NotificationMessageCalculator::TriggerHash>::iterator trigger)
{
	try {
		if (trigger->second.first == trigger->first.trigger_consecutive_packet) {
			trigger->second.first = 0;
			trigger->second.second.clear();
		}
		if (trigger->second.first == 0 && trigger->second.second.empty()) {
			json msg_base          = {{"task_id", task_id_},
			                          {"notification_title", trigger->first.notification_title},
			                          {"notification_body", trigger->first.notification_body},
			                          {"json_data", json::array()}};
			trigger->second.second = msg_base;
		}

		// construct jq/packet notifications
		unsigned long long int timestamp =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
		        .count();
		std::string packet_idx = "packet_" + std::to_string(trigger->second.first + 1);

		// Filter the packets depending on the trigger type and jq command
		if (trigger->first.type == Trigger::Type::DETECTION) {
			if (check_manual_detect_options(trigger->first.det_options, j_packet)) {
				json notify = {{packet_idx, j_packet}, {"timestamp", timestamp}};
				if (trigger->first.attach_json) {
					trigger->second.second["json_data"].push_back(notify);
				}
				trigger->second.first++;
			}
		} else if (trigger->first.type== Trigger::Type::TRACK) {
			if (check_manual_track_options(trigger->first.track_options, j_packet)) {
				json notify = {{packet_idx, j_packet}, {"timestamp", timestamp}};
				if (trigger->first.attach_json) {
					trigger->second.second["json_data"].push_back(notify);
				}
				trigger->second.first++;
			}
		} else if (trigger->first.type == Trigger::Type::CROWD_FLOW) {
			if (check_manual_crowd_options(trigger->first.crowd_options, j_packet)) {
				json notify = {{packet_idx, j_packet}, {"timestamp", timestamp}};
				if (trigger->first.attach_json) {
					trigger->second.second["json_data"].push_back(notify);
				}
				trigger->second.first++;
			}
		} else if (trigger->first.type == Trigger::Type::JQ) {
			std::string tmp_json_file_name = "/tmp/packet_" + std::to_string(timestamp) + ".json";
			std::ofstream tmp_file(tmp_json_file_name);
			if (!tmp_file.is_open()) {
				AUP_AVAF_LOG_NODE(
					node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
					__func__ << " Failed to open temporary json file, so unable to send message notifications.");
				return json::object();
			}
			tmp_file << j_packet;
			tmp_file.close();

			std::string jq_cmd = "jq " + trigger->first.jq_query_string + " " + tmp_json_file_name;
			// Execute jq command and capture output
			std::string res = execute_command(jq_cmd.c_str());
			if (!res.empty()) {
				json notify = {{packet_idx, j_packet}, {"timestamp", timestamp}};
				if (trigger->first.attach_json) {
					trigger->second.second["json_data"].push_back(notify);
				}
				trigger->second.first++;
			}
			// std::remove(tmp_json_file_name.c_str());
		} else if (trigger->first.type == Trigger::Type::PACKET) {
			// If trigger type is PACKET, we don't do jq filter
			json notify = {{packet_idx, j_packet}, {"timestamp", timestamp}};
			if (trigger->first.attach_json) {
				trigger->second.second["json_data"].push_back(notify);
			}
			trigger->second.first++;
		} else {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
					__func__ << ": Unkown trigger type");
			return json::object();
		}

		if (trigger->second.first == trigger->first.trigger_consecutive_packet) {
			return trigger->second.second;
		} else {
			return json::object();
		}
	} catch (nlohmann::json::exception& e) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call the generate_notification_string." << e.what());
		return json::object();
	} catch (const std::runtime_error& e) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call the generate_notification_string." << e.what());
		return json::object();
	}
}

ErrorCode NotificationMessageCalculator::execute()
{
	ErrorCode ec;
	PacketPtr<const JsonPacket> json_packets;

	if ((ec = node->get_packet(0, json_packets)) != ErrorCode::OK) {
		return ec;
	}
	// send json object to SMS/email server
	if (!json_packets->_json_object.empty()) {
		try {
			{ // make sure the lock is released before notifying
				std::lock_guard<mutex> lock(notification_q_lock_);

				// Construct notification message
				for (auto it = triggers.begin(); it != triggers.end(); ++it) {
					auto query = std::make_pair(json_packets->_json_object, it);
					notification_queue_.push(query);
					notification_q_conditions_.at(notification_q_idx_++).notify_one();
					if (notification_q_idx_ == notification_q_size_) {
						notification_q_idx_ = 0;
					}
				}
			}
		} catch (std::exception& e) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to call the notification_worker.");
			return ErrorCode::ERROR;
		}
	}

	return ErrorCode::OK;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "notification_message", NotificationMessageCalculator,
                             NotificationMessageOptions, "Aupera's notification message node.", {})
