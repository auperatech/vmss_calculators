#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "aup/avaf/calculator.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/json_packet.h"
#include "aup/avaf/utils.h"
#include "aup/avap/statistics_reader.pb.h"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace aup::avaf;
using namespace cv;

namespace fs = std::filesystem;

class StatisticsReaderCalculator : public CalculatorBase<StatisticsReaderOptions>
{
	thread io_service_thread;
	void io_service_worker() { io_service.run(); };
	boost::asio::io_service io_service;
	int64_t interval_int; // which initial value should be set for this ?
	uint64_t next_interval = 12345678;
	boost::posix_time::microseconds interval;
	unique_ptr<boost::asio::deadline_timer> timer;
	bool running              = false;
	bool is_file_input        = false;
	bool only_send_throughput = false;
	vector<std::string>::iterator json_path_itr;
	vector<std::string> json_paths;
	void send_json();

protected:
	ErrorCode fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str) override;

public:
	StatisticsReaderCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node), interval(0) {}
	~StatisticsReaderCalculator();
	ErrorCode initialize(std::string& err_str) override;
};

StatisticsReaderCalculator::~StatisticsReaderCalculator()
{
	io_service.stop();
	running = false;
	AUP_AVAF_THREAD_JOIN_NOTERM(io_service_thread);
}

ErrorCode StatisticsReaderCalculator::fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str)
{
	uint32_t sz_input = (uint32_t)contract->input_stream_names.size();
	if (sz_input != 0) {
		err_str = "node cannot have any input streams";
		return ErrorCode::INVALID_CONTRACT;
	}
	uint32_t sz_output = (uint32_t)contract->output_stream_names.size();
	if (sz_output != 1) {
		err_str = "node must have exactly one output stream";
		return ErrorCode::INVALID_CONTRACT;
	}
	vector<nlohmann::json> res;
	contract->sample_output_packets[0] = make_packet<JsonPacket>(0, res);
	return ErrorCode::OK;
}

ErrorCode StatisticsReaderCalculator::initialize(std::string& err_str)
{
	this->node = node;
	options    = make_unique<StatisticsReaderOptions>();
	for (const ::google::protobuf::Any& object : node->options) {
		if (object.Is<StatisticsReaderOptions>()) {
			if (!object.UnpackTo(options.get())) {
				AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
				                  "\033[33m" << __func__ << " Unable to unpack options of statistics reader.\033[0m");
				err_str = "Unable to unpack options of statistics reader.";
				return ErrorCode::ERROR;
			}
		} else {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  "\033[33m" << __func__ << " Options not match with statistics reader.\033[0m");
			err_str = "Options not match with statistics reader.";
			return ErrorCode::ERROR;
		}
	}

	interval_int  = options->interval_us();
	interval      = boost::posix_time::microseconds((boost::uint64_t)(interval_int));
	is_file_input = options->input_type() == StatisticsReaderOptions_InputType::StatisticsReaderOptions_InputType_FILE;
	only_send_throughput = options->only_send_throughput();
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m" << __func__ << " statistics_reader options:"
	                             << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " interval_us = " << options->interval_us() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " input_type = " << options->input_type() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " input_path = " << options->input_path() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " loop_over = " << options->loop_over() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[36m"
	                      << " only_send_throughput = " << options->only_send_throughput() << "\033[0m");

	if (is_file_input) {
		std::string json_path = options->input_path();
		std::string path      = json_path.substr(0, json_path.find_last_of('/'));
		if (access(path.c_str(), F_OK) < 0) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  "\033[33m" << __func__ << " The input path does not exist: " << path << "\033[0m");
			err_str = "The input path does not exist: " + std::string(path);
			return ErrorCode::ERROR;
		}
		if (json_path.substr(json_path.size() - 5) != ".json") {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  "\033[33m" << __func__ << " The input file is not a json file: " << json_path
			                             << "\033[0m");
			err_str = "The input file is not a json file: " + std::string(json_path);
			return ErrorCode::ERROR;
		}
		json_paths.push_back(json_path);
	} else {
		for (const auto& file : fs::directory_iterator(options->input_path())) {
			if (!file.is_regular_file()) {
				continue;
			}
			bool is_valid_file = true;
			if (boost::algorithm::to_lower_copy(file.path().extension().string()) != ".json") {
				is_valid_file = false;
			}
			if (is_valid_file) {
				json_paths.push_back(file.path().string());
			}
		}
		if (json_paths.empty()) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  "\033[33m" << __func__
			                             << "Statistics reader node did not find any json files in the directory: "
			                             << options->input_path() << "\033[0m");
			err_str = "Statistics reader node did not find any json files in the directory: " +
			          std::string(options->input_path());
			return ErrorCode::ERROR;
		}
	}

	json_path_itr = json_paths.begin();
	timer         = make_unique<boost::asio::deadline_timer>(io_service, interval);
	timer->async_wait(boost::bind(&StatisticsReaderCalculator::send_json, this));
	io_service_thread = thread([this] { io_service_worker(); });
	return ErrorCode::OK;
}

void StatisticsReaderCalculator::send_json()
{
	auto graph_status = node->get_graph_status();
	if (graph_status == GraphStatus::FAILED) {
		return;
	}
	if (graph_status == GraphStatus::FINISHED) {
		return;
	}
	if (graph_status == GraphStatus::RUNNING) {
		try {
			next_interval += interval_int;
			nlohmann::json j_statistics;
			std::ifstream file(*json_path_itr);
			if (file && file.peek() != std::ifstream::traits_type::eof()) {
				file >> j_statistics;
				if (only_send_throughput && j_statistics.is_object()) {
					if (j_statistics.contains("throughput") && j_statistics.contains("timestamp")) {
						nlohmann::json stats{
						    {"average_throughput_value", j_statistics["throughput"]["average_throughput_value"]},
						    {"timestamp", j_statistics["timestamp"]}};
						PacketPtr<JsonPacket> jpkg = make_packet<JsonPacket>(next_interval, stats);
						node->enqueue(0, jpkg);
					}
				} else {
					PacketPtr<JsonPacket> jpkg = make_packet<JsonPacket>(next_interval, j_statistics);
					node->enqueue(0, jpkg);
				}
			}

			if (!is_file_input) {
				if (++json_path_itr == json_paths.end()) {
					if (options->loop_over()) {
						json_path_itr == json_paths.begin();
					} else {
						return;
					}
				}
			}
		} catch (nlohmann::json::exception& e) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  "\033[33m" << __func__ << e.what() << "\033[0m");
		}
	}
	timer->expires_at(timer->expires_at() + interval);
	timer->async_wait(boost::bind(&StatisticsReaderCalculator::send_json, this));
}

AUP_AVAF_REGISTER_CALCULATOR_EXT("Aupera", "statistics_reader", StatisticsReaderCalculator, StatisticsReaderOptions,
                                 false, "Aupera's statistics reader calculator.", {})
