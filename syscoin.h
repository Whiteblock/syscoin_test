#ifndef SYSCOIN_H
#define SYSCOIN_H


#include <string>
#include <vector>
#include "../routine.h"
#include "../database/influx.h"

namespace syscoin{
	routine create_test(influx* db,const string& name,const string& funder,const vector<string>& senders,const vector<string>& receivers,int time_to_wait,
						double min_compl_percentage,int assets,int asset_sends_per_block);

	void* rpc_receiver(void* in);

	void* rpc_sender(void* in);

	void* rpc_init(void* in);
};




#endif