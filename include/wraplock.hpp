// comment out the following lines to remove clear and test actions for production deployment
#define INCLUDE_CLEAR_ACTION
#define INCLUDE_TEST_ACTIONS

#pragma once

#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>

#include <string>

namespace eosiosystem {
   class system_contract;
}

namespace eosio {

   using std::string;

   class [[eosio::contract("wraplock")]] token : public contract {
      private:

        struct st_create {
          name          issuer;
          asset         maximum_supply;
        };

         struct [[eosio::table]] global {
            checksum256   chain_id;
            name          bridge_contract;
            name          native_token_contract;
            symbol        native_token_symbol;
            checksum256   paired_chain_id;
            name          paired_liquid_wraptoken_contract;
            name          paired_staked_wraptoken_contract;
         } globalrow;

         struct [[eosio::table]] reserve {
            asset       locked_balance;

            uint64_t primary_key() const { return 0; }
         };

         struct [[eosio::table]] account {
            name        owner;
            asset       liquid_balance;

            asset       staked_balance;
            time_point  stake_weighted_days_last_updated;
            uint64_t    stake_weighted_days_owed;

            asset       unstaking_balance;

            uint64_t primary_key() const { return owner.value; }
         };

         // represents the queue for unstaking events to be included in aggregate sellrex
         struct [[eosio::table]] unstaking {
            name        owner;
            asset       quantity;
            time_point  started;

            uint64_t primary_key() const { return owner.value; }
            uint64_t by_started() const { return started.sec_since_epoch(); }
         };


         void sub_liquid_balance( const name& owner, const asset& value );
         void add_liquid_balance( const name& owner, const asset& value );

         void sub_locked_balance( const asset& value );
         void add_locked_balance( const asset& value );

         void sub_staked_balance( const name& owner, const asset& value );
         void add_staked_balance( const name& owner, const asset& value );

         void sub_unstaking_balance( const name& owner, const asset& value );
         void add_unstaking_balance( const name& owner, const asset& value );

         void _unlock( const name& beneficiary, const asset& quantity );
         void _unstake( const name& caller, const name& beneficiary, const asset& quantity );

         asset get_matured_rex();
         uint64_t calculated_owed_stake_weighted_days(const asset& staked_balance, const time_point& stake_weighted_days_last_updated);
      public:
         using contract::contract;

         struct [[eosio::table]] validproof {

           uint64_t                        id;
           action                          action;
           checksum256                     chain_id;
           checksum256                     receipt_digest;
           name                            prover;

           uint64_t primary_key()const { return id; }
           checksum256 by_digest()const { return receipt_digest; }

           EOSLIB_SERIALIZE( validproof, (id)(action)(chain_id)(receipt_digest)(prover))

         };

         struct [[eosio::table]] processed {

           uint64_t                        id;
           checksum256                     receipt_digest;

           uint64_t primary_key()const { return id; }
           checksum256 by_digest()const { return receipt_digest; }

           EOSLIB_SERIALIZE( processed, (id)(receipt_digest))

         };

         struct [[eosio::table]] xfer {
           name             owner;
           extended_asset   quantity;
           name             beneficiary;
           bool             staked;
         };

         // for checking for sufficient matured tokens before sellrex
         struct [[eosio::table]] rex_balance {
            uint8_t version = 0;
            name    owner;
            asset   vote_stake;
            asset   rex_balance;
            int64_t matured_rex = 0;
            std::deque<std::pair<time_point_sec, int64_t>> rex_maturities; /// REX daily maturity buckets

            uint64_t primary_key()const { return owner.value; }
         };

         struct [[eosio::table]] rex_pool {
            uint8_t    version = 0;
            asset      total_lent;
            asset      total_unlent;
            asset      total_rent;
            asset      total_lendable;
            asset      total_rex;
            asset      namebid_proceeds;
            uint64_t   loan_num = 0;

            uint64_t primary_key()const { return 0; }
         };


         /**
          * set contract globals (required before use)
          */
         [[eosio::action]]
         void init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const symbol& native_token_symbol, const checksum256& paired_chain_id, const name& paired_liquid_wraptoken_contract, const name& paired_staked_wraptoken_contract);


         /**
          * called to commit deposited tokens to the interchain transfer process
          * optionally also stakes the tokens to REX
          */
         [[eosio::action]]
         void lock(const name& owner, const asset& quantity, const name& beneficiary, const bool stake);

         /**
          * called to use a proof of retirement of liquid wrapped tokens
          * returns locked tokens to the appropiate liquid balance
          */
         [[eosio::action]]
         void unlock(const name& caller, const checksum256 action_receipt_digest);

         /**
          * called to use a proof of retirement of staked wrapped tokens
          * if sufficient matured_rex is available, returns staked tokens to the appropiate liquid balance
          * if insufficient, moves staked tokens to the unstaking balance and adds an unstaking record to the queue
          */
         [[eosio::action]]
         void unstake(const name& caller, const checksum256 action_receipt_digest);

         /**
          * transfers liquid tokens to the owners account
          */
         [[eosio::action]]
         void withdraw(const name& owner, const asset& quantity);
      

         [[eosio::action]]
         void open( const name& owner, const name& ram_payer );


         [[eosio::action]]
         void close( const name& owner );

         [[eosio::action]]
         void emitxfer(const token::xfer& xfer);

         /**
          * attempts to fulfill unstaking requests from the queue in fifo order
          * moves unstaking to liquid balances for all requests fulfilled
          * stops after count requests, or if there is insufficient matured_rex available for the next request
          */
         [[eosio::action]]
         void processqueue( const uint64_t count );

         [[eosio::action]]
         void unstaked( const name& owner, const asset& quantity );

         /**
          * testing actions to be removed in production
          */
         #ifdef INCLUDE_TEST_ACTIONS

            [[eosio::action]]
            void tstunlock( const name& caller, const name& beneficiary, const asset& quantity );

            [[eosio::action]]
            void tstunstake( const name& caller, const name& beneficiary, const asset& quantity );

         #endif

         #ifdef INCLUDE_CLEAR_ACTION
            [[eosio::action]]
            void clear(const name extaccount);

         #endif

        [[eosio::on_notify("*::transfer")]] void deposit(name from, name to, asset quantity, string memo);

         typedef eosio::multi_index< "reserves"_n, reserve > reservestable;
         typedef eosio::multi_index< "accounts"_n, account > accountstable;

         typedef eosio::multi_index< "unstaking"_n, unstaking,
            indexed_by<"started"_n, const_mem_fun<unstaking, uint64_t, &unstaking::by_started>>> unstakingtable;

         typedef eosio::multi_index< "proofs"_n, validproof,
            indexed_by<"digest"_n, const_mem_fun<validproof, checksum256, &validproof::by_digest>>> proofstable;
      
         typedef eosio::multi_index< "processed"_n, processed,
            indexed_by<"digest"_n, const_mem_fun<processed, checksum256, &processed::by_digest>>> processedtable;

         typedef eosio::multi_index< "rexbal"_n, rex_balance > rexbaltable;
         typedef eosio::multi_index< "rexpool"_n, rex_pool > rexpooltable;

         using globaltable = eosio::singleton<"global"_n, global>;

         void add_or_assert(const validproof& proof, const name& prover);

         validproof get_proof(const checksum256 action_receipt_digest);

         globaltable global_config;

         reservestable _reservestable;
         accountstable _accountstable;

         unstakingtable _unstakingtable;

        processedtable _processedtable;

        rexbaltable _rexbaltable;
        rexpooltable _rexpooltable;

        token( name receiver, name code, datastream<const char*> ds ) :
        contract(receiver, code, ds),
        global_config(_self, _self.value),
        _reservestable(_self, _self.value),
        _accountstable(_self, _self.value),
        _unstakingtable(_self, _self.value),
        _processedtable(_self, _self.value),
        _rexbaltable("eosio"_n, "eosio"_n.value),
        _rexpooltable("eosio"_n, "eosio"_n.value)
        {
        
        }
        
         using open_action = eosio::action_wrapper<"open"_n, &token::open>;
         using close_action = eosio::action_wrapper<"close"_n, &token::close>;
   };

}

