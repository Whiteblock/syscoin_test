#include "syscoin.h"

#include <queue>
#include <memory>
#include <tuple>
#include <chrono>
#include <ostream>
#include <utility>
#include <mutex>
#include <algorithm>
#include <iterator>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <rapidjson/document.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>

#include "../client.h"
#include "../rpc_obj.h"
#include "../rpc_api.h"
#include "../rpc/sysrpc.h"
#include "../util.h"
#include "../ip.h"
#include "syscoin_helpers.h"


using namespace std;
using namespace rapidjson;
using boost::multiprecision::int256_t;
using boost::multiprecision::cpp_dec_float_50;


#define BALANCE 500'000'000
#define RETRIES 100000
#define NS_TO_SLEEP_AFTER_FAIL 10'000'000l//10ms

static vector<rpc_obj*> RPC_OBJECTS;

void signal_completion()
{
    pthread_t thread;
    pthread_create(&thread,nullptr,client::trd_next_if_exists,nullptr);
}

routine syscoin::create_test(influx* db,const string& name,const string& funder,const vector<string>& senders,const vector<string>& receivers,int time_to_wait,
                    double min_compl_percentage,int assets,int asset_sends_per_block)
{
    int port = 8369;
    routine r("syscoin::create_test");

    syscoin_test* test_params = new syscoin_test(name,senders.size(),receivers.size(),assets,asset_sends_per_block,time_to_wait,min_compl_percentage,db);//

    r.add_routine(rpc_api([](void*)-> void* {return nullptr;},funder,port,nullptr));

    for(const auto& sender : senders){
        r.add_routine(rpc_api(syscoin::rpc_sender,sender,port,test_params));
    }
    for(const auto& receiver : receivers){
        r.add_routine(rpc_api(syscoin::rpc_receiver,receiver,port,test_params));
    }
    
    r.set_init_routine(syscoin::rpc_init);
    return r;
}


void* syscoin::rpc_sender(void* in)
{
    auto ro = static_cast<rpc_obj*>(in);
    auto test = static_cast<syscoin_test*>(ro->get_extras());

    sys_rpc w0(RPC_OBJECTS[0]);//Funder
    sys_rpc w3(ro);
    //size_t index = rpc->get_index();
    const int tx_to_send = test->assets_per_sender * test->asset_sends_per_block;

    vector< vector<string> > vvtx;
    vvtx.reserve(tx_to_send / MAX_TX_PER_CALL);

    vector<string> funded_addresses;
    funded_addresses.reserve(test->assets_per_sender);

    vector<string> backup_funded_addresses;

    vector<string> unfunded_addresses;

    {
        scoped_lock<mutex> lock(test->globs->sender_lock);
        //PHASE 1:  GENERATE UNFUNDED ADDRESSES FOR RECIPIENTS TO ASSETALLOCATIONSEND
        unfunded_addresses = syscoin::generate_empty_addresses(w3,tx_to_send);

        //PHASE 2:  GENERATE FUNDED ADDRESSES FOR CREATING AND SENDING ASSETS
        for(int i = test->assets_per_sender; i > 0; i -= MAX_TX_PER_CALL){
            int txno = (i > MAX_TX_PER_CALL) ? MAX_TX_PER_CALL : i;
            auto addrs = syscoin::create_funded_addresses(test,w0,w3,txno);
            std::move(addrs.begin(),addrs.end(),std::back_inserter(funded_addresses));
        }
        //PHASE 2b: GENERATE ~10% BACKUP FUNDED ADDRESSES
        for(int i = (test->assets_per_sender/10) + 1; i > 0; i -= MAX_TX_PER_CALL){
            int txno = (i > MAX_TX_PER_CALL) ? MAX_TX_PER_CALL : i;
            auto addrs = syscoin::create_funded_addresses(test,w0,w3,txno);
            std::move(addrs.begin(),addrs.end(),std::back_inserter(backup_funded_addresses));
        }

    }

    //PHASE 3:  SET tpstestsetenabled ON ALL SENDER/RECEIVER NODES
    test->generic_merge();

    w3.tpstestsetenabled(true);
    {
        scoped_lock<mutex> lock(test->globs->sender_lock);
        syscoin::generate(test,w3,101);
    }
    vector<int64_t> assets;
    queue<int64_t> backup_assets;
    vector<string> raw_signed_asset_alloc_sends;
    {
        scoped_lock<mutex> lock(test->globs->sender_lock);
        cout<<"\033[101mPHASE 4\033[0m\n";
        // PHASE 4:  CREATE ASSETS
        for(int i = 0; i < test->assets_per_sender; i++){
            
            const string& address = funded_addresses[i];
            auto guid = syscoin::AssetNew(test,w3,address,"","",8,test->asset_sends_per_block,test->asset_sends_per_block,31,"");
            assets.emplace_back(guid);  
        }
        cout<<"\033[101mPHASE 4b\033[0m\n";
        //PHASE 4b: CREATE ASSET_BACKUPS
        for(const auto& address : backup_funded_addresses){
            int64_t guid = syscoin::AssetNew(test,w3,address,"","",8,test->asset_sends_per_block,test->asset_sends_per_block,31,"");
            backup_assets.emplace(guid);  
        }
        cout<<"\033[101mPHASE 4c\033[0m\n";
        for(auto it = assets.begin(); it != assets.end(); it++){
            try{
                cout<<"Checking\n";
                w3.assetinfo(*it);
            }catch(const std::runtime_error& rte){
                cout<<"Asset was not found\nRecovering...\n\n\n";
                if(!backup_assets.empty()){
                    *it = backup_assets.front();
                    backup_assets.pop();
                    util::sleep(1);
                    it--;
                }
                
            }
        }
        // PHASE 5:  SEND ASSETS TO NEW ALLOCATIONS 
        cout<<"\033[101mPHASE 5\033[0m\n";
        for(int i = 0; i < test->assets_per_sender; i++){
            cout<<"\033[101mPHASE 5\033[0m\n";

            vector<string> arr = util::multi_try<std::runtime_error,vector<string>>(RETRIES,    
                    [&w3,&assets,&funded_addresses,i](){return w3.assetsend(assets[i],funded_addresses[i],250,"");});
            
            arr = util::multi_try<std::runtime_error,vector<string>>(RETRIES,    
                    [&w3,&arr,&funded_addresses,i](){
                        return w3.syscointxfund(arr[0],funded_addresses[i]);
                    },NS_TO_SLEEP_AFTER_FAIL);

            string hex_str = util::multi_try<std::runtime_error,string>(RETRIES,    
                    [&w3,&arr](){return w3.signrawtransactionwithwallet(arr[0]);});

            w3.sendrawtransaction(hex_str,true);
            syscoin::generate(test,w3);
        }
        cout<<"\033[101mPHASE 6\033[0m\n";
        // PHASE 6:  SEND ALLOCATIONS TO NEW ALLOCATIONS (UNFUNDED ADDRESSES) USING ZDAG

        for(int i = 0; i < test->assets_per_sender; i++){

            vector<pair<string,cpp_dec_float_50>> send_many;
            for(int j = 0; j < test->asset_sends_per_block;j++){
                send_many.emplace_back(make_pair(unfunded_addresses[(i*test->asset_sends_per_block)+j],1));
            }
          
            vector<string> result = util::multi_try<std::runtime_error,vector<string>>(RETRIES,    
                    [&w3,&assets,&funded_addresses,&send_many,i](){
                        return w3.assetallocationsend(assets[i],funded_addresses[i],send_many);
                    });

            result = util::multi_try<std::runtime_error,vector<string>>(RETRIES,    
                    [&w3,&result,&funded_addresses,i](){
                        return w3.syscointxfund(result[0],funded_addresses[i]);
                    },NS_TO_SLEEP_AFTER_FAIL);

            string hex_str = w3.signrawtransactionwithwallet(result[0]);
            raw_signed_asset_alloc_sends.emplace_back(move(hex_str));
            syscoin::generate(test,w3);
        }
    }

    bool clear = false;
    {
        scoped_lock<mutex> lck(test->globs->inc_lck);
        test->globs->sender_counter--;
        clear = (test->globs->sender_counter == 0);
        
    }
    if(clear){
        test->globs->sender_counter = test->senders;
        test->globs->send_all_lock.unlock();
        test->globs->tps_start_time = chrono::duration_cast<chrono::microseconds>(
                    chrono::high_resolution_clock::now().time_since_epoch()).count() + (1* 1000 * 1000);
        cout<<"Sending all of the txs via tpstestadd\nTime:"<< test->globs->tps_start_time <<endl;
    }else{
        //Wait for all threads to finish
        test->globs->send_all_lock.lock();
        test->globs->send_all_lock.unlock();
    }

    // PHASE 7:  DISTRIBUTE LOAD AMONG SENDERS
    // Note: Load is already pre distributed among senders,so this is just the tpstestadd call
    
    w3.tpstestadd(0,raw_signed_asset_alloc_sends);
    
    // PHASE 8:  CALL tpstestadd ON ALL SENDER/RECEIVER NODES WITH A FUTURE TIME
    w3.tpstestadd(test->globs->tps_start_time);

    test->merge();

    // PHASE 9:  WAIT 10 SECONDS + DELAY SET ABOVE (1 SECOND)
    
    cout<<"Waiting "<<test->time_to_wait<<" seconds as per the protocol\n";
    util::sleep(test->time_to_wait);

    const string tps_test_info = w3.tpstestinfo();//line 406

    Document data;
    data.Parse(tps_test_info.c_str());
     if(!data["result"].IsObject() || !data["result"].HasMember("teststarttime") || !data["result"]["teststarttime"].IsInt64()){
        cerr<<"Error with JSON Response to tpstestinfo for the sender node, aborting...\n";
        cerr<<"RAW is "+tps_test_info+"\n";
        raise(SIGTERM);
    }
    int64_t test_start_time = data["result"]["teststarttime"].GetInt64();
    #ifdef VERBOSE
        cout<<"test_start_time = "+to_string(test_start_time)+"\n";
    #endif
    {
        scoped_lock<mutex> lock(test->globs->inc_lck);
        test->globs->avg_test_start_time += test_start_time;
    }

    test->await_calc_mean();
    

    
    w3.tpstestsetenabled(false);

    {
        scoped_lock<mutex> lock(test->globs->inc_lck);
        test->globs->counter_merge--;
        if(test->globs->counter_merge == 0){
            //Automatically terminate the program if all threads have finished
            cout<<"All threads have finished. Signaling completion\n";
            
            int success = (test->globs->low_compl_perc > test->min_compl_percentage) ? 1 : 0;
            
            test->db->insert(12,test->name.c_str(),test->senders,test->receivers,test->assets_total,test->asset_sends_per_block,
                            test->time_to_wait,test->min_compl_percentage,(long long)test->globs->avg_test_start_time,
                            (long long)test->globs->total_time_avg,(double)test->globs->avg_tps,test->globs->low_compl_perc,
                            test->globs->high_compl_perc,success);
            delete test;
            signal_completion();
        }
    }
    cout<<"A sender thread has finished\n";
    
    return nullptr;
}

void* syscoin::rpc_receiver(void* in)
{
    auto rpc = static_cast<rpc_obj*>(in);
    sys_rpc w3(rpc);
    auto test = static_cast<syscoin_test*>(rpc->get_extras());
    // PHASE 3:  SET tpstestsetenabled ON ALL SENDER/RECEIVER NODES
    test->generic_merge();
    w3.tpstestsetenabled(true);

    test->merge();
    
    // PHASE 8:  CALL tpstestadd ON ALL SENDER/RECEIVER NODES WITH A FUTURE TIME
    w3.tpstestadd(test->globs->tps_start_time);
    
    // PHASE 9:  WAIT 10 SECONDS + DELAY SET ABOVE (1 SECOND)
    cout<<"Wait "<<test->time_to_wait<<" seconds as per the protocol\n";
    util::sleep(test->time_to_wait);

    test->await_calc_mean();

    test->process_receiver_data(w3.tpstestinfo());
    
    w3.tpstestsetenabled(false);
    //Automatically terminate the program if all threads have finished
    bool clear = false;
    {
        scoped_lock<mutex> lock(test->globs->inc_lck);
        test->globs->counter_merge--;
        clear = (test->globs->counter_merge == 0);
    }
    
    if(clear){
        cout<<"A receiver thread has finished\n"
            "All threads have finished. Signaling completion\n";
        int success = (test->globs->low_compl_perc > test->min_compl_percentage) ? 1 : 0;
        test->db->insert(12,test->name.c_str(),test->senders,test->receivers,test->assets_total,test->asset_sends_per_block,
                        test->time_to_wait,test->min_compl_percentage,(long long)test->globs->avg_test_start_time,
                        (long long)test->globs->total_time_avg,(double)test->globs->avg_tps,test->globs->low_compl_perc,
                        test->globs->high_compl_perc,success);
        delete test;
        signal_completion();
    }
    
    cout<<"A receiver thread has finished\n";
    return nullptr;
}


void* syscoin::rpc_init(void* in)
{
    auto rpcs = static_cast<vector<rpc_obj*>*>(in);
    //auto test = static_cast<syscoin_test*>(rpcs->at(1)->get_extras());
    #ifdef VERBOSE
        cout<<"Syncing the nodes\n";
    #endif
         //Do the weird syncing stuff
    for(auto& it : *rpcs){
        sys_rpc w3(it);
        w3.generate(1);
        //w3.mnsync("next");
    }
    #ifdef VERBOSE
        cout<<"Nodes have been synced\n";
    #endif
    sys_rpc gen_node(rpcs->at(0));

    RPC_OBJECTS.clear();
    RPC_OBJECTS.reserve(rpcs->size());
    size_t index = 0;
    for(auto& it : *rpcs){
        sys_rpc w3(it);
        RPC_OBJECTS.push_back(it);
        //vector<string> all_addrs;
        //vector<string> addrs;
        #ifdef VERBOSE
            cout<<"Setting up node "<<index<<endl;
        #endif
        if(index == 0){
            w3.getnewaddress();//500,000,000 syscoin, so don't fund
            util::multi_try<std::runtime_error,void>(RETRIES,[&w3](){w3.generate(101);});
            syscoin::get_new_funded_address(nullptr,w3,w3,BALANCE);
            util::multi_try<std::runtime_error,void>(RETRIES,[&w3](){w3.generate(101);});
        }
        w3.mnsync("next");
        //syscoin::syscoin_debug(w3,index);
        index++;
    }
    cout<<"Sleeping for 5 seconds to ensure everything has propogated\n";
    util::sleep(5);

    #ifdef VERBOSE
        cout<<"Init routine has finished!\n";
    #endif

    return nullptr;
}