#ifndef NOTIFICATION_MONGO_H
#define NOTIFICATION_MONGO_H

#include <mongoc.h>
#include <bson.h>
#include "aup/avaf/calculator.h"
#include "aup/avaf/graph.h"
#include "aup/avaf/node.h"
#include "aup/avaf/utils.h"
#include "aup/avaf/packets/json_packet.h"
#include "aup/avap/notification_mongo.pb.h"

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <list>

using namespace aup::avaf;


class NotificationMongoCalculator : public CalculatorBase<NotificationMongoOptions>
{
public:
	NotificationMongoCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node) {}
	virtual ~NotificationMongoCalculator();

	ErrorCode execute() override;
	ErrorCode initialize(std::string& err_str) override;

	/**
     * @brief Connect mongondb server.
     * @return Returns true if init successfully, otherwise false is set.
     */
    bool init();

    /**
     * @brief Insert a document to collection.
     * @param doc_json Document with json format.
     * @return Returns true if insert successfully, otherwise false is set.
     */
    bool insert_a_doc(const std::string& doc_json);

    /**
     * @brief Find all documents from collection.
     * @param Output list of documents.
     * @return Returns true if find successfully, otherwise false is set.
     */
    bool find_all_docs(std::list<std::string>& docs_list);

    /**
     * @brief Update a document.
     * @param The selector with json format.
     * @param The update data.
     * @return Returns true if update successfully, otherwise false is set.
     */
    bool update_a_doc(const std::string& selector_json, const bson_t& update);

    /**
     * @brief Detete a document from collection.
     * @param The selector with json format.
     * @return Returns true if delete successfully, otherwise false is set.
     */
    bool delete_a_doc(const std::string& selector_json);

    /**
     * @brief Get count of doeument. To get count of all documents by default.
     * @param The filter with json format.
     * @return Returns true if get successfully, otherwise false is set.
     */
    uint64_t get_docs_count(const std::string filter_str = "{}");

protected:
	ErrorCode fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str) override;

private:
	mongoc_client_t* _pop_client_from_pool();
    void _pust_client_to_pool(mongoc_client_t* client);

    std::string mongodb_address_;
    std::string database_name_;
    std::string collection_name_;

    mongoc_client_pool_t *_pool;
    mongoc_uri_t *_uri;

    void notification_worker(const int thread_idx);
    bool all_threads_running_ = false;
    std::vector<std::unique_ptr<std::thread>> notification_threads_;
    std::queue<std::string> notification_queue_;
    mutable std::mutex notification_q_lock_;
    int notification_q_idx_ = 0;
    std::vector<std::condition_variable> notification_q_conditions_;
    int notification_q_size_ = 1;

};

#endif // NOTIFICATION_MONGO_H