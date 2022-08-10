#pragma once

#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>

#include <string>

#include <bridge.hpp>

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
            name          paired_wraptoken_contract;
         } globalrow;

         struct [[eosio::table]] extaccount {
            asset    balance;

            uint64_t primary_key()const { return balance.symbol.code().raw(); }
         };


         void sub_external_balance( const name& owner, const asset& value );
         void add_external_balance( const name& owner, const asset& value, const name& ram_payer );

         void sub_reserve(const asset& value );
         void add_reserve(const asset& value );

      public:
         using contract::contract;

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
         };


         [[eosio::action]]
         void init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const checksum256& paired_chain_id, const name& paired_wraptoken_contract);


         [[eosio::action]]
         void lock(const name& owner, const asset& quantity, const name& beneficiary);

         void _withdraw(const name& prover, const bridge::actionproof actionproof);

         [[eosio::action]]
         void withdrawa(const name& prover, const bridge::heavyproof blockproof, const bridge::actionproof actionproof);

         [[eosio::action]]
         void withdrawb(const name& prover, const bridge::lightproof blockproof, const bridge::actionproof actionproof);
      

         [[eosio::action]]
         void open( const name& owner, const symbol& symbol, const name& ram_payer );


         [[eosio::action]]
         void close( const name& owner, const symbol& symbol );

         [[eosio::action]]
         void emitxfer(const token::xfer& xfer);


         [[eosio::action]]
         void clear(const name extaccount);

        [[eosio::on_notify("*::transfer")]] void deposit(name from, name to, asset quantity, string memo);


         typedef eosio::multi_index< "extaccounts"_n, extaccount > extaccounts;
         typedef eosio::multi_index< "reserves"_n, extaccount > reserves;
      
         typedef eosio::multi_index< "processed"_n, processed,
            indexed_by<"digest"_n, const_mem_fun<processed, checksum256, &processed::by_digest>>> processedtable;

         using globaltable = eosio::singleton<"global"_n, global>;

         void add_or_assert(const bridge::actionproof& actionproof, const name& payer);

         globaltable global_config;

        processedtable _processedtable;
        reserves _reservestable;

        token( name receiver, name code, datastream<const char*> ds ) :
        contract(receiver, code, ds),
        global_config(_self, _self.value),
        _processedtable(_self, _self.value),
        _reservestable(_self, _self.value)
        {
        
        }
        
   };

}

