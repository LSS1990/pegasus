// Copyright (c) 2017, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#pragma once

#include <getopt.h>
#include <thread>
#include <iomanip>
#include <fstream>
#include <queue>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <rocksdb/db.h>
#include <rocksdb/sst_dump_tool.h>
#include <rocksdb/env.h>
#include <monitoring/histogram.h>
#include <dsn/dist/cli/cli.client.h>
#include <dsn/dist/replication/replication_ddl_client.h>
#include <dsn/dist/replication/mutation_log_tool.h>
#include <dsn/perf_counter/perf_counter_utils.h>

#include <rrdb/rrdb.code.definition.h>
#include <pegasus/version.h>
#include <pegasus/git_commit.h>
#include <pegasus/error.h>
#include <geo/lib/geo_client.h>

#include "base/pegasus_key_schema.h"
#include "base/pegasus_value_schema.h"
#include "base/pegasus_utils.h"

#include "command_executor.h"
#include "command_utils.h"

using namespace dsn::replication;

#define STR_I(var) #var
#define STR(var) STR_I(var)
#ifndef DSN_BUILD_TYPE
#define PEGASUS_BUILD_TYPE ""
#else
#define PEGASUS_BUILD_TYPE STR(DSN_BUILD_TYPE)
#endif

DEFINE_TASK_CODE(LPC_SCAN_DATA, TASK_PRIORITY_COMMON, ::dsn::THREAD_POOL_DEFAULT)
enum scan_data_operator
{
    SCAN_COPY,
    SCAN_CLEAR,
    SCAN_COUNT,
    SCAN_GEN_GEO
};
class top_container
{
public:
    struct top_heap_item
    {
        std::string hash_key;
        std::string sort_key;
        long row_size;
        top_heap_item(std::string &&hash_key_, std::string &&sort_key_, long row_size_)
            : hash_key(std::move(hash_key_)), sort_key(std::move(sort_key_)), row_size(row_size_)
        {
        }
    };
    struct top_heap_compare
    {
        bool operator()(top_heap_item i1, top_heap_item i2) { return i1.row_size < i2.row_size; }
    };
    typedef std::priority_queue<top_heap_item, std::vector<top_heap_item>, top_heap_compare>
        top_heap;

    top_container(int count) : _count(count) {}

    void push(std::string &&hash_key, std::string &&sort_key, long row_size)
    {
        dsn::utils::auto_lock<dsn::utils::ex_lock_nr> l(_lock);
        if (_heap.size() < _count) {
            _heap.emplace(std::move(hash_key), std::move(sort_key), row_size);
        } else {
            const top_heap_item &top = _heap.top();
            if (top.row_size < row_size) {
                _heap.pop();
                _heap.emplace(std::move(hash_key), std::move(sort_key), row_size);
            }
        }
    }

    top_heap &all() { return _heap; }

private:
    int _count;
    top_heap _heap;
    dsn::utils::ex_lock_nr _lock;
};

struct scan_data_context
{
    scan_data_operator op;
    int split_id;
    int max_batch_count;
    int timeout_ms;
    pegasus::pegasus_client::pegasus_scanner_wrapper scanner;
    pegasus::pegasus_client *client;
    pegasus::geo::geo_client *geoclient;
    std::atomic_bool *error_occurred;
    std::atomic_long split_rows;
    std::atomic_long split_request_count;
    std::atomic_bool split_completed;
    bool stat_size;
    rocksdb::HistogramImpl hash_key_size_histogram;
    rocksdb::HistogramImpl sort_key_size_histogram;
    rocksdb::HistogramImpl value_size_histogram;
    rocksdb::HistogramImpl row_size_histogram;
    int top_count;
    top_container top_rows;
    scan_data_context(scan_data_operator op_,
                      int split_id_,
                      int max_batch_count_,
                      int timeout_ms_,
                      pegasus::pegasus_client::pegasus_scanner_wrapper scanner_,
                      pegasus::pegasus_client *client_,
                      pegasus::geo::geo_client *geoclient_,
                      std::atomic_bool *error_occurred_,
                      bool stat_size_ = false,
                      int top_count_ = 0)
        : op(op_),
          split_id(split_id_),
          max_batch_count(max_batch_count_),
          timeout_ms(timeout_ms_),
          scanner(scanner_),
          client(client_),
          geoclient(geoclient_),
          error_occurred(error_occurred_),
          split_rows(0),
          split_request_count(0),
          split_completed(false),
          stat_size(stat_size_),
          top_count(top_count_),
          top_rows(top_count_)
    {
    }
};
inline void update_atomic_max(std::atomic_long &max, long value)
{
    while (true) {
        long old = max.load();
        if (value <= old || max.compare_exchange_weak(old, value)) {
            break;
        }
    }
}
inline void scan_data_next(scan_data_context *context)
{
    while (!context->split_completed.load() && !context->error_occurred->load() &&
           context->split_request_count.load() < context->max_batch_count) {
        context->split_request_count++;
        context->scanner->async_next([context](int ret,
                                               std::string &&hash_key,
                                               std::string &&sort_key,
                                               std::string &&value,
                                               pegasus::pegasus_client::internal_info &&info) {
            if (ret == pegasus::PERR_OK) {
                switch (context->op) {
                case SCAN_COPY:
                    context->split_request_count++;
                    context->client->async_set(
                        hash_key,
                        sort_key,
                        value,
                        [context](int err, pegasus::pegasus_client::internal_info &&info) {
                            if (err != pegasus::PERR_OK) {
                                if (!context->split_completed.exchange(true)) {
                                    fprintf(stderr,
                                            "ERROR: split[%d] async set failed: %s\n",
                                            context->split_id,
                                            context->client->get_error_string(err));
                                    context->error_occurred->store(true);
                                }
                            } else {
                                context->split_rows++;
                                scan_data_next(context);
                            }
                            // should put "split_request_count--" at end of the scope,
                            // to prevent that split_request_count becomes 0 in the middle.
                            context->split_request_count--;
                        },
                        context->timeout_ms);
                    break;
                case SCAN_CLEAR:
                    context->split_request_count++;
                    context->client->async_del(
                        hash_key,
                        sort_key,
                        [context](int err, pegasus::pegasus_client::internal_info &&info) {
                            if (err != pegasus::PERR_OK) {
                                if (!context->split_completed.exchange(true)) {
                                    fprintf(stderr,
                                            "ERROR: split[%d] async del failed: %s\n",
                                            context->split_id,
                                            context->client->get_error_string(err));
                                    context->error_occurred->store(true);
                                }
                            } else {
                                context->split_rows++;
                                scan_data_next(context);
                            }
                            // should put "split_request_count--" at end of the scope,
                            // to prevent that split_request_count becomes 0 in the middle.
                            context->split_request_count--;
                        },
                        context->timeout_ms);
                    break;
                case SCAN_COUNT:
                    context->split_rows++;
                    if (context->stat_size) {
                        long hash_key_size = hash_key.size();
                        context->hash_key_size_histogram.Add(hash_key_size);

                        long sort_key_size = sort_key.size();
                        context->sort_key_size_histogram.Add(sort_key_size);

                        long value_size = value.size();
                        context->value_size_histogram.Add(value_size);

                        long row_size = hash_key_size + sort_key_size + value_size;
                        context->row_size_histogram.Add(row_size);

                        if (context->top_count > 0) {
                            context->top_rows.push(
                                std::move(hash_key), std::move(sort_key), row_size);
                        }
                    }
                    scan_data_next(context);
                    break;
                case SCAN_GEN_GEO:
                    context->split_request_count++;
                    context->geoclient->async_set(
                        hash_key,
                        sort_key,
                        value,
                        [context](int err, pegasus::pegasus_client::internal_info &&info) {
                            if (err != pegasus::PERR_OK) {
                                if (!context->split_completed.exchange(true)) {
                                    fprintf(stderr,
                                            "ERROR: split[%d] async set failed: %s\n",
                                            context->split_id,
                                            context->client->get_error_string(err));
                                    context->error_occurred->store(true);
                                }
                            } else {
                                context->split_rows++;
                                scan_data_next(context);
                            }
                            // should put "split_request_count--" at end of the scope,
                            // to prevent that split_request_count becomes 0 in the middle.
                            context->split_request_count--;
                        },
                        context->timeout_ms);
                    break;
                default:
                    dassert(false, "op = %d", context->op);
                    break;
                }
            } else if (ret == pegasus::PERR_SCAN_COMPLETE) {
                context->split_completed.store(true);
            } else {
                if (!context->split_completed.exchange(true)) {
                    fprintf(stderr,
                            "ERROR: split[%d] scan next failed: %s\n",
                            context->split_id,
                            context->client->get_error_string(ret));
                    context->error_occurred->store(true);
                }
            }
            // should put "split_request_count--" at end of the scope,
            // to prevent that split_request_count becomes 0 in the middle.
            context->split_request_count--;
        });
    }
}

struct node_desc
{
    std::string desc;
    dsn::rpc_address address;
    node_desc(const std::string &s, const dsn::rpc_address &n) : desc(s), address(n) {}
};
// type: all | replica-server | meta-server
inline bool fill_nodes(shell_context *sc, const std::string &type, std::vector<node_desc> &nodes)
{
    if (type == "all" || type == "meta-server") {
        for (auto &addr : sc->meta_list) {
            nodes.emplace_back("meta-server", addr);
        }
    }

    if (type == "all" || type == "replica-server") {
        std::map<dsn::rpc_address, dsn::replication::node_status::type> rs_nodes;
        ::dsn::error_code err =
            sc->ddl_client->list_nodes(dsn::replication::node_status::NS_ALIVE, rs_nodes);
        if (err != ::dsn::ERR_OK) {
            fprintf(stderr, "ERROR: list node failed: %s\n", err.to_string());
            return false;
        }
        for (auto &kv : rs_nodes) {
            nodes.emplace_back("replica-server", kv.first);
        }
    }

    return true;
}

inline void call_remote_command(shell_context *sc,
                                const std::vector<node_desc> &nodes,
                                const ::dsn::command &cmd,
                                std::vector<std::pair<bool, std::string>> &results)
{
    dsn::cli_client cli;
    std::vector<dsn::task_ptr> tasks;
    tasks.resize(nodes.size());
    results.resize(nodes.size());
    for (int i = 0; i < nodes.size(); ++i) {
        auto callback = [&results,
                         i](::dsn::error_code err, dsn::message_ex *req, dsn::message_ex *resp) {
            if (err == ::dsn::ERR_OK) {
                results[i].first = true;
                ::dsn::unmarshall(resp, results[i].second);
            } else {
                results[i].first = false;
                results[i].second = err.to_string();
            }
        };
        tasks[i] =
            cli.call(cmd, callback, std::chrono::milliseconds(5000), 0, 0, 0, nodes[i].address);
    }
    for (int i = 0; i < nodes.size(); ++i) {
        tasks[i]->wait();
    }
}

inline bool parse_app_pegasus_perf_counter_name(const std::string &name,
                                                int32_t &app_id,
                                                int32_t &partition_index,
                                                std::string &counter_name)
{
    std::string::size_type find = name.find_last_of('@');
    if (find == std::string::npos)
        return false;
    int n = sscanf(name.c_str() + find + 1, "%d.%d", &app_id, &partition_index);
    if (n != 2)
        return false;
    std::string::size_type find2 = name.find_last_of('*');
    if (find2 == std::string::npos)
        return false;
    counter_name = name.substr(find2 + 1, find - find2 - 1);
    return true;
}

struct row_data
{
    std::string row_name;
    double get_qps = 0;
    double multi_get_qps = 0;
    double put_qps = 0;
    double multi_put_qps = 0;
    double remove_qps = 0;
    double multi_remove_qps = 0;
    double incr_qps = 0;
    double check_and_set_qps = 0;
    double check_and_mutate_qps = 0;
    double scan_qps = 0;
    double recent_expire_count = 0;
    double recent_filter_count = 0;
    double recent_abnormal_count = 0;
    double storage_mb = 0;
    double storage_count = 0;
    double rdb_block_cache_hit_count = 0;
    double rdb_block_cache_total_count = 0;
    double rdb_block_cache_mem_usage = 0;
    double rdb_index_and_filter_blocks_mem_usage = 0;
    double rdb_memtable_mem_usage = 0;
};

inline bool
update_app_pegasus_perf_counter(row_data &row, const std::string &counter_name, double value)
{
    if (counter_name == "get_qps")
        row.get_qps += value;
    else if (counter_name == "multi_get_qps")
        row.multi_get_qps += value;
    else if (counter_name == "put_qps")
        row.put_qps += value;
    else if (counter_name == "multi_put_qps")
        row.multi_put_qps += value;
    else if (counter_name == "remove_qps")
        row.remove_qps += value;
    else if (counter_name == "multi_remove_qps")
        row.multi_remove_qps += value;
    else if (counter_name == "incr_qps")
        row.incr_qps += value;
    else if (counter_name == "check_and_set_qps")
        row.check_and_set_qps += value;
    else if (counter_name == "check_and_mutate_qps")
        row.check_and_mutate_qps += value;
    else if (counter_name == "scan_qps")
        row.scan_qps += value;
    else if (counter_name == "recent.expire.count")
        row.recent_expire_count += value;
    else if (counter_name == "recent.filter.count")
        row.recent_filter_count += value;
    else if (counter_name == "recent.abnormal.count")
        row.recent_abnormal_count += value;
    else if (counter_name == "disk.storage.sst(MB)")
        row.storage_mb += value;
    else if (counter_name == "disk.storage.sst.count")
        row.storage_count += value;
    else if (counter_name == "rdb.block_cache.hit_count")
        row.rdb_block_cache_hit_count += value;
    else if (counter_name == "rdb.block_cache.total_count")
        row.rdb_block_cache_total_count += value;
    else if (counter_name == "rdb.block_cache.memory_usage")
        row.rdb_block_cache_mem_usage += value;
    else if (counter_name == "rdb.index_and_filter_blocks.memory_usage")
        row.rdb_index_and_filter_blocks_mem_usage += value;
    else if (counter_name == "rdb.memtable.memory_usage")
        row.rdb_memtable_mem_usage += value;
    else
        return false;
    return true;
}
inline bool
get_app_stat(shell_context *sc, const std::string &app_name, std::vector<row_data> &rows)
{
    std::vector<::dsn::app_info> apps;
    dsn::error_code err = sc->ddl_client->list_apps(dsn::app_status::AS_AVAILABLE, apps);
    if (err != dsn::ERR_OK) {
        derror("list apps failed, error = %s", err.to_string());
        return true;
    }

    ::dsn::app_info *app_info = nullptr;
    if (!app_name.empty()) {
        for (auto &app : apps) {
            if (app.app_name == app_name) {
                app_info = &app;
                break;
            }
        }
        if (app_info == nullptr) {
            derror("app %s not found", app_name.c_str());
            return true;
        }
    }

    std::vector<node_desc> nodes;
    if (!fill_nodes(sc, "replica-server", nodes)) {
        derror("get replica server node list failed");
        return true;
    }

    ::dsn::command command;
    command.cmd = "perf-counters";
    char tmp[256];
    if (app_name.empty()) {
        sprintf(tmp, ".*\\*app\\.pegasus\\*.*@.*");
    } else {
        sprintf(tmp, ".*\\*app\\.pegasus\\*.*@%d\\..*", app_info->app_id);
    }
    command.arguments.push_back(tmp);
    std::vector<std::pair<bool, std::string>> results;
    call_remote_command(sc, nodes, command, results);

    if (app_name.empty()) {
        std::map<int32_t, std::vector<dsn::partition_configuration>> app_partitions;
        for (::dsn::app_info &app : apps) {
            int32_t app_id = 0;
            int32_t partition_count = 0;
            dsn::error_code err = sc->ddl_client->list_app(
                app.app_name, app_id, partition_count, app_partitions[app.app_id]);
            if (err != ::dsn::ERR_OK) {
                derror("list app %s failed, error = %s", app_name.c_str(), err.to_string());
                return true;
            }
            dassert(app_id == app.app_id, "%d VS %d", app_id, app.app_id);
            dassert(partition_count == app.partition_count,
                    "%d VS %d",
                    partition_count,
                    app.partition_count);
        }

        rows.resize(app_partitions.size());
        int idx = 0;
        std::map<int32_t, int> app_row_idx; // app_id --> row_idx
        for (::dsn::app_info &app : apps) {
            rows[idx].row_name = app.app_name;
            app_row_idx[app.app_id] = idx;
            idx++;
        }

        for (int i = 0; i < nodes.size(); ++i) {
            dsn::rpc_address node_addr = nodes[i].address;
            if (!results[i].first) {
                derror("query perf counter info from node %s failed", node_addr.to_string());
                return true;
            }
            dsn::perf_counter_info info;
            dsn::blob bb(results[i].second.data(), 0, results[i].second.size());
            if (!dsn::json::json_forwarder<dsn::perf_counter_info>::decode(bb, info)) {
                derror("decode perf counter info from node %s failed, result = %s",
                       node_addr.to_string(),
                       results[i].second.c_str());
                return true;
            }
            if (info.result != "OK") {
                derror("query perf counter info from node %s returns error, error = %s",
                       node_addr.to_string(),
                       info.result.c_str());
                return true;
            }
            for (dsn::perf_counter_metric &m : info.counters) {
                int32_t app_id_x, partition_index_x;
                std::string counter_name;
                bool parse_ret = parse_app_pegasus_perf_counter_name(
                    m.name, app_id_x, partition_index_x, counter_name);
                dassert(parse_ret, "name = %s", m.name.c_str());
                auto find = app_partitions.find(app_id_x);
                if (find == app_partitions.end())
                    continue;
                dsn::partition_configuration &pc = find->second[partition_index_x];
                if (pc.primary != node_addr)
                    continue;
                update_app_pegasus_perf_counter(rows[app_row_idx[app_id_x]], counter_name, m.value);
            }
        }
    } else {
        rows.resize(app_info->partition_count);
        for (int i = 0; i < app_info->partition_count; i++)
            rows[i].row_name = boost::lexical_cast<std::string>(i);
        int32_t app_id = 0;
        int32_t partition_count = 0;
        std::vector<dsn::partition_configuration> partitions;
        dsn::error_code err =
            sc->ddl_client->list_app(app_name, app_id, partition_count, partitions);
        if (err != ::dsn::ERR_OK) {
            derror("list app %s failed, error = %s", app_name.c_str(), err.to_string());
            return true;
        }
        dassert(app_id == app_info->app_id, "%d VS %d", app_id, app_info->app_id);
        dassert(partition_count == app_info->partition_count,
                "%d VS %d",
                partition_count,
                app_info->partition_count);

        for (int i = 0; i < nodes.size(); ++i) {
            dsn::rpc_address node_addr = nodes[i].address;
            if (!results[i].first) {
                derror("query perf counter info from node %s failed", node_addr.to_string());
                return true;
            }
            dsn::perf_counter_info info;
            dsn::blob bb(results[i].second.data(), 0, results[i].second.size());
            if (!dsn::json::json_forwarder<dsn::perf_counter_info>::decode(bb, info)) {
                derror("decode perf counter info from node %s failed, result = %s",
                       node_addr.to_string(),
                       results[i].second.c_str());
                return true;
            }
            if (info.result != "OK") {
                derror("query perf counter info from node %s returns error, error = %s",
                       node_addr.to_string(),
                       info.result.c_str());
                return true;
            }
            for (dsn::perf_counter_metric &m : info.counters) {
                int32_t app_id_x, partition_index_x;
                std::string counter_name;
                bool parse_ret = parse_app_pegasus_perf_counter_name(
                    m.name, app_id_x, partition_index_x, counter_name);
                dassert(parse_ret, "name = %s", m.name.c_str());
                dassert(app_id_x == app_id, "name = %s", m.name.c_str());
                dassert(partition_index_x < partition_count, "name = %s", m.name.c_str());
                if (partitions[partition_index_x].primary != node_addr)
                    continue;
                update_app_pegasus_perf_counter(rows[partition_index_x], counter_name, m.value);
            }
        }
    }
    return true;
}

class table_printer
{
public:
    void add_title(const std::string &title)
    {
        dassert_f(matrix_data.empty() && max_col_width.empty(),
                  "`add_title` must be called only once");
        max_col_width.push_back(title.length());
        add_row(title);
    }

    void add_column(const std::string &col_name)
    {
        dassert_f(matrix_data.size() == 1,
                  "`add_column` must be called before real data appendding");
        max_col_width.emplace_back(col_name.length());
        append_data(col_name);
    }

    void add_row(const std::string &row_name)
    {
        matrix_data.emplace_back(std::vector<std::string>());
        append_data(row_name);
    }

    void append_data(uint64_t data) { append_data(std::to_string(data)); }

    void append_data(double data)
    {
        if (abs(data) < 1e-6) {
            append_data("0.00");
        } else {
            std::stringstream s;
            s << std::fixed << std::setprecision(precision) << data;
            append_data(s.str());
        }
    }

    void output(std::ostream &out) const
    {
        if (max_col_width.empty()) {
            return;
        }

        for (const auto &row : matrix_data) {
            dassert_f(!row.empty(), "Row name must be exist at least");
            out << std::setw(max_col_width[0] + space_width) << std::left << row[0];
            for (size_t i = 1; i < row.size(); ++i) {
                out << std::setw(max_col_width[i] + space_width) << std::right << row[i];
            }
            out << std::endl;
        }
    }

private:
    void append_data(const std::string &data)
    {
        matrix_data.rbegin()->emplace_back(data);

        // update column max length
        int &cur_len = max_col_width[matrix_data.rbegin()->size() - 1];
        if (cur_len < data.size()) {
            cur_len = data.size();
        }
    }

private:
    static const int precision = 2;
    static const int space_width = 2;
    std::vector<int> max_col_width;
    std::vector<std::vector<std::string>> matrix_data;
};
