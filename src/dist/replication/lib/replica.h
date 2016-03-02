/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     replica interface, the base object which rdsn replicates
 *
 * Revision history:
 *     Mar., 2015, @imzhenyu (Zhenyu Guo), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

#pragma once

//
// a replica is a replication partition of a serivce,
// which handles all replication related issues
// and on_request the app messages to replication_app_base
// which is binded to this replication partition
//

# include <dsn/cpp/serverlet.h>
# include "replication_common.h"
# include "mutation.h"
# include "prepare_list.h"
# include "replica_context.h"

namespace dsn { namespace replication {

class replication_app_base;
class replica_stub;
class replication_checker;
namespace test {
    class test_checker;
}

using namespace ::dsn::service;

class replica : public serverlet<replica>, public ref_counter
{
public:        
    ~replica(void);

    //
    //    routines for replica stub
    //
    static replica* load(replica_stub* stub, const char* dir);
    static replica* newr(replica_stub* stub, const char* app_type, global_partition_id gpid);    
    // return true when the mutation is valid for the current replica
    bool replay_mutation(mutation_ptr& mu, bool is_private);
    void reset_prepare_list_after_replay();
    // return false when update fails or replica is going to be closed
    bool update_local_configuration_with_no_ballot_change(partition_status status);
    void set_inactive_state_transient(bool t);
    void check_state_completeness();
    //error_code check_and_fix_private_log_completeness();
    void close();

    //
    //    requests from clients
    // 
    void on_client_write(task_code code, dsn_message_t request);
    void on_client_read(task_code code, dsn_message_t request);

    //
    //    messages and tools from/for meta server
    //
    void on_config_proposal(configuration_update_request& proposal);
    void on_config_sync(const partition_configuration& config);
            
    //
    //    messages from peers (primary or secondary)
    //
    void on_prepare(dsn_message_t request);    
    void on_learn(dsn_message_t msg, const learn_request& request);
    void on_learn_completion_notification(const group_check_response& report);
    void on_add_learner(const group_check_request& request);
    void on_remove(const replica_configuration& request);
    void on_group_check(const group_check_request& request, /*out*/ group_check_response& response);
    void on_copy_checkpoint(const replica_configuration& request, /*out*/ learn_response& response);

    //
    //    messsages from liveness monitor
    //
    void on_meta_server_disconnected();
    
    //
    //  routine for testing purpose only
    //
    void send_group_check_once_for_test(int delay_milliseconds);
    
    //
    //  local information query
    //
    ballot get_ballot() const {return _config.ballot; }    
    partition_status status() const { return _config.status; }
    global_partition_id get_gpid() const { return _config.gpid; }    
    replication_app_base* get_app() { return _app.get(); }
    decree max_prepared_decree() const { return _prepare_list->max_decree(); }
    decree last_committed_decree() const { return _prepare_list->last_committed_decree(); }
    decree last_prepared_decree() const;
    decree last_durable_decree() const;    
    const std::string& dir() const { return _dir; }
    bool group_configuration(/*out*/ partition_configuration& config) const;
    uint64_t last_config_change_time_milliseconds() const { return _last_config_change_time_ms; }
    const char* name() const { return _name; }
    mutation_log_ptr private_log() const { return _private_log; }

    void json_state(std::stringstream& out) const;
    void update_commit_statistics(int count);
        
private:
    // common helpers
    void init_state();
    void response_client_message(dsn_message_t request, error_code error, decree decree = -1);    
    void execute_mutation(mutation_ptr& mu);
    mutation_ptr new_mutation(decree decree);    
        
    // initialization
    replica(replica_stub* stub, global_partition_id gpid, const char* app_type, const char* dir);
    error_code initialize_on_new();
    error_code initialize_on_load();
    error_code init_app_and_prepare_list(bool create_new);
        
    /////////////////////////////////////////////////////////////////
    // 2pc
    void init_prepare(mutation_ptr& mu);
    void send_prepare_message(::dsn::rpc_address addr, partition_status status, mutation_ptr& mu, int timeout_milliseconds, int64_t learn_signature = invalid_signature);
    void on_append_log_completed(mutation_ptr& mu, error_code err, size_t size);
    void on_prepare_reply(std::pair<mutation_ptr, partition_status> pr, error_code err, dsn_message_t request, dsn_message_t reply);
    void do_possible_commit_on_primary(mutation_ptr& mu);    
    void ack_prepare_message(error_code err, mutation_ptr& mu);
    void cleanup_preparing_mutations(bool wait);
    
    /////////////////////////////////////////////////////////////////
    // learning    
    void init_learn(uint64_t signature);
    void on_learn_reply(error_code err, learn_request&& req, learn_response&& resp);
    void on_copy_remote_state_completed(error_code err, size_t size, learn_request&& req, learn_response&& resp);
    void on_learn_remote_state_completed(error_code err);
    void handle_learning_error(error_code err);
    void handle_learning_succeeded_on_primary(::dsn::rpc_address node, uint64_t learn_signature);
    void notify_learn_completion();
    error_code apply_learned_state_from_private_log(learn_state& state);
        
    /////////////////////////////////////////////////////////////////
    // failure handling    
    void handle_local_failure(error_code error);
    void handle_remote_failure(partition_status status, ::dsn::rpc_address node, error_code error);

    /////////////////////////////////////////////////////////////////
    // reconfiguration
    void assign_primary(configuration_update_request& proposal);
    void add_potential_secondary(configuration_update_request& proposal);
    void upgrade_to_secondary_on_primary(::dsn::rpc_address node);
    void downgrade_to_secondary_on_primary(configuration_update_request& proposal);
    void downgrade_to_inactive_on_primary(configuration_update_request& proposal);
    void remove(configuration_update_request& proposal);
    void update_configuration_on_meta_server(config_type type, ::dsn::rpc_address node, partition_configuration& newConfig);
    void on_update_configuration_on_meta_server_reply(error_code err, dsn_message_t request, dsn_message_t response, std::shared_ptr<configuration_update_request> req);
    void replay_prepare_list();
    bool is_same_ballot_status_change_allowed(partition_status olds, partition_status news);

    // return false when update fails or replica is going to be closed
    bool update_configuration(const partition_configuration& config);
    bool update_local_configuration(const replica_configuration& config, bool same_ballot = false);
    
    /////////////////////////////////////////////////////////////////
    // group check
    void init_group_check();
    void broadcast_group_check();
    void on_group_check_reply(error_code err, const std::shared_ptr<group_check_request>& req, const std::shared_ptr<group_check_response>& resp);

    /////////////////////////////////////////////////////////////////
    // check timer for gc, checkpointing etc.
    void on_checkpoint_timer();
    void garbage_collection();
    void init_checkpoint();
    void background_checkpoint();
    void catch_up_with_private_logs(partition_status s);
    void on_checkpoint_completed(error_code err);
    void on_copy_checkpoint_ack(error_code err, const std::shared_ptr<replica_configuration>& req, const std::shared_ptr<learn_response>& resp);
    void on_copy_checkpoint_file_completed(error_code err, size_t sz, std::shared_ptr<learn_response> resp, const std::string &chk_dir);

private:
    friend class ::dsn::replication::replication_checker;
    friend class ::dsn::replication::test::test_checker;
    friend class ::dsn::replication::mutation_queue;

    // replica configuration, updated by update_local_configuration ONLY    
    replica_configuration   _config;
    uint64_t                _last_config_change_time_ms;

    // prepare list
    prepare_list*           _prepare_list;

    // private prepare log (may be empty, depending on config)
    mutation_log_ptr        _private_log;

    // local checkpoint timer for gc, checkpoint, etc.
    dsn::task_ptr           _checkpoint_timer;

    // application
    std::unique_ptr<replication_app_base>  _app;

    // constants
    replica_stub*           _stub;
    std::string             _app_type;
    std::string             _dir;
    char                    _name[256]; // app.index @ host:port
    replication_options     *_options;
    
    // replica status specific states
    primary_context             _primary_states;
    secondary_context           _secondary_states;
    potential_secondary_context _potential_secondary_states;
    bool                        _inactive_is_transient; // upgrade to P/S is allowed only iff true

    // perf counters
    perf_counter_               _counter_commit_latency;
};

}} // namespace
