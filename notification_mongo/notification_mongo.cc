#include "notification_mongo.h"
#include "aup/avaf/config.h"


using namespace std;
using json = nlohmann::json;

NotificationMongoCalculator::~NotificationMongoCalculator()
{
	// clean up the mongoDB set up
	if (_uri) {
		mongoc_uri_destroy(_uri);
	}
	if (_pool) {
		mongoc_client_pool_destroy(_pool);
	}
	mongoc_cleanup();
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

ErrorCode NotificationMongoCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
	uint32_t sz_input = (uint32_t)contract->input_stream_names.size();
	if (sz_input != 1) {
		err_str = "node must have exactly one input";
		return ErrorCode::INVALID_CONTRACT;
	}
	vector<json> res;
	contract->sample_input_packets[0] = make_packet<JsonPacket>(0, res);

	return ErrorCode::OK;
}

ErrorCode NotificationMongoCalculator::initialize(std::string& err_str)
{
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m" << __func__ << " notification mongo options:"
	                             << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m"
	                      << " mongodb_address = " << options->mongodb_address() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m"
	                      << " database_name = " << options->database_name() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m"
	                      << " collection_name = " << options->collection_name() << "\033[0m");
	AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
	                  "\033[34m"
	                      << " notification_q_size = " << options->notification_q_size() << "\033[0m");

	mongodb_address_     = options->mongodb_address();
	database_name_       = options->database_name();
	collection_name_     = options->collection_name();
	notification_q_size_ = options->notification_q_size() ?: 1;

	if (!init()) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " MongoDB connection initialization failed: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_
		                           << " , collection_name: " << collection_name_);
		err_str = "MongoDB connection initialization failed.";
		return ErrorCode::ERROR;
	}

	all_threads_running_ = true;

#if AUP_AVAF_PLATFORM_IS_KRIA_SOM
	bson_error_t error;
	mongoc_client_t* client = nullptr;
	client = mongoc_client_new(mongodb_address_.c_str());
    if (!client) {
        AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to parse MongoDB URI: " << mongodb_address_);
		all_threads_running_ = false;
    }
	bool ping = mongoc_client_command_simple(client, "admin", BCON_NEW("ping", BCON_INT32(1)), NULL, NULL, &error);
    if (!ping) {
        AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " The mongoDB server is not available. " << error.message);
		all_threads_running_ = false;
    }
	if(!all_threads_running_)
	{
		mongoc_client_destroy(client);
    	mongoc_cleanup();
	} else {
		mongoc_client_destroy(client);
	}
#endif

	std::vector<std::condition_variable> temp(notification_q_size_);
	notification_q_conditions_.swap(temp);

	for (int i = 0; i < notification_q_size_; i++) {
		notification_threads_.push_back(std::make_unique<std::thread>([this, i] { notification_worker(i); }));
	}

	return ErrorCode::OK;
}

void NotificationMongoCalculator::notification_worker(const int thread_idx)
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

		if (!insert_a_doc(cur_notif)) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to insert json file to MongoDB server. ");
			all_threads_running_ = false;
		}
	}
}

bool NotificationMongoCalculator::init()
{
	mongoc_init();
	bson_error_t error;
	memset(&error, 0, sizeof(bson_error_t));
	_uri = mongoc_uri_new_with_error(mongodb_address_.c_str(), &error);

	if (!_uri) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to parse MongoDB URI: " << mongodb_address_
		                           << ", error message: " << error.message);
		return false;
	}

#if AUP_AVAF_PLATFORM_IS_KRIA_SOM
	bool is_mongoserver_available = true; 
	mongoc_client_t* client = nullptr;
	client = mongoc_client_new(mongodb_address_.c_str());
    if (!client) {
        AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to parse MongoDB URI: " << mongodb_address_);
		is_mongoserver_available = false;
    }
	bool ping = mongoc_client_command_simple(client, "admin", BCON_NEW("ping", BCON_INT32(1)), NULL, NULL, &error);
    if (!ping) {
        AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " The mongoDB server is not available. " << error.message);
		is_mongoserver_available = false;
    }
	if(!is_mongoserver_available)
	{
		mongoc_client_destroy(client);
    	mongoc_cleanup();
		return false;
	} else {
		mongoc_client_destroy(client);
	}
#endif

	_pool = mongoc_client_pool_new(_uri);

	if (!_pool) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to create mongoc client pool");
		return false;
	}

	if (false == mongoc_client_pool_set_error_api(_pool, 2)) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call mongoc_client_pool_set_error_api. ");
		return false;
	}

	return true;
}

bool NotificationMongoCalculator::insert_a_doc(const std::string& doc)
{
	bson_error_t error;
	bson_t* bjson                   = nullptr;
	mongoc_collection_t* collection = nullptr;

	memset(&error, 0, sizeof(error));

	mongoc_client_t* client = _pop_client_from_pool();
	if (!client) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call _pop_client_from_pool to pop doc from MongoDB server. ");
		return false;
	}

	bjson = bson_new_from_json((const uint8_t*)doc.c_str(), -1, &error);
	if (!bjson || error.code) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call bson_new_from_json to create an bson object from json. ");
		goto fail;
	}

	collection = mongoc_client_get_collection(client, database_name_.c_str(), collection_name_.c_str());
	if (!collection) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to get collection from MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_
		                           << " , collection_name: " << collection_name_);
		goto fail;
	}

	if (!mongoc_collection_insert_one(collection, bjson, nullptr, nullptr, &error)) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call mongoc_collection_insert_one to insert the doc: " << doc
		                           << " to the MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_ << " , collection_name: " << collection_name_
		                           << " error message: " << error.message);
		goto fail;
	} else {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
		                  "Success to insert docs to the MongoDB server: "
		                      << mongodb_address_ << ", "
		                      << "database_name: " << database_name_ << ", collection_name: " << collection_name_);
	}

	mongoc_collection_destroy(collection);
	_pust_client_to_pool(client);
	bson_destroy(bjson);
	return true;

fail:
	if (collection) {
		mongoc_collection_destroy(collection);
	}
	if (client) {
		_pust_client_to_pool(client);
	}
	if (bjson) {
		bson_destroy(bjson);
	}
	return false;
}

bool NotificationMongoCalculator::find_all_docs(std::list<std::string>& j_docs_list)
{
	mongoc_collection_t* collection = nullptr;
	mongoc_cursor_t* cursor         = nullptr;
	char* str                       = nullptr;
	const bson_t* doc;

	bson_t* query = bson_new();
	if (!query) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR, __func__ << " failed to call bson_new. ");
		return false;
	}

	mongoc_client_t* client = _pop_client_from_pool();
	if (!client) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call _pop_client_from_pool to pop doc from MongoDB server. ");
		goto fail;
	}

	collection = mongoc_client_get_collection(client, database_name_.c_str(), collection_name_.c_str());
	if (!collection) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to get collections from MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_
		                           << " , collection_name: " << collection_name_);
		goto fail;
	}

	cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);

	while (mongoc_cursor_next(cursor, &doc)) {
		str = bson_as_canonical_extended_json(doc, nullptr);
		j_docs_list.push_back(str);
		bson_free(str);
	}

	bson_destroy(query);
	mongoc_cursor_destroy(cursor);
	mongoc_collection_destroy(collection);
	_pust_client_to_pool(client);
	return true;

fail:
	if (cursor) {
		mongoc_cursor_destroy(cursor);
	}
	if (collection) {
		mongoc_collection_destroy(collection);
	}
	if (query) {
		bson_destroy(query);
	}
	if (client) {
		_pust_client_to_pool(client);
	}
	return false;
}

bool NotificationMongoCalculator::update_a_doc(const std::string& selector_str, const bson_t& update)
{
	mongoc_collection_t* collection = nullptr;
	mongoc_client_t* client         = nullptr;
	bson_error_t error;
	bson_t* selector = nullptr;

	client = _pop_client_from_pool();
	if (!client) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call _pop_client_from_pool to pop doc from MongoDB server. ");
		goto fail;
	}

	collection = mongoc_client_get_collection(client, database_name_.c_str(), collection_name_.c_str());
	if (!collection) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to get collections from MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_
		                           << " , collection_name: " << collection_name_);
		goto fail;
	}

	selector = bson_new_from_json((const uint8_t*)selector_str.c_str(), -1, &error);
	if (!selector) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call bson_new_from_json to create an bson object from json. ");
		goto fail;
	}

	if (!mongoc_collection_update_one(collection, selector, &update, nullptr, nullptr, &error)) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to update the doc: " << selector_str
		                           << " to the MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_ << " , collection_name: " << collection_name_
		                           << ". "
		                           << " error message: " << error.message);
		goto fail;
	}

	bson_destroy(selector);
	mongoc_collection_destroy(collection);
	_pust_client_to_pool(client);
	return true;

fail:
	if (selector) {
		bson_destroy(selector);
	}
	if (collection) {
		mongoc_collection_destroy(collection);
	}
	if (client) {
		_pust_client_to_pool(client);
	}
	return false;
}

bool NotificationMongoCalculator::delete_a_doc(const std::string& selector_json)
{
	mongoc_client_t* client         = NULL;
	mongoc_collection_t* collection = NULL;
	bson_t* selector                = NULL;
	bson_error_t error;

	client = _pop_client_from_pool();
	if (!client) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call _pop_client_from_pool to pop doc from MongoDB server. ");
		goto fail;
	}

	collection = mongoc_client_get_collection(client, database_name_.c_str(), collection_name_.c_str());
	if (!collection) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to get collections from MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_
		                           << " , collection_name: " << collection_name_);
		goto fail;
	}

	selector = bson_new_from_json((const uint8_t*)selector_json.c_str(), -1, &error);
	if (!selector) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call bson_new_from_json to create an bson object from json. ");
		goto fail;
	}

	if (!mongoc_collection_delete_one(collection, selector, NULL, NULL, &error)) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to delete the doc: " << selector_json
		                           << " in the MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_ << " , collection_name: " << collection_name_
		                           << ". "
		                           << " error message: " << error.message);
		goto fail;
	}

	bson_destroy(selector);
	mongoc_collection_destroy(collection);
	_pust_client_to_pool(client);
	return true;

fail:
	if (selector) {
		bson_destroy(selector);
	}
	if (collection) {
		mongoc_collection_destroy(collection);
	}
	if (client) {
		_pust_client_to_pool(client);
	}
	return false;
}

uint64_t NotificationMongoCalculator::get_docs_count(const std::string filter_str)
{
	mongoc_client_t* client         = nullptr;
	mongoc_collection_t* collection = nullptr;
	bson_error_t error;
	bson_t* filter = nullptr;
	int64_t count  = 0;

	client = _pop_client_from_pool();
	if (!client) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call _pop_client_from_pool to pop doc from MongoDB server. ");
		goto fail;
	}

	collection = mongoc_client_get_collection(client, database_name_.c_str(), collection_name_.c_str());
	if (!collection) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to get collections from MongoDB server: " << mongodb_address_ << ". "
		                           << " database_name: " << database_name_
		                           << " , collection_name: " << collection_name_);
		goto fail;
	}

	filter = bson_new_from_json((const uint8_t*)filter_str.c_str(), -1, &error);
	if (!filter) {
		AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		                  __func__ << " failed to call bson_new_from_json to create an bson object from json. ");
		goto fail;
	}

	// TODO Update approach to avoid using depricated function
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	count = mongoc_collection_count(collection, MONGOC_QUERY_NONE, filter, 0, 0, nullptr, &error);
#pragma GCC diagnostic pop

	if (-1 == count) {
		AUP_AVAF_LOG_NODE(
		    node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
		    __func__ << " failed to call mongoc_collection_estimated_document_count to get doc counts from server. ");
		goto fail;
	}

	bson_destroy(filter);
	mongoc_collection_destroy(collection);
	_pust_client_to_pool(client);

	return count;

fail:
	if (client) {
		_pust_client_to_pool(client);
	}
	if (collection) {
		mongoc_collection_destroy(collection);
	}
	if (filter) {
		bson_destroy(filter);
	}
	return -1;
}

mongoc_client_t* NotificationMongoCalculator::_pop_client_from_pool()
{
	if (!_pool)
		return nullptr;
	return mongoc_client_pool_pop(_pool);
}

void NotificationMongoCalculator::_pust_client_to_pool(mongoc_client_t* client)
{
	if (_pool && client) {
		mongoc_client_pool_push(_pool, client);
	}
}

ErrorCode NotificationMongoCalculator::execute()
{
	PacketPtr<const JsonPacket> json_packets;

	ErrorCode ec;
	if ((ec = node->get_packet(0, json_packets)) != ErrorCode::OK) {
		return ec;
	}
	// Insert json object to MongoDB collection
	if (!json_packets->_json_object.empty()) {
		try {
			{ // make sure the lock is released before notifying
				std::lock_guard<mutex> lock(notification_q_lock_);
				notification_queue_.push(json_packets->_json_object.dump());
			}
			notification_q_conditions_.at(notification_q_idx_++).notify_one();
			if (notification_q_idx_ == notification_q_size_) {
				notification_q_idx_ = 0;
			}
		} catch (std::exception& e) {
			AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
			                  __func__ << " failed to call the notification_worker. ");
			return ErrorCode::ERROR;
		}
	}

	return ErrorCode::OK;
}

AUP_AVAF_REGISTER_CALCULATOR("Aupera", "notification_mongo", NotificationMongoCalculator, NotificationMongoOptions,
                             "Aupera's notification MongoDB node", {})
