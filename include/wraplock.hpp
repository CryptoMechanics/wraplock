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
            checksum256   paired_chain_id;
            name          paired_liquid_wraptoken_contract;
            name          paired_staked_wraptoken_contract;
         } globalrow;

         struct [[eosio::table]] account {
            name     owner;
            asset    liquid_balance;
            asset    locked_balance;
            asset    staked_balance;
            asset    unstaking_balance;

            uint64_t primary_key()const { return owner.value; }
         };


         void sub_liquid_balance( const name& owner, const asset& value );
         void add_liquid_balance( const name& owner, const asset& value );

         void sub_locked_balance( const name& owner, const asset& value );
         void add_locked_balance( const name& owner, const asset& value );

         void sub_staked_balance( const name& owner, const asset& value );
         void add_staked_balance( const name& owner, const asset& value );

         void sub_unstaking_balance( const name& owner, const asset& value );
         void add_unstaking_balance( const name& owner, const asset& value );

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


         [[eosio::action]]
         void init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const checksum256& paired_chain_id, const name& paired_liquid_wraptoken_contract, const name& paired_staked_wraptoken_contract);


         [[eosio::action]]
         void lock(const name& owner, const asset& quantity, const name& beneficiary, const bool stake);

         [[eosio::action]]
         void unlock(const name& caller, const checksum256 action_receipt_digest);

         [[eosio::action]]
         void unstake(const name& caller, const checksum256 action_receipt_digest);

         [[eosio::action]]
         void withdraw(const name& owner, const asset& quantity);
      

         [[eosio::action]]
         void open( const name& owner, const symbol& symbol, const name& ram_payer );


         [[eosio::action]]
         void close( const name& owner, const symbol& symbol );

         [[eosio::action]]
         void emitxfer(const token::xfer& xfer);


         [[eosio::action]]
         void clear(const name extaccount);

        [[eosio::on_notify("*::transfer")]] void deposit(name from, name to, asset quantity, string memo);


         typedef eosio::multi_index< "accounts"_n, account > accountstable;

         typedef eosio::multi_index< "proofs"_n, validproof,
            indexed_by<"digest"_n, const_mem_fun<validproof, checksum256, &validproof::by_digest>>> proofstable;
      
         typedef eosio::multi_index< "processed"_n, processed,
            indexed_by<"digest"_n, const_mem_fun<processed, checksum256, &processed::by_digest>>> processedtable;

         using globaltable = eosio::singleton<"global"_n, global>;

         void add_or_assert(const validproof& proof, const name& prover);

         validproof get_proof(const checksum256 action_receipt_digest);

         globaltable global_config;

         accountstable _accountstable;

        processedtable _processedtable;

        token( name receiver, name code, datastream<const char*> ds ) :
        contract(receiver, code, ds),
        global_config(_self, _self.value),
        _accountstable(_self, _self.value),
        _processedtable(_self, _self.value)
        {
        
        }
        
         using open_action = eosio::action_wrapper<"open"_n, &token::open>;
         using close_action = eosio::action_wrapper<"close"_n, &token::close>;
   };

}

