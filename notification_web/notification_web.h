#ifndef NOTIFICATION_WEB_H
#define NOTIFICATION_WEB_H

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "aup/avaf/utils.h"
#include "aup/avaf/node.h"
#include "aup/avaf/calculator.h"
#include "aup/avaf/packets/json_packet.h"
#include "aup/avap/notification_web.pb.h"
#include "httplib.h"
#include <arpa/inet.h>
#include <sys/socket.h> // socket
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <opencv2/opencv.hpp>

#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <memory>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace aup::avaf;

class NotificationWebCalculator : public CalculatorBase<NotificationWebOptions>
{
public:

	NotificationWebCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node) {}
	virtual ~NotificationWebCalculator();

	ErrorCode execute() override;
	ErrorCode initialize(std::string& err_str) override;

	// UDP notification functions
	bool init_udp_server(std::string& err_str);
	bool send(const std::string& data);

protected:
	ErrorCode fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str) override;

private:
	bool parse_http_notification_url(const std::string& url, std::string& host_port, std::string& path);
	bool parse_udp_notification_url(const std::string& url, std::string& host, std::string& port);
	std::string generate_the_notif_string(const PacketPtr<const JsonPacket>& j_packets, const nlohmann::json& j_packet);
	void notification_worker(const int thread_idx);

	bool all_threads_running_ = false;
	std::vector<std::unique_ptr<std::thread>> notification_threads_;
	std::queue<std::string> notification_queue_;
	mutable std::mutex notification_q_lock_;
	std::vector<std::condition_variable> notification_q_conditions_;
	int notification_q_size_ = 1;
	int notification_q_idx_ = 0;
	bool is_http_protocol = true;
	std::string task_id_ = ""; 
	std::string notification_url_ = "";

	// UDP notification variables
	std::string udp_notification_host_;
	std::string udp_notification_port_;
	int sockfd_;
	struct sockaddr_in addr_;
	
	// HTTP notification variables
	std::string http_notification_host_port_;
	std::string http_notification_path_;
	std::unique_ptr<httplib::Client> client_;
	std::string notify;

};



#endif // NOTIFICATION_WEB_H
