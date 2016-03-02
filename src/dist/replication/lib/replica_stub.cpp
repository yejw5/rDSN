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
 *     replica container - replica stub
 *
 * Revision history:
 *     Mar., 2015, @imzhenyu (Zhenyu Guo), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */


#include "replica.h"
#include "replica_stub.h"
#include "mutation_log.h"
#include "mutation.h"
#include "replication_failure_detector.h"
#include <dsn/cpp/json_helper.h>

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "replica.stub"

namespace dsn { namespace replication {

using namespace dsn::service;

bool replica_stub::s_not_exit_on_log_failure = false;

replica_stub::replica_stub(replica_state_subscriber subscriber /*= nullptr*/, bool is_long_subscriber/* = true*/)
    : serverlet("replica_stub"), _replicas_lock(true), _cli_replica_stub_json_state_handle(nullptr)
{    
    _replica_state_subscriber = subscriber;
    _is_long_subscriber = is_long_subscriber;
    _failure_detector = nullptr;
    _state = NS_Disconnected;
    _log = nullptr;
    install_perf_counters();
}

replica_stub::~replica_stub(void)
{
    close();
}

void replica_stub::install_perf_counters()
{
    _counter_replicas_count.init("eon.replication", "replica#", COUNTER_TYPE_NUMBER, "# in replica_stub._replicas");
    _counter_replicas_opening_count.init("eon.replication", "opening_replica#", COUNTER_TYPE_NUMBER, "# in replica_stub._opening_replicas");
    _counter_replicas_closing_count.init("eon.replication", "closing_replica#", COUNTER_TYPE_NUMBER, "# in replica_stub._closing_replicas");
    _counter_replicas_total_commit_throught.init("eon.replication", "replicas.commit(#/s)", COUNTER_TYPE_RATE, "app commit throughput for all replicas");

    _counter_replicas_learning_failed_latency.init("eon.replication", "replicas.learning.failed(ns)", COUNTER_TYPE_NUMBER_PERCENTILES, "learning time (failed)");
    _counter_replicas_learning_success_latency.init("eon.replication", "replicas.learning.success(ns)", COUNTER_TYPE_NUMBER_PERCENTILES, "learning time (success)");
    _counter_replicas_learning_count.init("eon.replication", "replicas.learnig(#)", COUNTER_TYPE_NUMBER, "total learning count");
}

void replica_stub::initialize(bool clear/* = false*/)
{
    replication_options opts;
    opts.initialize();
    initialize(opts, clear);
}

void replica_stub::initialize(const replication_options& opts, bool clear/* = false*/)
{
    //zauto_lock l(_replicas_lock);
    set_options(opts);
    _primary_address = primary_address();

    // clear dirs if need
    if (clear)
    {
        if (!dsn::utils::filesystem::remove_path(_options.slog_dir))
        {
            dassert(false, "Fail to remove %s.", _options.slog_dir.c_str());
        }
        for (auto& dir : _options.data_dirs)
        {
            if (!dsn::utils::filesystem::remove_path(dir))
            {
                dassert(false, "Fail to remove %s.", dir.c_str());
            }
        }
    }

    // init dirs
    if (!dsn::utils::filesystem::create_directory(_options.slog_dir))
    {
        dassert(false, "Fail to create directory %s.", _options.slog_dir.c_str());
    }
    std::string cdir;
    if (!dsn::utils::filesystem::get_absolute_path(_options.slog_dir, cdir))
    {
        dassert(false, "Fail to get absolute path from %s.", _options.slog_dir.c_str());
    }
    _options.slog_dir = cdir;
    ddebug("app[%s]: slog_dir=%s", _options.app_name.c_str(), _options.slog_dir.c_str());
    int count = 0;
    for (auto& dir : _options.data_dirs)
    {
        if (!dsn::utils::filesystem::create_directory(dir))
        {
            dassert(false, "Fail to create directory %s.", dir.c_str());
        }
        std::string cdir;
        if (!dsn::utils::filesystem::get_absolute_path(dir, cdir))
        {
            dassert(false, "Fail to get absolute path from %s.", dir.c_str());
        }
        dir = cdir;
        ddebug("app[%s]: data_dirs[%d]=%s", _options.app_name.c_str(), count, dir.c_str());
        count++;
    }

    _log = new mutation_log(
        _options.slog_dir,
        _options.log_shared_batch_buffer_kb,
        _options.log_shared_file_size_mb,
        _options.log_shared_force_flush
        );

    // init rps
    replicas rps;
    std::vector<std::string> dir_list;

    for (auto& dir : _options.data_dirs)
    {
        std::vector<std::string> tmp_list;
        if (!dsn::utils::filesystem::get_subdirectories(dir, tmp_list, false))
        {
            dassert(false, "Fail to get subdirectories in %s.", dir.c_str());
        }
        dir_list.insert(dir_list.end(), tmp_list.begin(), tmp_list.end());
    }

    for (auto& dir : dir_list)
    {
        if (dir.length() >= 4 && dir.substr(dir.length() - 4) == ".err")
            continue;

        auto r = replica::load(this, dir.c_str());
        if (r != nullptr)
        {
            if (rps.find(r->get_gpid()) != rps.end())
            {
                dassert(false, "conflict replica dir: %s <--> %s", r->dir().c_str(), rps[r->get_gpid()]->dir().c_str());
            }
            ddebug("%u.%u @ %s: load replica '%s' success, <durable, commit> = <%" PRId64 ", %" PRId64 ">, last_prepared_decree = %" PRId64,
                r->get_gpid().app_id, r->get_gpid().pidx,
                primary_address().to_string(),
                dir.c_str(),
                r->last_durable_decree(),
                r->last_committed_decree(),
                r->last_prepared_decree()
                );
            rps[r->get_gpid()] = r;
        }
    }
    dir_list.clear();

    // init shared prepare log
    error_code err = _log->open(
        [&rps](mutation_ptr& mu)
        {
            auto it = rps.find(mu->data.header.gpid);
            if (it != rps.end())
            {
                return it->second->replay_mutation(mu, false);
            }
            else
            {
                return false;
            }
        }
        );

    if (err != ERR_OK)
    {
        derror(
            "%s: replication log replay failed, err %s, clear all logs ...",
            primary_address().to_string(),
            err.to_string()
            );

        // we must delete or update meta server the error for all replicas
        // before we fix the logs
        // otherwise, the next process restart may consider the replicas'
        // state complete

        // delete all replicas
        // TODO: checkpoint latest state and update on meta server so learning is cheaper
        for (auto it = rps.begin(); it != rps.end(); ++it)
        {
            it->second->close();
            std::string new_dir = it->second->dir() + ".err";
            if (!utils::filesystem::rename_path(it->second->dir(), new_dir))
            {
                dassert(false, "we cannot recover from the above error, exit ...");
            }
        }
        rps.clear();

        // restart log service
        _log->close();
        _log = nullptr;
        if (!utils::filesystem::remove_path(_options.slog_dir))
        {
            dassert(false, "remove directory %s failed", _options.slog_dir.c_str());
        }
        _log = new mutation_log(
            _options.slog_dir,
            opts.log_shared_batch_buffer_kb,
            opts.log_shared_file_size_mb,
            opts.log_shared_force_flush
            );
        auto lerr = _log->open(nullptr);
        dassert(lerr == ERR_OK, "restart log service must succeed");
    }

    for (auto it = rps.begin(); it != rps.end(); ++it)
    {
        it->second->reset_prepare_list_after_replay();
                
        decree smax = _log->max_decree(it->first);
        decree pmax = invalid_decree;
        if (it->second->private_log())
        {
            pmax = it->second->private_log()->max_decree(it->first);

            // possible when shared log is restarted
            if (smax == 0)
            {
                _log->update_max_decree(it->first, pmax);
                smax = pmax;
            }
        }

        dwarn(
            "%u.%u @ %s: load replica with err = %s, durable = %" PRId64 ", committed = %" PRId64 ", "
            "maxpd = %" PRId64 ", ballot = %" PRId64 ", max(share) = %" PRId64 ", max(private) = %" PRId64 ", log_offset = <%" PRId64 ", %" PRId64 ">",
            it->first.app_id, it->first.pidx,
            primary_address().to_string(),
            err.to_string(),
            it->second->last_durable_decree(),
            it->second->last_committed_decree(),
            it->second->max_prepared_decree(),
            it->second->get_ballot(),
            smax,
            pmax,
            it->second->get_app()->init_info().init_offset_in_shared_log,
            it->second->get_app()->init_info().init_offset_in_private_log
            );

        if (err == ERR_OK)
        {
            dassert(smax == pmax, "incomplete private log state");
            it->second->set_inactive_state_transient(true);
        }
        else
        {
            it->second->set_inactive_state_transient(false);
        }
    }

    // gc
    if (false == _options.gc_disabled)
    {
        _gc_timer_task = tasking::enqueue_timer(
            LPC_GARBAGE_COLLECT_LOGS_AND_REPLICAS,
            this,
            [this] {on_gc();},
            std::chrono::milliseconds(_options.gc_interval_ms),
            0,
            std::chrono::milliseconds(random32(0, _options.gc_interval_ms))
            );
    }
    
    // attach rps
    _replicas = std::move(rps);
    _counter_replicas_count.add((uint64_t)_replicas.size());

    // start timer for configuration sync
    if (!_options.config_sync_disabled)
    {
        _config_sync_timer_task = tasking::enqueue_timer(
            LPC_QUERY_CONFIGURATION_ALL,
            this,
            [this] {query_configuration_by_node();},
            std::chrono::milliseconds(_options.config_sync_interval_ms)
            );
    }
    
    // init livenessmonitor
    dassert (NS_Disconnected == _state, "");
    if (_options.fd_disabled == false)
    {
        _failure_detector = new replication_failure_detector(this, _options.meta_servers);
        err = _failure_detector->start(
            _options.fd_check_interval_seconds,
            _options.fd_beacon_interval_seconds,
            _options.fd_lease_seconds,
            _options.fd_grace_seconds
            );
        dassert(err == ERR_OK, "FD start failed, err = %s", err.to_string());

        _failure_detector->register_master(_failure_detector->current_server_contact());
    }
    else
    {
        _state = NS_Connected;
    }
}

replica_ptr replica_stub::get_replica(global_partition_id gpid, bool new_when_possible, const char* app_type)
{
    zauto_lock l(_replicas_lock);
    auto it = _replicas.find(gpid);
    if (it != _replicas.end())
        return it->second;
    else
    {
        if (!new_when_possible)
            return nullptr;
        else if (_opening_replicas.find(gpid) != _opening_replicas.end())
        {
            ddebug("cannot create new replica coz it is under open");
            return nullptr;
        }
        else if (_closing_replicas.find(gpid) != _closing_replicas.end())
        {
            ddebug("cannot create new replica coz it is under close");
            return nullptr;
        }
        else
        {
            dassert (app_type, "");
            replica* rep = replica::newr(this, app_type, gpid);
            if (rep != nullptr) 
            {
                add_replica(rep);
            }
            return rep;
        }
    }
}

replica_ptr replica_stub::get_replica(int32_t app_id, int32_t partition_index)
{
    global_partition_id gpid;
    gpid.app_id = app_id;
    gpid.pidx = partition_index;
    return get_replica(gpid);
}

void replica_stub::on_client_write(global_partition_id gpid, dsn_message_t request)
{
    replica_ptr rep = get_replica(gpid);
    if (rep != nullptr)
    {
        rep->on_client_write(task_code(dsn_msg_task_code(request)), request);
    }
    else
    {
        response_client_error(request, ERR_OBJECT_NOT_FOUND);
    }
}

void replica_stub::on_client_read(global_partition_id gpid, dsn_message_t request)
{
    replica_ptr rep = get_replica(gpid);
    if (rep != nullptr)
    {
        rep->on_client_read(task_code(dsn_msg_task_code(request)), request);
    }
    else
    {
        response_client_error(request, ERR_OBJECT_NOT_FOUND);
    }
}

void replica_stub::on_client_write2(dsn_message_t request)
{
    write_request_header hdr;
    ::unmarshall(request, hdr);

    if (hdr.code == TASK_CODE_INVALID)
    {
        response_client_error(request, ERR_INVALID_DATA);
        return;
    }

    replica_ptr rep = get_replica(hdr.gpid);
    if (rep != nullptr)
    {
        rep->on_client_write(hdr.code, request);
    }
    else
    {
        response_client_error(request, ERR_OBJECT_NOT_FOUND);
    }
}

void replica_stub::on_client_read2(dsn_message_t request)
{
    read_request_header hdr;
    ::unmarshall(request, hdr);

    if (hdr.code == TASK_CODE_INVALID)
    {
        response_client_error(request, ERR_INVALID_DATA);
        return;
    }

    replica_ptr rep = get_replica(hdr.gpid);
    if (rep != nullptr)
    {
        rep->on_client_read(hdr.code, request);
    }
    else
    {
        response_client_error(request, ERR_OBJECT_NOT_FOUND);
    }
}

void replica_stub::on_config_proposal(const configuration_update_request& proposal)
{
    if (!is_connected())
    {
        dwarn("on_config_proposal: not connected, ignore");
        return;
    }

    // TODO(qinzuoyan): if all replicas are down, then the meta server will choose one to assign primary,
    // if we open the replica with new_when_possible = true, then the old data will be cleared, is it reasonable?
    replica_ptr rep = get_replica(proposal.config.gpid, proposal.type == CT_ASSIGN_PRIMARY, proposal.config.app_type.c_str());
    if (rep == nullptr)
    {
        if (proposal.type == CT_ASSIGN_PRIMARY)
        {
            begin_open_replica(proposal.config.app_type, proposal.config.gpid);
        }   
        else if (proposal.type == CT_UPGRADE_TO_PRIMARY)
        {
            remove_replica_on_meta_server(proposal.config);
        }
    }

    if (rep != nullptr)
    {
        rep->on_config_proposal((configuration_update_request&)proposal);
    }
}

void replica_stub::on_query_decree(const query_replica_decree_request& req, /*out*/ query_replica_decree_response& resp)
{
    replica_ptr rep = get_replica(req.gpid);
    if (rep != nullptr)
    {
        resp.err = ERR_OK;
        if (PS_POTENTIAL_SECONDARY == rep->status())
        {
            resp.last_decree = 0;
        }
        else
        {
            resp.last_decree = rep->last_committed_decree();
            // TODO: use the following to alleviate data lost
            //resp.last_decree = rep->last_prepared_decree();
        }
    }
    else
    {
        resp.err = ERR_OBJECT_NOT_FOUND;
        resp.last_decree = 0;
    }
}

void replica_stub::on_query_replica_info(const query_replica_info_request& req, /*out*/ query_replica_info_response& resp)
{
    replicas rs;
    {
        zauto_lock l(_replicas_lock);
        rs = _replicas;
    }
    for (auto it = rs.begin(); it != rs.end(); ++it)
    {
        replica_ptr r = it->second;
        replica_info info;
        info.gpid = r->get_gpid();
        info.ballot = r->get_ballot();
        info.status = r->status();
        info.last_committed_decree = r->last_committed_decree();
        info.last_prepared_decree = r->last_prepared_decree();
        info.last_durable_decree = r->last_durable_decree();
        resp.replicas.push_back(info);
    }
    resp.err = ERR_OK;
}

void replica_stub::on_prepare(dsn_message_t request)
{
    global_partition_id gpid;
    ::unmarshall(request, gpid);    
    replica_ptr rep = get_replica(gpid);
    if (rep != nullptr)
    {
        rep->on_prepare(request);
    }
    else
    {
        prepare_ack resp;
        resp.gpid = gpid;
        resp.err = ERR_OBJECT_NOT_FOUND;
        reply(request, resp);
    }
}

void replica_stub::on_group_check(const group_check_request& request, /*out*/ group_check_response& response)
{
    if (!is_connected())
    {
        dwarn("on_group_check: not connected, ignore");
        return;
    }

    replica_ptr rep = get_replica(request.config.gpid, request.config.status == PS_POTENTIAL_SECONDARY, request.app_type.c_str());
    if (rep != nullptr)
    {
        rep->on_group_check(request, response);
    }
    else 
    {
        if (request.config.status == PS_POTENTIAL_SECONDARY)
        {
            std::shared_ptr<group_check_request> req(new group_check_request);
            *req = request;

            begin_open_replica(request.app_type, request.config.gpid, req);
            response.err = ERR_OK;
            response.learner_signature = invalid_signature;
        }
        else
        {
            response.err = ERR_OBJECT_NOT_FOUND;
        }
    }
}

void replica_stub::on_learn(dsn_message_t msg)
{
    learn_request request;
    ::unmarshall(msg, request);

    replica_ptr rep = get_replica(request.gpid);
    if (rep != nullptr)
    {
        rep->on_learn(msg, request);
    }
    else
    {
        learn_response response;
        response.err = ERR_OBJECT_NOT_FOUND;
        reply(msg, response);
    }
}

void replica_stub::on_copy_checkpoint(const replica_configuration& request, /*out*/ learn_response& response)
{
    replica_ptr rep = get_replica(request.gpid);
    if (rep != nullptr)
    {
        rep->on_copy_checkpoint(request, response);
    }
    else
    {
        response.err = ERR_OBJECT_NOT_FOUND;
    }
}

void replica_stub::on_learn_completion_notification(const group_check_response& report)
{
    replica_ptr rep = get_replica(report.gpid);
    if (rep != nullptr)
    {
        rep->on_learn_completion_notification(report);
    }
    else
    {
        report.err.end_tracking();
    }
}

void replica_stub::on_add_learner(const group_check_request& request)
{
    replica_ptr rep = get_replica(request.config.gpid, false, request.app_type.c_str());
    if (rep != nullptr)
    {
        rep->on_add_learner(request);
    }
    else
    {
        std::shared_ptr<group_check_request> req(new group_check_request);
        *req = request;
        begin_open_replica(request.app_type, request.config.gpid, req);
    }
}

void replica_stub::on_remove(const replica_configuration& request)
{
    replica_ptr rep = get_replica(request.gpid);
    if (rep != nullptr)
    {
        rep->on_remove(request);
    }
}

void replica_stub::json_state(std::stringstream& out) const
{
    std::vector<replica_ptr> replicas_copy;
    {
        zauto_lock _(_replicas_lock);
        for (auto& rep : _replicas)
        {
            replicas_copy.push_back(rep.second);
        }
    }
    json_encode(out, replicas_copy);
}

void replica_stub::static_replica_stub_json_state(void* context, int argc, const char** argv, dsn_cli_reply* reply)
{
    auto stub = reinterpret_cast<replica_stub*>(context);
    std::stringstream ss;
    stub->json_state(ss);
    auto danglingstr = new std::string(std::move(ss.str()));
    reply->message = danglingstr->c_str();
    reply->size = danglingstr->size();
    reply->context = danglingstr;
}

void replica_stub::static_replica_stub_json_state_freer(dsn_cli_reply reply)
{
    dassert(reply.context != nullptr, "corrupted cli reply");
    auto danglingstr = reinterpret_cast<std::string*>(reply.context);
    dassert(danglingstr->c_str() == reply.message, "corrupted cli reply");
    delete danglingstr;
}

void replica_stub::query_configuration_by_node()
{
    if (_state == NS_Disconnected)
    {
        return;
    }

    if (_config_query_task != nullptr)
    {
        _config_query_task->cancel(false);
    }

    dsn_message_t msg = dsn_msg_create_request(RPC_CM_QUERY_NODE_PARTITIONS, 0, 0);

    configuration_query_by_node_request req;
    req.node = _primary_address;
    ::marshall(msg, req);

    rpc_address target(_failure_detector->get_servers());
    _config_query_task = rpc::call(
        target,
        msg,
        this,
        [this](error_code err, dsn_message_t request, dsn_message_t resp)
        {
            on_node_query_reply(err, request, resp);
        }
        );
}

void replica_stub::on_meta_server_connected()
{
    ddebug(
        "%s: meta server connected",
        primary_address().to_string()
        );

    zauto_lock l(_replicas_lock);
    if (_state == NS_Disconnected)
    {
        _state = NS_Connecting;
        query_configuration_by_node();
    }
}

void replica_stub::on_node_query_reply(error_code err, dsn_message_t request, dsn_message_t response)
{
    ddebug(
        "%s: node view replied, err = %s",
        primary_address().to_string(),
        err.to_string()
        );    

    if (err != ERR_OK)
    {
        zauto_lock l(_replicas_lock);
        if (_state == NS_Connecting)
        {
            query_configuration_by_node();
        }
    }
    else
    {
        zauto_lock l(_replicas_lock);
        if (_state == NS_Connecting)
        {
            _state = NS_Connected;
        }

        // DO NOT UPDATE STATE WHEN DISCONNECTED
        if (_state != NS_Connected)
            return;
        
        configuration_query_by_node_response resp;
        ::unmarshall(response, resp);

        if (resp.err != ERR_OK)
            return;
        
        replicas rs = _replicas;
        for (auto it = resp.partitions.begin(); it != resp.partitions.end(); ++it)
        {
            rs.erase(it->gpid);
            tasking::enqueue(
                LPC_QUERY_NODE_CONFIGURATION_SCATTER,
                this,
                std::bind(&replica_stub::on_node_query_reply_scatter, this, this, *it),
                gpid_to_hash(it->gpid)
                );
        }

        // for rps not exist on meta_servers
        for (auto it = rs.begin(); it != rs.end(); ++it)
        {
            tasking::enqueue(
                LPC_QUERY_NODE_CONFIGURATION_SCATTER,
                this,
                std::bind(&replica_stub::on_node_query_reply_scatter2, this, this, it->first),
                gpid_to_hash(it->first)
                );
        }
    }
}

void replica_stub::set_meta_server_connected_for_test(const configuration_query_by_node_response& resp)
{
    zauto_lock l(_replicas_lock);
    dassert (_state != NS_Connected, "");
    _state = NS_Connected;

    for (auto it = resp.partitions.begin(); it != resp.partitions.end(); ++it)
    {
        tasking::enqueue(
            LPC_QUERY_NODE_CONFIGURATION_SCATTER,
            this,
            std::bind(&replica_stub::on_node_query_reply_scatter, this, this, *it),
            gpid_to_hash(it->gpid)
            );
    }
}

void replica_stub::set_replica_state_subscriber_for_test(replica_state_subscriber subscriber, bool is_long_subscriber)
{
    _replica_state_subscriber = subscriber;
    _is_long_subscriber = is_long_subscriber;
}

// this_ is used to hold a ref to replica_stub so we don't need to cancel the task on replica_stub::close
void replica_stub::on_node_query_reply_scatter(replica_stub_ptr this_, const partition_configuration& config)
{
    replica_ptr replica = get_replica(config.gpid);
    if (replica != nullptr)
    {
        replica->on_config_sync(config);
    }
    else
    {
        ddebug(
            "%u.%u @ %s: replica not exists on replica server, remove it from meta server",
            config.gpid.app_id, config.gpid.pidx,
            primary_address().to_string()
            );

        if (config.primary == _primary_address)
        {
            remove_replica_on_meta_server(config);
        }
    }
}

void replica_stub::on_node_query_reply_scatter2(replica_stub_ptr this_, global_partition_id gpid)
{
    replica_ptr replica = get_replica(gpid);
    if (replica != nullptr && replica->status() != PS_POTENTIAL_SECONDARY)
    {
        ddebug(
            "%u.%u @ %s: replica not exists on meta server, removed",
            gpid.app_id, gpid.pidx,
            primary_address().to_string()
            );
        replica->update_local_configuration_with_no_ballot_change(PS_ERROR);
    }
}

void replica_stub::remove_replica_on_meta_server(const partition_configuration& config)
{
    dsn_message_t msg = dsn_msg_create_request(RPC_CM_UPDATE_PARTITION_CONFIGURATION, 0, 0);

    std::shared_ptr<configuration_update_request> request(new configuration_update_request);
    request->config = config;
    request->config.ballot++;        
    request->node = _primary_address;
    request->type = CT_DOWNGRADE_TO_INACTIVE;

    if (_primary_address == config.primary)
    {
        request->config.primary.set_invalid();        
    }
    else if (replica_helper::remove_node(_primary_address, request->config.secondaries))
    {
    }
    else
    {
        return;
    }

    ::marshall(msg, *request);

    rpc_address target(_failure_detector->get_servers());
    rpc::call(
        _failure_detector->get_servers(),
        msg,
        nullptr,
        [](error_code, dsn_message_t, dsn_message_t) {}
        );
}

void replica_stub::on_meta_server_disconnected()
{
    ddebug(
        "%s: meta server disconnected",
        primary_address().to_string()
        );
    zauto_lock l(_replicas_lock);
    if (NS_Disconnected == _state)
        return;

    _state = NS_Disconnected;

    for (auto it = _replicas.begin(); it != _replicas.end(); ++it)
    {
        tasking::enqueue(
            LPC_CM_DISCONNECTED_SCATTER,
            this,
            std::bind(&replica_stub::on_meta_server_disconnected_scatter, this, this, it->first),
            gpid_to_hash(it->first)
            );
    }
}

// this_ is used to hold a ref to replica_stub so we don't need to cancel the task on replica_stub::close
void replica_stub::on_meta_server_disconnected_scatter(replica_stub_ptr this_, global_partition_id gpid)
{
    {
        zauto_lock l(_replicas_lock);
        if (_state != NS_Disconnected)
            return;
    }

    replica_ptr replica = get_replica(gpid);
    if (replica != nullptr)
    {
        replica->on_meta_server_disconnected();
    }
}

void replica_stub::response_client_error(dsn_message_t request, error_code error)
{
    if (nullptr == request)
    {
        error.end_tracking();
        return;
    }

    ddebug("reply client read/write, err = %s", error.to_string());
    reply(request, error);
}

void replica_stub::init_gc_for_test()
{
    dassert (_options.gc_disabled, "");

    _gc_timer_task = tasking::enqueue(
        LPC_GARBAGE_COLLECT_LOGS_AND_REPLICAS,
        this,
        [this] {on_gc();},
        0,
        std::chrono::milliseconds(_options.gc_interval_ms)
        );
}

void replica_stub::on_gc()
{
    replicas rs;
    {
        zauto_lock l(_replicas_lock);
        rs = _replicas;
    }

    // gc shared prepare log
    if (_log != nullptr)
    {
        // gc condition is:
        //   d <= last_durable_decree && d <= private_log.max_commit_decree
        replica_log_info_map gc_condition;
        for (auto it = rs.begin(); it != rs.end(); ++it)
        {
            replica_log_info ri;
            replica_ptr r = it->second;
            mutation_log_ptr plog = r->private_log();
            if (plog)
            {
                ri.max_decree = std::min(r->last_durable_decree(), plog->max_commit_on_disk());
            }
            else
            {
                ri.max_decree = r->last_durable_decree();
            }
            ri.valid_start_offset = r->get_app()->init_info().init_offset_in_shared_log;
            gc_condition[it->first] = ri;
        }
        _log->garbage_collection(gc_condition);
    }
    
    // gc on-disk rps
    std::vector<std::string> sub_list;
    for (auto& dir : _options.data_dirs)
    {
        std::vector<std::string> tmp_list;
        if (!dsn::utils::filesystem::get_subdirectories(dir, tmp_list, false))
        {
            dwarn("on_gc(): failed to get subdirectories in %s", dir.c_str());
            return;
        }
        sub_list.insert(sub_list.end(), tmp_list.begin(), tmp_list.end());
    }
    std::string ext = ".err";
    for (auto& fpath : sub_list)
    {
        auto&& name = dsn::utils::filesystem::get_file_name(fpath);
        if ((name.length() > ext.length())
            && (name.compare((name.length() - ext.length()), std::string::npos, ext) == 0)
            )
        {
            time_t mt;
            if (!dsn::utils::filesystem::last_write_time(fpath, mt))
            {
                dwarn("on_gc(): failed to get last write time of %s", fpath.c_str());
                continue;
            }

            if (mt > ::time(0) + _options.gc_disk_error_replica_interval_seconds)
            {
                if (!dsn::utils::filesystem::remove_path(fpath))
                {
                    dwarn("on_gc(): failed to delete directory %s", fpath.c_str());
                }
            }
        }
    }
    sub_list.clear();

#if 0
    boost::filesystem::directory_iterator endtr;
    for (boost::filesystem::directory_iterator it(dir());
        it != endtr;
        ++it)
    {
        auto name = it->path().filename().string();
        if (name.length() > strlen(".err") && name.substr() == ".err")
        {
            std::time_t mt = boost::filesystem::last_write_time(it->path());
            if (mt > time(0) + _options.gc_disk_error_replica_interval_seconds)
            {
                boost::filesystem::remove_all(_dir + "/" + name);
            }
        }
    }
#endif
}

::dsn::task_ptr replica_stub::begin_open_replica(const std::string& app_type, global_partition_id gpid, std::shared_ptr<group_check_request> req)
{
    _replicas_lock.lock();
    if (_replicas.find(gpid) != _replicas.end())
    {
        _replicas_lock.unlock();
        return nullptr;
    }        

    auto it = _opening_replicas.find(gpid);
    if (it != _opening_replicas.end())
    {
        _replicas_lock.unlock();
        return nullptr;
    }
    else 
    {
        auto it2 = _closing_replicas.find(gpid);
        if (it2 != _closing_replicas.end())
        {
            if (it2->second.second->status() == PS_INACTIVE 
                && it2->second.first->cancel(false))
            {
                replica_ptr r = it2->second.second;
                _closing_replicas.erase(it2);
                _counter_replicas_closing_count.decrement();

                add_replica(r);

                // unlock here to avoid dead lock
                _replicas_lock.unlock();

                ddebug( "open replica which is to be closed '%s.%u.%u'", app_type.c_str(), gpid.app_id, gpid.pidx);

                if (req != nullptr)
                {
                    on_add_learner(*req);
                }
                return nullptr;
            }
            else 
            {
                _replicas_lock.unlock();
                dwarn( "open replica '%s.%u.%u' failed coz replica is under closing", 
                    app_type.c_str(), gpid.app_id, gpid.pidx);                
                return nullptr;
            }
        }
        else 
        {
            task_ptr task = tasking::enqueue(LPC_OPEN_REPLICA, this, std::bind(&replica_stub::open_replica, this, app_type, gpid, req));

            _counter_replicas_opening_count.increment();
            _opening_replicas[gpid] = task;
            _replicas_lock.unlock();
            return task;
        }
    }
}

void replica_stub::open_replica(const std::string app_type, global_partition_id gpid, std::shared_ptr<group_check_request> req)
{
    std::string dir = get_replica_dir(app_type.c_str(), gpid);
    dwarn("open replica '%s'", dir.c_str());

    replica_ptr rep = replica::load(this, dir.c_str());

    if (rep == nullptr)
    {
        rep = replica::newr(this, app_type.c_str(), gpid);
    }

    if (rep == nullptr)
    {
        _counter_replicas_opening_count.decrement();
        zauto_lock l(_replicas_lock);
        _opening_replicas.erase(gpid);
        return;
    }
            
    {
        _counter_replicas_opening_count.decrement();
        zauto_lock l(_replicas_lock);
        auto it = _replicas.find(gpid);
        dassert (it == _replicas.end(), "");
        add_replica(rep);
        _opening_replicas.erase(gpid);
    }

    if (nullptr != req)
    {
        rpc::call_one_way_typed(_primary_address, RPC_LEARN_ADD_LEARNER, *req, gpid_to_hash(req->config.gpid));
    }
}

::dsn::task_ptr replica_stub::begin_close_replica(replica_ptr r)
{
    dassert(
        r->status() == PS_ERROR || r->status() == PS_INACTIVE,
        "%s: invalid state %s when calling begin_close_replica",
        r->name(),
        enum_to_string(r->status())
        );

    zauto_lock l(_replicas_lock);

    //// TODO: so what?
    //// initialization is still ongoing
    //if (nullptr == _failure_detector)
    //    return nullptr;

    if (remove_replica(r))
    {
        task_ptr task = tasking::enqueue(LPC_CLOSE_REPLICA, this,
            [=]()
            {
                close_replica(r);
            }, 
            0, 
            std::chrono::milliseconds(r->status() == PS_ERROR ? 0 : _options.gc_memory_replica_interval_ms)
            );
        _closing_replicas[r->get_gpid()] = std::make_pair(task, r);
        _counter_replicas_closing_count.increment();
        return task;
    }
    else
    {
        return nullptr;
    }
}

void replica_stub::close_replica(replica_ptr r)
{
    dwarn( "close replica '%s'", r->dir().c_str());

    r->close();

    {
        _counter_replicas_closing_count.decrement();
        zauto_lock l(_replicas_lock);
        _closing_replicas.erase(r->get_gpid());
    }
}

void replica_stub::add_replica(replica_ptr r)
{
    _counter_replicas_count.increment();
    zauto_lock l(_replicas_lock);
    auto pr = _replicas.insert(replicas::value_type(r->get_gpid(), r));
    dassert(pr.second, "replica %s is already in the collection", r->name());
}

bool replica_stub::remove_replica(replica_ptr r)
{
    zauto_lock l(_replicas_lock);
    if (_replicas.erase(r->get_gpid()) > 0)
    {
        _counter_replicas_count.decrement();
        return true;
    }
    else
    {
        return false;
    }
}

void replica_stub::notify_replica_state_update(const replica_configuration& config, bool is_closing)
{
    if (nullptr != _replica_state_subscriber)
    {
        if (_is_long_subscriber)
        {
            tasking::enqueue(LPC_REPLICA_STATE_CHANGE_NOTIFICATION, this, std::bind(_replica_state_subscriber, _primary_address, config, is_closing));
        }
        else
        {
            _replica_state_subscriber(_primary_address, config, is_closing);
        }
    }
}

void replica_stub::handle_log_failure(error_code err)
{
    derror("handle log failure: %s", err.to_string());
    if (!s_not_exit_on_log_failure)
    {
        dassert(false, "TODO: better log failure handling ...");
    }
}

void replica_stub::open_service()
{
    register_rpc_handler(RPC_REPLICATION_CLIENT_WRITE, "write", &replica_stub::on_client_write2);
    register_rpc_handler(RPC_REPLICATION_CLIENT_READ, "read", &replica_stub::on_client_read2);

    register_rpc_handler(RPC_CONFIG_PROPOSAL, "ProposeConfig", &replica_stub::on_config_proposal);

    register_rpc_handler(RPC_PREPARE, "prepare", &replica_stub::on_prepare);
    register_rpc_handler(RPC_LEARN, "Learn", &replica_stub::on_learn);
    register_rpc_handler(RPC_LEARN_COMPLETION_NOTIFY, "LearnNotify", &replica_stub::on_learn_completion_notification);
    register_rpc_handler(RPC_LEARN_ADD_LEARNER, "LearnAdd", &replica_stub::on_add_learner);
    register_rpc_handler(RPC_REMOVE_REPLICA, "remove", &replica_stub::on_remove);
    register_rpc_handler(RPC_GROUP_CHECK, "GroupCheck", &replica_stub::on_group_check);
    register_rpc_handler(RPC_QUERY_PN_DECREE, "query_decree", &replica_stub::on_query_decree);
    register_rpc_handler(RPC_QUERY_REPLICA_INFO, "query_replica_info", &replica_stub::on_query_replica_info);
    register_rpc_handler(RPC_REPLICA_COPY_LAST_CHECKPOINT, "copy_checkpoint", &replica_stub::on_copy_checkpoint);

    _cli_replica_stub_json_state_handle = dsn_cli_app_register("info", "get the info of replica_stub on this node", "",
        this, &static_replica_stub_json_state, &static_replica_stub_json_state_freer);
    dassert(_cli_replica_stub_json_state_handle != nullptr, "register cli command failed");
}

void replica_stub::close()
{
    // this replica may not be opened
    // or is already closed by calling tool_app::stop_all_apps()
    // in this case, just return
    if(_cli_replica_stub_json_state_handle == nullptr)
    {
        return;
    }

    dsn_cli_deregister(_cli_replica_stub_json_state_handle);
    _cli_replica_stub_json_state_handle = nullptr;

    if (_config_sync_timer_task != nullptr)
    {
        _config_sync_timer_task->cancel(true);
        _config_sync_timer_task = nullptr;
    }

    if (_config_query_task != nullptr)
    {
        _config_query_task->cancel(true);
        _config_query_task = nullptr;
    }
    _state = NS_Disconnected;
    
    if (_gc_timer_task != nullptr)
    {
        _gc_timer_task->cancel(true);
        _gc_timer_task = nullptr;
    }
    
    {
        zauto_lock l(_replicas_lock);    
        while (_closing_replicas.empty() == false)
        {
            task_ptr task = _closing_replicas.begin()->second.first;
            global_partition_id tmp_gpid = _closing_replicas.begin()->first;
            _replicas_lock.unlock();

            task->wait();

            _replicas_lock.lock();
            // task will automatically remove this replica from _closing_replicas
            if(false == _closing_replicas.empty())
            {
                dassert((tmp_gpid == _closing_replicas.begin()->first) == false, "this replica '%u.%u' should be removed from _closing_replicas, gpid", tmp_gpid.app_id, tmp_gpid.pidx);
            }
        }

        while (_opening_replicas.empty() == false)
        {
            task_ptr task = _opening_replicas.begin()->second;
            _replicas_lock.unlock();

            task->cancel(true);

            _counter_replicas_opening_count.decrement();
            _replicas_lock.lock();
            _opening_replicas.erase(_opening_replicas.begin());
        }

        while (_replicas.empty() == false)
        {
            _replicas.begin()->second->close();

            _counter_replicas_count.decrement();
            _replicas.erase(_replicas.begin());
        }
    }
        
    if (_failure_detector != nullptr)
    {
        _failure_detector->stop();
        delete _failure_detector;
        _failure_detector = nullptr;
    }

    if (_log != nullptr)
    {
        _log->close();
        _log = nullptr;
    }
}

std::string replica_stub::get_replica_dir(const char* app_type, global_partition_id gpid) const
{
    char buffer[256];
    sprintf(buffer, "%u.%u.%s", gpid.app_id, gpid.pidx, app_type);
    std::string ret_dir;
    for (auto& dir : _options.data_dirs)
    {
        std::string cur_dir = utils::filesystem::path_combine(dir, buffer);
        if (utils::filesystem::directory_exists(cur_dir))
        {
            if (!ret_dir.empty())
            {
                dassert(false, "replica dir conflict: %s <--> %s", cur_dir.c_str(), ret_dir.c_str());
            }
            ret_dir = cur_dir;
        }
    }
    if (ret_dir.empty())
    {
        /*
        int r = dsn_random32(0, _options.data_dirs.size() - 1);
        ret_dir = utils::filesystem::path_combine(_options.data_dirs[r], buffer);
        */
        static std::atomic<int> next_id;
        int pos = (next_id++) % _options.data_dirs.size();
        ret_dir = utils::filesystem::path_combine(_options.data_dirs[pos], buffer);
    }
    return ret_dir;
}

}} // namespace

