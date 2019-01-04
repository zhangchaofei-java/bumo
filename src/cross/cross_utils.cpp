/*
	bumo is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	bumo is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with bumo.  If not, see <http://www.gnu.org/licenses/>.
	*/

#include <glue/glue_manager.h>
#include <overlay/peer_manager.h>
#include<ledger/fee_calculate.h>
#include <utils/base_int.h>
#include "cross_utils.h"

#define MAX_SEND_TRANSACTION_TIMES 50
namespace bumo {
	typedef std::shared_ptr<MerkleNode> MerkleNodePointer;
	int32_t CrossUtils::QueryContract(const std::string &address, const std::string &input, Json::Value &query_rets){
		std::string result = "";
		ContractTestParameter parameter;
		parameter.code_ = "";
		parameter.input_ = input;
		parameter.opt_type_ = ContractTestParameter::QUERY;
		parameter.contract_address_ = address;
		parameter.source_address_ = "";
		parameter.fee_limit_ = 1000000000000;
		parameter.gas_price_ = LedgerManager::Instance().GetCurFeeConfig().gas_price();
		parameter.contract_balance_ = 1000000000000;

		int32_t error_code = protocol::ERRCODE_SUCCESS;
		AccountFrm::pointer acc = NULL;
		utils::Sleep(10);
		do {
			if (parameter.contract_address_.empty()) {
				error_code = protocol::ERRCODE_INVALID_PARAMETER;
				result = "Empty contract address";
				LOG_ERROR("%s", result.c_str());
				break;
			}

			if (!Environment::AccountFromDB(parameter.contract_address_, acc)) {
				error_code = protocol::ERRCODE_NOT_EXIST;
				result = utils::String::Format("Account(%s) is not existed", parameter.contract_address_.c_str());
				LOG_ERROR("Failed to load the account from the database. %s", result.c_str());
				break;
			}

			parameter.code_ = acc->GetProtoAccount().contract().payload();

			if (parameter.code_.empty()) {
				error_code = protocol::ERRCODE_NOT_EXIST;
				result = utils::String::Format("Account(%s) has no contract code", parameter.contract_address_.c_str());
				LOG_ERROR("Failed to load test parameter. %s", result.c_str());
				break;
			}

			Result exe_result;
			Json::Value result_json = Json::Value(Json::objectValue);
			if (!LedgerManager::Instance().context_manager_.SyncTestProcess(LedgerContext::AT_TEST_V8,
				(TestParameter*)&parameter,
				utils::MICRO_UNITS_PER_SEC,
				exe_result, result_json["logs"], result_json["txs"], result_json["query_rets"], result_json["stat"])) {
				error_code = exe_result.code();
				result = exe_result.desc();
				LOG_ERROR("Failed to execute the test.%s", result.c_str());
				break;
			}

			query_rets = result_json["query_rets"];
			if (query_rets[Json::UInt(0)].isMember("error")){
				error_code = protocol::ERRCODE_CONTRACT_EXECUTE_FAIL;
				LOG_ERROR("Query Contract error,reason:%s", query_rets[Json::UInt(0)]["error"]["exception"].asString().c_str());
				break;
			}
		} while (false);

		//LOG_INFO("Query result code:%d, result:%s", error_code, result.c_str());

		return error_code;
	}

	TransactionSender::TransactionSender(){
		enabled_ = false;
		thread_ptr_ = NULL;
		cur_nonce_ = 0;
		last_update_time_ = utils::Timestamp::HighResolution();
	}
	TransactionSender::~TransactionSender(){
		if (thread_ptr_){
			delete thread_ptr_;
			thread_ptr_ = NULL;
		}
	}

	bool TransactionSender::Initialize(const std::string &private_key){
		enabled_ = true;
		thread_ptr_ = new utils::Thread(this);
		if (!thread_ptr_->Start("TransactionSender")) {
			return false;
		}

		PrivateKey pkey(private_key);
		if (!pkey.IsValid()){
			LOG_ERROR("Private key is not valid");
			return false;
		}

		private_key_ = private_key;
		source_address_ = pkey.GetEncAddress();
		return true;
	}

	bool TransactionSender::Exit(){
		enabled_ = false;
		if (thread_ptr_) {
			thread_ptr_->JoinWithStop();
		}
		return true;
	}

	void TransactionSender::AsyncSendTransaction(ITransactionSenderNotify *notify, const TransTask &trans_task){
		assert(trans_task.amount_ >= 0);
		assert(!trans_task.dest_address_.empty());
		assert(!trans_task.input_paras_.empty());
		assert(notify != nullptr);

		utils::MutexGuard guard(task_vector_lock_);
		TransTaskVector &task_vector = trans_task_map_[notify];
		task_vector.push_back(trans_task);
	}
	void TransactionSender::Run(utils::Thread *thread){
		while (enabled_){
			utils::Sleep(10);
			int64_t current_time = utils::Timestamp::HighResolution();
			if ((current_time - last_update_time_) <= 5 * utils::MICRO_UNITS_PER_SEC){
				continue;
			}

			SendingAll();
			last_update_time_ = current_time;
		}
	}

	void TransactionSender::SendingAll(){
		TransTaskMap temp_map;
		do
		{
			utils::MutexGuard guard(task_vector_lock_);
			temp_map.swap(trans_task_map_);
			trans_task_map_.clear();
			assert(trans_task_map_.size() == 0);
		} while (false);
		if (temp_map.empty()){
			return;
		}

		AccountFrm::pointer account_ptr;
		if (!Environment::AccountFromDB(source_address_, account_ptr)) {
			LOG_ERROR("Address:%s not exsit", source_address_.c_str());
			return;
		}
		cur_nonce_ = account_ptr->GetAccountNonce() + 1;

		for (TransTaskMap::const_iterator itr = temp_map.begin(); itr != temp_map.end(); itr++){
			ITransactionSenderNotify* notify = itr->first;
			const TransTaskVector &task_vector = itr->second;
			for (uint32_t i = 0; i < task_vector.size(); i++){
				const TransTask &trans_task = task_vector[i];
				TransTaskResult task_result = SendingSingle(trans_task.input_paras_, trans_task.dest_address_);
				notify->HandleTransactionSenderResult(trans_task, task_result);
			}
		}
	}

	TransTaskResult TransactionSender::SendingSingle(const std::vector<std::string> &paras, const std::string &dest){
		int32_t err_code = 0;

		for (int i = 0; i <= MAX_SEND_TRANSACTION_TIMES; i++){
			TransactionFrm::pointer trans = BuildTransaction(private_key_, dest, paras, cur_nonce_);
			if (nullptr == trans){
				LOG_ERROR("Trans pointer is null");
				continue;
			}
			std::string hash = utils::String::BinToHexString(trans->GetContentHash().c_str());
			err_code = SendTransaction(trans);
			switch (err_code)
			{
			case protocol::ERRCODE_SUCCESS:
			case protocol::ERRCODE_ALREADY_EXIST:{
													 cur_nonce_++;
													 TransTaskResult task_result(true, "", hash);
													 return task_result;
			}
			case protocol::ERRCODE_BAD_SEQUENCE:{
													cur_nonce_++;
													continue;
			}
			default:{
						LOG_ERROR("Send transaction erro code:%d", err_code);
						continue;
			}
			}

			utils::Sleep(10);
		}

		TransTaskResult task_result(false, "Try MAX_SEND_TRANSACTION_TIMES times", "");
		return task_result;
	}

	TransactionFrm::pointer TransactionSender::BuildTransaction(const std::string &private_key, const std::string &dest, const std::vector<std::string> &paras, int64_t nonce){
		PrivateKey pkey(private_key);
		if (!pkey.IsValid()){
			LOG_ERROR("Private key is not valid");
			return nullptr;
		}

		std::string source_address = pkey.GetEncAddress();

		protocol::TransactionEnv tran_env;
		protocol::Transaction *tran = tran_env.mutable_transaction();

		tran->set_source_address(source_address);
		tran->set_nonce(nonce);
		for (unsigned i = 0; i < paras.size(); i++){
			protocol::Operation *ope = tran->add_operations();
			ope->set_type(protocol::Operation_Type_PAY_COIN);
			protocol::OperationPayCoin *pay_coin = ope->mutable_pay_coin();
			pay_coin->set_amount(0);
			pay_coin->set_dest_address(dest);
			pay_coin->set_input(paras[i]);
		}

		tran->set_gas_price(LedgerManager::Instance().GetCurFeeConfig().gas_price());
		tran->set_chain_id(General::GetSelfChainId());
		int64_t fee_limit = 0;
		//300 is signature byte
		if (!utils::SafeIntMul(tran->gas_price(), ((int64_t)tran_env.ByteSize() + 300), fee_limit)){
			LOG_ERROR("Failed to evaluate fee.");
			return nullptr;
		}
		fee_limit = fee_limit * 5;
		tran->set_fee_limit(fee_limit);

		std::string content = tran->SerializeAsString();
		std::string sign = pkey.Sign(content);
		protocol::Signature *signpro = tran_env.add_signatures();
		signpro->set_sign_data(sign);
		signpro->set_public_key(pkey.GetEncPublicKey());

		std::string tx_hash = utils::String::BinToHexString(HashWrapper::Crypto(content)).c_str();
		LOG_INFO("Pay coin tx hash %s", tx_hash.c_str());

		TransactionFrm::pointer ptr = std::make_shared<TransactionFrm>(tran_env);
		return ptr;
	}

	int32_t TransactionSender::SendTransaction(TransactionFrm::pointer tran_ptr) {
		Result result;
		GlueManager::Instance().OnTransaction(tran_ptr, result);
		if (result.code() != 0) {
			LOG_ERROR("Pay coin result code:%d, des:%s", result.code(), result.desc().c_str());
			return result.code();
		}

		PeerManager::Instance().Broadcast(protocol::OVERLAY_MSGTYPE_TRANSACTION, tran_ptr->GetProtoTxEnv().SerializeAsString());
		return protocol::ERRCODE_SUCCESS;
	}

	//merkel tree

	MerkleNode::MerkleNode(){}
	MerkleNodePointer MerkleNode::GetParent(){
		return parent_;
	}
	void  MerkleNode::SetChildren(const MerkleNodePointer &children_l, const MerkleNodePointer &children_r){
		left_node_ = children_l;
		right_node_ = children_r;
	}
	MerkleNodePointer MerkleNode::GetChildrenLeft(){
		return left_node_;
	}

	MerkleNodePointer MerkleNode::GetChildrenRight(){
		return right_node_;
	}

	void MerkleNode::SetParent(const MerkleNodePointer &parent){
		parent_ = parent;
	}


	string MerkleNode::GetHash(){
		return hash_;
	}

	MerkleNodePointer MerkleNode::GetSibling(){
		// the left child gets the right child, and the right child gets the left child
		// gets the parent node of the node
		// determine whether the left child of the parent node is the same as this node
		// same return right child, different return left child
		MerkleNodePointer parent = this->GetParent();
		return parent->GetChildrenLeft() == parent ? parent->GetChildrenRight() : parent->GetChildrenLeft();
	}

	void MerkleNode::SetHash(const std::string &leaf_hash){
		hash_ = utils::String::BinToHexString(HashWrapper::Crypto(leaf_hash)).c_str();
	}

	MerkleNode::~MerkleNode(){}

	int64_t MerkleNode::CheckDir(){
		// returns 1 if the left child of the parent node is 0
		return parent_->left_node_ == std::shared_ptr<MerkleNode>(this) ? 0 : 1;
	}

	bool MerkleNode::IsLeaf(){ return left_node_ == nullptr && right_node_ == nullptr; }

	MerkleTree::MerkleTree() {}
	MerkleTree::~MerkleTree() {}

	// as merkle algorithm requires that leaf nodes are even in blockchain, when a block contains an odd number of transactions,
	// make an even number of copies of the last transaction.
	int64_t MerkleTree::MakeBinary(std::vector<MerkleNodePointer> &node_vector){
		// make leaf nodes double
		int64_t length = node_vector.size();

		// If the number of elements is odd, then put the last node's push_back once
		if ((length % 2) != 0){
			node_vector.push_back(node_vector.end()[-1]); //The last element of push_back,end() - 1
			length++;
		}
		return length;
	}

	//build merkle tree
	void MerkleTree::BuildTree(const vector<string> &base_leafs){
		utils::MutexGuard guard(base_nodes_lock_);
		BuildBaseLeafes(base_leafs);
		do{
			std::vector<MerkleNodePointer> new_nodes;
			MakeBinary(base_nodes_.end()[-1]); //Incoming tail element is a list of nodes

			for (size_t i = 0; i < base_nodes_.end()[-1].size(); i += 2){
				MerkleNodePointer new_parent = std::make_shared<MerkleNode>();
				//Set the parent node.Pass in the last element, ie the i and i + 1 of a node list.
				base_nodes_.end()[-1][i]->SetParent(new_parent);
				base_nodes_.end()[-1][i + 1]->SetParent(new_parent);

				// set the hash value of the parent node by the hash value of the two child nodes
				new_parent->SetHash(base_nodes_.end()[-1][i]->GetHash() + base_nodes_.end()[-1][i + 1]->GetHash());
				// set the left and right child nodes of the parent node to these two
				new_parent->SetChildren(base_nodes_.end()[-1][i], base_nodes_.end()[-1][i + 1]);
				// push new parent into new nodes
				new_nodes.push_back(new_parent);

				//cout << "Hash togther: " << base_nodes_.end()[-1][i]->GetHash() << \
																	//				" and " << base_nodes_.end()[-1][i + 1]->GetHash() << " attached: " << \
																	//				&new_parent << endl;
			}
			//Push a new round of parent node new_n odes into base
			base_nodes_.push_back(new_nodes);

			//cout << "Hashed level with: " << base_nodes_.end()[-1].size() << '\n';
		} while (base_nodes_.end()[-1].size() > 1); // so that each round gets a new layer of parent nodes, until the root node to exit the loop

		merkle_root_ = base_nodes_.end()[-1][0]->GetHash(); // the hash value of the root node

		/*cout << "Merkle Root is : " << merkle_root_ << endl << endl;*/
	}

	void MerkleTree::IterateUp(const int64_t &element){
		int64_t length = this->base_nodes_[0].size();
		if (element<0 || element>length - 1){
			return;
		}

		MerkleNodePointer el_node = this->base_nodes_[0][element];
		do {
			LOG_INFO("Current Hash:%s", el_node->GetHash());
		} while ((el_node = el_node->GetParent()) != NULL);
	}

	string MerkleTree::GetMerkleRoot(){
		return merkle_root_;
	}

	// create a list of leaf nodes
	void MerkleTree::BuildBaseLeafes(const vector<string> &base_leafs){
		std::vector<MerkleNodePointer> new_nodes;
		// creates a node for each string and sets the hash value through this string
		for (auto leaf : base_leafs){
			MerkleNodePointer new_node = std::make_shared<MerkleNode>();
			new_node->SetHash(leaf);
			new_nodes.push_back(new_node);
		}
		base_nodes_.push_back(new_nodes);
	}

	bool MerkleTree::VerifyMerkelLeaf(const std::string &leaf_hash){
		MerkleNodePointer el_node = nullptr;
		return VerifyMerkelLeaf(leaf_hash, el_node);
	}

	bool MerkleTree::VerifyMerkelLeaf(const std::string &leaf_hash, MerkleNodePointer &node){
		MerkleNodePointer el_node = nullptr;
		string act_hash = leaf_hash; // the hash value of the leaf node to be verified
		utils::MutexGuard guard(base_nodes_lock_);
		// if base[0] that is, the hash value of a node in the leaf node is equal to it
		for (int64_t i = 0; i < base_nodes_[0].size(); i++){
			if (base_nodes_[0][i]->GetHash() == leaf_hash){
				node = base_nodes_[0][i]; // points to the node
				el_node = node;
			}
		}

		if (el_node == nullptr){
			return false;
		}

		LOG_INFO("Hash verify: %s", act_hash.c_str());

		// verify whether merkle tree has changed
		do{
			// the hash of the parent node is the hash string of the left child + the hash string of the right child
			// if the left node of the parent of el_node is el_node
			// gets the parent node of the node
			MerkleNodePointer parent = el_node->GetParent();
			int64_t flag = parent->GetChildrenLeft() == el_node ? 0 : 1;
			MerkleNodePointer sibling = parent->GetChildrenLeft() == el_node ? parent->GetChildrenRight() : parent->GetChildrenLeft();

			if (flag == 0){
				// is the hash string for the left child + the hash string for the right child
				act_hash = HashMerkleBranches(act_hash, sibling->GetHash());
			}
			else{
				act_hash = HashMerkleBranches(sibling->GetHash(), act_hash);
			}

			LOG_INFO("Hash verify: %s", act_hash.c_str());
			el_node = el_node->GetParent();
		} while ((el_node->GetParent()) != nullptr); // to the root node

		return act_hash == merkle_root_ ? true : false;
	}

	// print the hash value of each node
	void MerkleTree::PrintTreeLevel(const std::vector<MerkleNodePointer> &node_level){
		for (MerkleNodePointer el : node_level){
			LOG_INFO("Node hash is:%s", el->GetHash());
		}
	}

	string MerkleTree::HashMerkleBranches(const std::string &left, const std::string &right){
		return utils::String::BinToHexString(HashWrapper::Crypto(left + right)).c_str();
	}

	void MerkleTree::BuildAuditTrail(vector<protocol::MerkelProofHash> &audit_trail, const MerkleNodePointer &parent, const MerkleNodePointer &child){
		if (parent != nullptr){
			//Contract(() = > child.Parent == parent, "Parent of child is not expected parent.");
			auto next_child = parent->GetChildrenLeft() == child ? parent->GetChildrenRight() : parent->GetChildrenLeft();
			auto direction = parent->GetChildrenLeft() == child ? protocol::MERKEL_BRANCH_TYPE::LEFT : protocol::MERKEL_BRANCH_TYPE::RIGHT;

			// For the last leaf, the right node may not exist.  In that case, we ignore it because it's
			// the hash we are given to verify.
			if (next_child != nullptr){
				protocol::MerkelProofHash proof_hash;
				proof_hash.set_hash(next_child->GetHash());
				proof_hash.set_direction(direction);
				audit_trail.push_back(proof_hash);
			}

			BuildAuditTrail(audit_trail, child->GetParent()->GetParent(), child->GetParent());
		}
	}

	void MerkleTree::AuditProof(const std::string &leaf_hash, std::vector<protocol::MerkelProofHash> &audit_trail){
		MerkleNodePointer leaf_node = nullptr;

		//Verify MerkelLeaf and build audit trail
		if (!VerifyMerkelLeaf(leaf_hash, leaf_node)){
			return;
		}

		if (leaf_node->GetParent() == nullptr){
			return;
		}

		auto parent = leaf_node->GetParent();
		BuildAuditTrail(audit_trail, parent, leaf_node);

	}

	bool MerkleTree::VerifyAudit(const std::string &root_hash, const std::string& leaf_hash, std::vector<protocol::MerkelProofHash> &audit_trail){
		if (audit_trail.size() < 0){
			return false;
		}
		std::string test_hash = leaf_hash;

		for (auto iter = audit_trail.begin(); iter != audit_trail.end(); iter++){
			test_hash = iter->direction() == protocol::MERKEL_BRANCH_TYPE::LEFT ?
				HashMerkleBranches(test_hash, iter->hash()) : HashMerkleBranches(iter->hash(), test_hash);
		}
		return root_hash == test_hash;
	}

	void MerkleTree::TestMerkleTree(){
		string check_str = "10";
		vector<string> v;
		v.push_back("0");
		v.push_back("1");
		v.push_back("2");
		v.push_back("3");
		v.push_back("4");
		v.push_back("5");
		v.push_back("6");
		v.push_back("7");
		v.push_back("8");
		v.push_back("9");
		v.push_back("10");
		v.push_back("11");
		v.push_back("12");
		v.push_back("13");
		v.push_back("14");
		v.push_back("15");
		check_str = utils::String::BinToHexString(HashWrapper::Crypto(check_str)).c_str();

		BuildTree(v);
		if (VerifyMerkelLeaf(check_str)){
			cout << "All clear\n";
		}else{
			cout << "something is wrong\n";
		}

		std::vector<protocol::MerkelProofHash> audit_trail;
		AuditProof(check_str, audit_trail);
		VerifyAudit(merkle_root_, check_str, audit_trail);
	}

}