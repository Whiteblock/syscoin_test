#ifndef SYSCOIN_HELPERS_H
#define SYSCOIN_HELPERS_H


#include <vector>
#include <queue>
#include <string>
#include <memory>
#include <mutex>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>

#include <stdlib.h>
#include <unistd.h>

#include "../rpc/sysrpc.h"

#include "../database/influx.h"
#include "../merger.h"

using namespace std;
using boost::multiprecision::int256_t;
using boost::multiprecision::cpp_dec_float_50;


namespace syscoin{
    struct globals{
        int sender_counter;
        int receiver_counter;
        int total_merge;
        int counter_merge;

        int64_t tps_start_time;
        cpp_dec_float_50 avg_test_start_time;
        cpp_dec_float_50 avg_tps;
        double low_compl_perc;
        double high_compl_perc;
        cpp_dec_float_50 total_time_avg;

        mutex send_all_lock;
        mutex send_all_lock_merge;
        mutex inc_lck;
        mutex sender_lock;
        mutex average_wait_lock;
        mutex avg_tps_lock;
        mutex gen_lock;

        globals(int senders,int receivers);
        virtual ~globals(){}
    };

    struct syscoin_test{
        string name;
        int senders;
        int receivers;
        int assets_total;
        int asset_sends_per_block;
        int assets_per_sender;
        int time_to_wait;
        double min_compl_percentage;
        string funder;
        globals* globs;
        influx* db;
        mutex merge_lock;
        merger generic_merge;

        syscoin_test(string name,int senders,int receivers,int assets,int asset_sends_per_block,int time_to_wait,double min_compl_percentage,influx* db);
        ~syscoin_test(){delete this->globs;}
        void merge();
        void await_calc_mean();
        void await_calc_mean_tps();
        void process_receiver_data(const string& raw_tps_info);
    };
    int64_t AssetNew(syscoin_test* test,const sys_rpc& w3,const string& address,const string& public_value,const string& contract,
                         int precision,const int256_t& supply,const int256_t& max_supply,int update_flags,const string& witness);

    vector<string> create_funded_addresses(syscoin_test* test,const sys_rpc& w0, const sys_rpc& w3, int notx);

    vector<string> generate_empty_addresses(const sys_rpc& node,int addresses);

    void generate(syscoin_test* test,const sys_rpc& node,int64_t blocks = 1);

    void get_extra_info(const sys_rpc& w3);

    string get_new_funded_address(syscoin_test* test,const sys_rpc& gen_node,const sys_rpc& node,const cpp_dec_float_50& amount);

    void peer_test(const sys_rpc& w3);

    void syscoin_debug(const sys_rpc& w3,uint64_t node);
    
};


#define MAX_TX_PER_CALL 100

#endif