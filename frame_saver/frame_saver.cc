// std headers
#include <filesystem>

// framework headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avap/frame_saver.pb.h"

using namespace std;
using namespace aup::avaf;
namespace fs = std::filesystem;

class FrameSaverCalculator : public CalculatorBase<FrameSaverOptions>
{
  uint32_t itr          = 0;
  uint32_t save_cnt     = 0;
  uint32_t interval_cnt = 0;

protected:
  ErrorCode fill_contract(shared_ptr<Contract>& contract, std::string& err_str) override;

public:
  FrameSaverCalculator(const Node* node) : CalculatorBase(node) {}
  ~FrameSaverCalculator() {}
  ErrorCode initialize(string& err_str) override;
  ErrorCode execute() override;
};

ErrorCode FrameSaverCalculator::fill_contract(shared_ptr<Contract>& contract, std::string& err_str)
{
  if (contract->sample_input_packets.size() != 1 || contract->sample_output_packets.size() != 0) {
    err_str = "node can have exactly one input and zero outputs.";
    return ErrorCode::INVALID_CONTRACT;
  }
  contract->sample_input_packets[0] = make_packet<ImagePacket>();
  return ErrorCode::OK;
}

ErrorCode FrameSaverCalculator::initialize(string& err_str)
{
  if (options->directory().empty()) {
    options->set_directory("./");
  }
  try {
    fs::create_directories(options->directory());
  } catch (const bad_alloc& exp) {
    err_str = "Bad allocation:" + string(exp.what());
    return ErrorCode::ERROR;
  } catch (const fs::filesystem_error& exp) {
    err_str = "Filesystem error:" + string(exp.what());
    return ErrorCode::ERROR;
  }
  if (options->directory_cleanup()) {
    for (const auto& entry : fs::directory_iterator(options->directory())) {
      fs::remove_all(entry.path());
    }
  }
  return ErrorCode::OK;
}

ErrorCode FrameSaverCalculator::execute()
{
  AUP_AVAF_DBG_TRACE_NODE(node);
  if (itr++ < options->save_offset()) {
    AUP_AVAF_DBG_TRACE_NODE(node);
    return ErrorCode::OK;
  }
  if (options->save_limit() != 0 && save_cnt >= options->save_limit()) {
    AUP_AVAF_DBG_TRACE_NODE(node);
    return ErrorCode::OK;
  }
  if (interval_cnt < options->save_skip()) {
	interval_cnt++;
	AUP_AVAF_DBG_TRACE_NODE(node);
	return ErrorCode::OK;
  }
  PacketPtr<ImagePacket const> image_packet;
  auto ec = node->get_packet(0, image_packet);
  if (ec != ErrorCode::OK) {
    AUP_AVAF_DBG_TRACE_NODE(node);
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      __FUNCTION__ << " failure in getting packet.");
    return ec;
  }
  AUP_AVAF_DBG_TRACE_NODE(node);
  switch (options->output_type()) {
    case FrameSaverOptions::RAW:
      image_packet->write_raw_disk(options->directory());
      break;
    case FrameSaverOptions::JPEG:
      image_packet->write_jpeg_disk(options->directory());
      break;
    default:
      AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                        "Invalid output_type in options");
      return ErrorCode::INVALID;
  }
  save_cnt++;
  interval_cnt = 0;
  return ErrorCode::OK;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "frame_saver", FrameSaverCalculator, FrameSaverOptions,
                             "Frame saver", {})
