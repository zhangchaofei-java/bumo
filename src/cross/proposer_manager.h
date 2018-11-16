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

#ifndef PROPOSER_MANAGER_H_
#define PROPOSER_MANAGER_H_

#include <utils/singleton.h>
#include <utils/thread.h>
#include<cross/message_channel_manager.h>

namespace bumo {
	class Header{
	public:
		int64_t seq_;
		int64_t chanin_id_;
	public:
		bool operator == (const Header &header){
			if ((seq_ == header.seq_) && (chanin_id_ == header.chanin_id_)){
				return true;
			}
			else{
				return false;
			}
		}
	};

	class ProposerManager :public utils::Singleton<ProposerManager>,
		public bumo::IMessageChannelConsumer,
		public utils::Runnable{
		friend class utils::Singleton<bumo::ProposerManager>;
	public:
		ProposerManager();
		~ProposerManager();

		bool Initialize();
		bool Exit();
		void HandleMessageChannelConsumer(const protocol::MessageChannel &message_channel);

	private:
		virtual void Run(utils::Thread *thread) override;
		void HandleChildChainBlock();
		void HandleChildChainNotExistBlock();
		bool HandleSingleChildChainBlock(const protocol::LedgerHeader& ledger_header);
		bool CheckChildBlockExsit(const std::string& hash, int64_t chain_id);
		bool QueryFreshChildBlock(const int64_t chain_id, Header& header);
		bool CommitTransaction(const protocol::LedgerHeader& ledger_header);
		bool CheckNodeIsValidate(const std::string &address, int64_t chain_id);
		void HandleSingleChildChainBlockNotExsit(const Header& header);
		void HandleChildChainBlockNotExsitList(const Header& header);
		void UpdateValidateAddressList(utils::StringList& validate_address, int64_t chain_id);
		bool enabled_;
		utils::Thread *thread_ptr_;
		int64_t last_uptate_validate_address_time_;
		utils::Mutex handle_child_chain_list_lock_;
		utils::Mutex handle_child_chain_list_not_lock_;
		int64_t last_uptate_handle_child_chain_time_;
		int64_t last_uptate_handle_child_chain_not_time_;
		std::list<protocol::LedgerHeader> handle_child_chain_block_list_;
		std::list<Header> handle_child_chain_block_list_not_;
	};

}

#endif
