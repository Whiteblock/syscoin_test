#include "syscoin_helpers.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <assert.h>
#include <rapidjson/document.h>
#include "../util.h"

using namespace std;
using namespace rapidjson;
using boost::multiprecision::int256_t;
using boost::multiprecision::cpp_dec_float_50;

#define RETRIES 100000
#define NS_TO_SLEEP_AFTER_FAIL 10'000'000l//10ms

//3.2
int64_t syscoin::AssetNew(syscoin_test* test,const sys_rpc& w3,const string& address,const string& public_value,const string& contract,
                         int precision,const int256_t& supply,const int256_t& max_supply,int update_flags,const string& witness)
{
    pair<string,int64_t> asset_data = util::multi_try<std::runtime_error,pair<string,int64_t>>(RETRIES,
            [&](){
                return w3.assetnew(address,public_value,contract,"",precision,
                                   supply,max_supply,update_flags,witness);
            },NS_TO_SLEEP_AFTER_FAIL);

    syscoin::generate(test,w3);

    vector<string> arr = util::multi_try<std::runtime_error,vector<string>>(RETRIES,
            [&](){return w3.syscointxfund(asset_data.first,address);},NS_TO_SLEEP_AFTER_FAIL);

    string signed_tx_hex = util::multi_try<std::runtime_error,string>(RETRIES,
                [&w3,&arr](){return w3.signrawtransactionwithwallet(arr[0]);});

    util::multi_try<std::runtime_error,void>(RETRIES,
        [&w3,&signed_tx_hex](){w3.testmempoolaccept({signed_tx_hex});});

    util::multi_try<std::runtime_error,void>(RETRIES,    
        [&w3,&signed_tx_hex](){w3.sendrawtransaction(signed_tx_hex,true);});

    return asset_data.second;
}

vector<string> syscoin::create_funded_addresses(syscoin_test* test,const sys_rpc& w0, const sys_rpc& w3, int notx)
{
    const cpp_dec_float_50 balance = 2000; 
    //cout<<"Creating and funding addresses";
    auto addresses = syscoin::generate_empty_addresses(w3,notx*2);


    for(int i = 0; i < notx * 2;i += 200){
        int tx_to_process = 200;
        if(((notx *2) - i) < 200){
            tx_to_process = (notx *2) - i;
        }
        if(tx_to_process == 0){
            cerr<<"Warning: Tried to give 0 transactions to process"<<endl;
            break;
        }
        vector<pair<string,cpp_dec_float_50>> amounts;

        //amounts.reserve(tx_to_process);
        for(int j = i; j < i+tx_to_process; j++){
            amounts.emplace_back(addresses[j],balance);
        }
        util::multi_try<std::runtime_error,void>(RETRIES,
            [&w0,&amounts](){w0.sendmany(amounts);});
        
        syscoin::generate(test,w3);     
    }
    syscoin::generate(test,w3);
    
    return addresses;
}

void syscoin::get_extra_info(const sys_rpc& w3)
{
    w3.getpoolinfo();
    w3.getmempoolinfo();
}

string syscoin::get_new_funded_address(syscoin_test* test,const sys_rpc& gen_node,const sys_rpc& node,const cpp_dec_float_50& amount)
{
    string address = node.getnewaddress();
    //cout<<"get_new_funded_address init balance ::: "+util::ftos(node.getaddressbalance(address)) + "\n";
    util::multi_try<std::runtime_error,void>(RETRIES,    
        [&gen_node,&address,&amount](){gen_node.sendtoaddress(address,amount);});

    
    //node.generatetoaddress(101,address);
    if(test == nullptr){
        node.generate(1);
        gen_node.generate(1);
        return address;
    }
    syscoin::generate(test,node);
    syscoin::generate(test,gen_node);
    

    return address;
}

vector<string> syscoin::generate_empty_addresses(const sys_rpc& node,int addresses)
{
    vector<string> out;
    //out.reserve(addresses);
    for(int i = 0; i < addresses; i++){
        out.emplace_back(node.getnewaddress());
    }
    return out;
}

void syscoin::generate(syscoin_test* test,const sys_rpc& node,int64_t blocks)
{
    scoped_lock<mutex> lock(test->globs->gen_lock);
    bool success = false;
    int count = 0;
    while(!success && count < 20){
        try{
            node.generate(blocks);
            success = true;
        }catch(const std::runtime_error& re){
            cerr<<re.what()<<endl;
            util::sleep(0,300'000'000);
        }
        count++;
    }
    if(count >= 20){
        throw std::runtime_error("syscoin::generate");
    }
    
}


void syscoin::peer_test(const sys_rpc& w3)
{
    cout<<"Connection Count: "<<w3.getconnectioncount()<<'\n';
    cout<<"Network Info:"<<w3.getnetworkinfo()<<'\n';
}

void syscoin::syscoin_debug(const sys_rpc& w3,uint64_t node)
{
    cout<<"------Node "<<node<<"------\n";
    peer_test(w3);
    cout<<"Balance: "<<util::ftos(w3.getbalance())<<'\n';
    cout<<"Block Number: "<<w3.getblockcount()<<'\n';
}

syscoin::globals::globals(int senders,int receivers) :
    sender_counter(senders),
    receiver_counter(receivers),
    total_merge(senders + receivers),
    counter_merge(senders + receivers),
    avg_test_start_time(0),
    avg_tps(0),
    low_compl_perc(100),
    high_compl_perc(0),
    total_time_avg(0)
{

    this->send_all_lock.lock();
    this->send_all_lock_merge.lock();
    this->average_wait_lock.lock();
    this->avg_tps_lock.lock();
}

syscoin::syscoin_test::syscoin_test(string name,int senders,int receivers,int assets,int asset_sends_per_block,int time_to_wait,double min_compl_percentage,influx* db) :
    name(move(name)),
    senders(senders),
    receivers(receivers),
    assets_total(assets - (assets % senders) ),
    asset_sends_per_block(asset_sends_per_block),
    assets_per_sender(assets/senders),
    time_to_wait(time_to_wait),
    min_compl_percentage(min_compl_percentage),
    globs(new syscoin::globals(senders,receivers)),
    db(db),
    generic_merge(senders+receivers,[](){cout<<"Merged\n";})
{}



void syscoin::syscoin_test::merge()
{
    {   
        scoped_lock<mutex> lock(this->globs->inc_lck);
        this->globs->counter_merge--;
        if(this->globs->counter_merge == 0){
            this->globs->counter_merge = this->globs->total_merge;
            this->globs->send_all_lock_merge.unlock();
            cout<<"Synced up and read to parse the results\n";
        }
    }
    //Wait for all threads to finish
    this->globs->send_all_lock_merge.lock();
    this->globs->send_all_lock_merge.unlock();
}



void syscoin::syscoin_test::await_calc_mean()
{
    {   
        scoped_lock<mutex> lock(this->globs->inc_lck);
        this->globs->counter_merge--;
        if(this->globs->counter_merge == 0){
            this->globs->counter_merge = this->globs->total_merge;
            this->globs->avg_test_start_time /= this->senders;
            this->globs->average_wait_lock.unlock();
            #ifdef VERBOSE
                cout<<"Average has been calculated on the Senders, now having the receivers process then rest of the data\n";
                cout<<"avg_test_start_time = "<<util::ftos(this->globs->avg_test_start_time)<<endl;
            #endif
        }
    }
    //Wait for all threads to finish
    this->globs->average_wait_lock.lock();
    this->globs->average_wait_lock.unlock();
}

void syscoin::syscoin_test::await_calc_mean_tps()
{
    {   
        scoped_lock<mutex> lock(this->globs->inc_lck);
        this->globs->receiver_counter--;

        if(this->globs->receiver_counter == 0){
            this->globs->receiver_counter = this->receivers;
            this->globs->avg_tps /= this->receivers;
            this->globs->total_time_avg /= this->receivers;

            this->globs->avg_tps_lock.unlock();

            #ifdef VERBOSE
                cout<<"Average has been calculated on the Recievers\n";
                cout<<"avg_tps = "<<util::ftos(this->globs->avg_tps)<<endl;
                cout<<this->globs->total_time_avg<<endl;
            #endif
        }
    }
    //Wait for all threads to finish
    this->globs->avg_tps_lock.lock();
    this->globs->avg_tps_lock.unlock();
}


void syscoin::syscoin_test::process_receiver_data(const string& raw_tps_info)
{
    int64_t total_tx = this->assets_total * this->asset_sends_per_block; 
    int64_t total_addr = total_tx * 2;
    cpp_dec_float_50 total_time = 0,  throughput;
    Document data;
    data.Parse(raw_tps_info.c_str());
    const Value& results = data["result"];

    const Value& tpsresponsereceivers = results["receivers"];

    if(!tpsresponsereceivers.IsArray()){
        cerr<<"Unexpected receivers result\n";
    }
    
    for(const auto& tpsresponsereceiver : tpsresponsereceivers.GetArray()){
        total_time += tpsresponsereceiver["time"].GetInt64() - this->globs->avg_test_start_time;    
    }
    int64_t total_processed(0);
    if (tpsresponsereceivers.Size() > 0 ){
        total_time /= tpsresponsereceivers.Size();
        total_processed = tpsresponsereceivers.Size();
    }

    cout<<"Processed ~"<<total_processed * this->asset_sends_per_block<<"/"<<total_tx<<'\n';

    double percent_completed = total_processed * this->asset_sends_per_block;
    percent_completed /= total_tx;

    
    if(total_time != 0){
        throughput = total_tx * 1000000;//or total_processed?
        throughput /= total_time;
    }
    
    {
        std::scoped_lock<std::mutex> lock(this->globs->inc_lck);
        this->globs->avg_tps += throughput;
        this->globs->total_time_avg += total_time;
        if(percent_completed > this->globs->high_compl_perc){
            this->globs->high_compl_perc = percent_completed;
        }

        if(this->globs->low_compl_perc > percent_completed) {
            this->globs->low_compl_perc = percent_completed;
        }
    }
    
    
    this->await_calc_mean_tps();

    string output;
    output += to_string(this->receivers + this->senders + 1) + ",";
    output += to_string(total_tx) + ",";
    output += to_string(total_addr) + ",";
    output += util::ftos(this->globs->avg_test_start_time,10) + ",";
    output += util::ftos(total_time,10) + ",";
    output += util::ftos(throughput,10) + ",";
    output += util::ftos(this->globs->avg_tps,10);
    cout<<output<<endl;
}