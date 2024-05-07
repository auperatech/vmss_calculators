#include "notification_web.h"

using namespace cv;
using namespace std;

#define THROUGHPUT_NOTIFICATION 14
#define LPR_MLOPS_NOTIFICATION 15

NotificationWebCalculator::~NotificationWebCalculator()
{
	close(sockfd_);
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

ErrorCode NotificationWebCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	uint32_t sz_input = (uint32_t)contract->input_stream_names.size();
	if (sz_input != 1) {
		err_str = "node must have exactly one input.";
		return ErrorCode::INVALID_CONTRACT;
	}
	vector<nlohmann::json> res;
	contract->sample_input_packets[0] = make_packet<JsonPacket>(0, res);

	return ErrorCode::OK;
}

ErrorCode NotificationWebCalculator::initialize(std::string& err_str)
{
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m" << __func__ << " notification_web options:"
	                             << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " notification_q_size = " << options->notification_q_size() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " protocol_type = " << options->protocol_type() << "\033[0m");

	// check notification_url and task_id
	is_http_protocol =
	    options->protocol_type() == NotificationWebOptions_ProtocolType::NotificationWebOptions_ProtocolType_UDP ? false
	                                                                                                             : true;
	task_id_ = node->get_task_id();
	notification_url_ =
	    options->notification_url().empty() ? node->get_notification_url() : options->notification_url();
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " task_id = " << task_id_ << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " notification_url = " << notification_url_ << "\033[0m");
	if (task_id_.empty()) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " The graph-level task_id in pbtxt is empty, please set the value of it. ");
		return ErrorCode::ERROR;
	}
	if (notification_url_.empty()) {
		AUP_AVAF_LOG_NODE(
		    node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		    __func__ << " Both graph-level and node-level notification_url are empty, please set the value of it.");
		return ErrorCode::ERROR;
	}
	// Initialize udp and http client
	if (is_http_protocol) {
		if (!parse_http_notification_url(notification_url_, http_notification_host_port_, http_notification_path_)) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to parse HTTP notification url.");
			return ErrorCode::ERROR;
		}
		client_ = std::make_unique<httplib::Client>(http_notification_host_port_.c_str());

	} else {
		if (!parse_udp_notification_url(notification_url_, udp_notification_host_, udp_notification_port_)) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to parse UDP notification url.");
			return ErrorCode::ERROR;
		}
		if (!init_udp_server(err_str)) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to init UDP client in notification_web node.");
			return ErrorCode::ERROR;
		}
	}

	// Initialize working threads
	notification_q_size_ = options->notification_q_size() ?: 1;
	all_threads_running_ = true;
	std::vector<std::condition_variable> temp(notification_q_size_);
	notification_q_conditions_.swap(temp);
	for (int i = 0; i < notification_q_size_; i++) {
		notification_threads_.push_back(std::make_unique<std::thread>([this, i] { notification_worker(i); }));
	}

	return ErrorCode::OK;
}

bool NotificationWebCalculator::parse_http_notification_url(const std::string& url, std::string& host_port,
                                                            std::string& path)
{
	if (url.substr(0, strlen("http://")) != "http://" && url.substr(0, strlen("https://")) != "https://") {
		return false;
	}

	size_t pos = url.find("//");
	if (pos + 2 == url.npos) {
		return false;
	}

	std::string ip_port_path = url.substr(pos + 2, url.npos);

	pos = ip_port_path.find("/");
	if (pos == ip_port_path.npos) {
		path      = "/";
		host_port = url;
	} else {
		path      = ip_port_path.substr(pos, ip_port_path.npos);
		host_port = url.substr(0, url.length() - path.length());
	}
	
	return true;
}

bool NotificationWebCalculator::parse_udp_notification_url(const std::string& url, std::string& host, std::string& port)
{
	if (url.substr(0, strlen("udp://")) != "udp://") {
		return false;
	}
	size_t pos = url.find("//");
	if (pos + 2 == url.npos) {
		return false;
	}
	std::string ip_port_path = url.substr(pos + 2, url.npos);
	size_t pos1              = ip_port_path.find(":");
	size_t pos2              = ip_port_path.find_first_of("/");
	host                     = ip_port_path.substr(0, pos1);
	port                     = ip_port_path.substr(pos1 + 1, pos2);
	return true;
}

bool NotificationWebCalculator::init_udp_server(std::string& err_str)
{
	try {
		sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
		if (sockfd_ == -1) {
			err_str = "failed to initialize udp server: invalid udp socket.";
			return false;
		}
		addr_.sin_family      = AF_INET;
		addr_.sin_port        = htons(std::stoi(udp_notification_port_));
		addr_.sin_addr.s_addr = inet_addr(udp_notification_host_.c_str());
		if (addr_.sin_addr.s_addr == INADDR_NONE) {
			close(sockfd_);
			err_str = "failed to initialize udp server: invalid ip ddress.";
			return false;
		}
		return true;
	} catch (std::exception& e) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " Exception throw when initializing udp server.");
		return false;
	}
}

void NotificationWebCalculator::notification_worker(const int thread_idx)
{
	while (all_threads_running_) {
		std::unique_lock<std::mutex> lock(notification_q_lock_);
		notification_q_conditions_.at(thread_idx).wait(lock, [this]() { 
			return !notification_queue_.empty() || !all_threads_running_;
		});

		if (notification_queue_.empty()) {
			continue;
		}

		auto cur_notif = notification_queue_.front();
		notification_queue_.pop();

		try {
			if (is_http_protocol) {
				httplib::Headers headers = {{"content-type", "application/json"}};
				if(!options->https_username().empty() && !options->https_password().empty()) {
					client_->enable_server_certificate_verification(false);
					client_->set_basic_auth(options->https_username(), options->https_password());
				}	
				if (auto res = client_->Post(http_notification_path_.c_str(), headers, cur_notif, "application/json"))
				{
					if (res->status == 200) {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_TRACE,
									__func__ << " Success to send notification to the HTTP server " << notification_url_);
					} else {
						AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_TRACE,
									__func__ << " Failed to send notification to the HTTP server " << notification_url_
											<< " with res status = " << res->status);
						all_threads_running_ = false;
					}
				} else {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
									__func__ << " Failed to send notification to the HTTP web server: " << res.error());
					all_threads_running_ = false;
				}
				
			} else {
				if (send(cur_notif)) {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_TRACE,
									__func__ << " Success to send notification to the UDP server "
											<< udp_notification_host_ << ":" << udp_notification_port_);
				} else {
					AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
									__func__ << " failed to send notification to the UDP web server.");
					all_threads_running_ = false;
				}
			}
		} catch (std::exception& e) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
							__func__ << " Exception throw when in sending notification: " << e.what());
		}
	}
}

bool NotificationWebCalculator::send(const std::string& data)
{
	auto res =
	    sendto(sockfd_, data.c_str(), data.length(), 0, reinterpret_cast<struct sockaddr*>(&addr_), sizeof(addr_));
	return res == -1 ? false : true;
}


std::string NotificationWebCalculator::generate_the_notif_string(const PacketPtr<const JsonPacket>& j_packets, 
					const nlohmann::json& j_packet)
{
	int command = THROUGHPUT_NOTIFICATION;
	if(j_packets->_usr_source == "mlops_notification") {
		command = LPR_MLOPS_NOTIFICATION;
	}
	unsigned long long int timestamp = (unsigned long long int)std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();
	notify = nlohmann::json {
		{"command", command},
		{"data", {
			{"timestamp", timestamp},
			{"task_id", task_id_},
			{"source", j_packets->_usr_source},
			{"user_payload", j_packet}}
		}
	}.dump();
	
	return notify;
}

ErrorCode NotificationWebCalculator::execute()
{
	ErrorCode ec;
	PacketPtr<const JsonPacket> json_packets;

	if ((ec = node->get_packet(0, json_packets)) != ErrorCode::OK) {
		return ec;
	}
	// send json object to web server
	if (!json_packets->_json_object.empty()) {
		try {
			{ // make sure the lock is released before notifying
				std::lock_guard<mutex> lock(notification_q_lock_);
				std::string notify = generate_the_notif_string(json_packets, json_packets->_json_object);
				notification_queue_.push(notify);
			}
			notification_q_conditions_.at(notification_q_idx_++).notify_one();
			if (notification_q_idx_ == notification_q_size_) {
				notification_q_idx_ = 0;
			}
		} catch (std::exception& e) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to call the notification_worker.");
			return ErrorCode::ERROR;
		}
	}

	return ErrorCode::OK;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "notification_web", NotificationWebCalculator, NotificationWebOptions,
                             "Aupera's notification web node.", {})
