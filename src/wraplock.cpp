#include <wraplock.hpp>

namespace eosio {


//adds a proof to the list of processed proofs (throws an exception if proof already exists)
void wraplock::add_or_assert(const bridge::actionproof& actionproof, const name& payer){

    auto pid_index = _processedtable.get_index<"digest"_n>();

    std::vector<char> serializedAction = pack(actionproof.action);
    std::vector<char> serializedReceipt = pack(actionproof.receipt);
    checksum256 action_digest = sha256(serializedAction.data(), serializedAction.size());
    checksum256 action_receipt_digest = sha256(serializedReceipt.data(), serializedReceipt.size());

    auto p_itr = pid_index.find(action_receipt_digest);

    check(p_itr == pid_index.end(), "action already proved");

    _processedtable.emplace( payer, [&]( auto& s ) {
        s.id = _processedtable.available_primary_key();
        s.receipt_digest = action_receipt_digest;
    });

}

void wraplock::init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const checksum256& paired_chain_id, const name& paired_wraptoken_contract)
{
    require_auth( _self );

    auto global = global_config.get_or_create(_self, globalrow);
    global.chain_id = chain_id;
    global.bridge_contract = bridge_contract;
    global.native_token_contract = native_token_contract;
    global.paired_chain_id = paired_chain_id;
    global.paired_wraptoken_contract = paired_wraptoken_contract;
    global_config.set(global, _self);

}

//emits an xfer receipt to serve as proof in interchain transfers
void wraplock::emitxfer(const wraplock::xfer& xfer){

 check(global_config.exists(), "contract must be initialized first");
 
 require_auth(_self);

}

void wraplock::sub_reserve( const asset& value ){

   const auto& res = _reservestable.get( value.symbol.code().raw(), "no balance object found" );
   check( res.balance.amount >= value.amount, "overdrawn balance" );

   _reservestable.modify( res, _self, [&]( auto& a ) {
         a.balance -= value;
      });
}

void wraplock::add_reserve(const asset& value){

   auto res = _reservestable.find( value.symbol.code().raw() );
   if( res == _reservestable.end() ) {
      _reservestable.emplace( _self, [&]( auto& a ){
        a.balance = value;
      });
   } else {
      _reservestable.modify( res, _self, [&]( auto& a ) {
        a.balance += value;
      });
   }

}

// called on transfer action to lock tokens and initiate interchain transfer
void wraplock::deposit(name from, name to, asset quantity, string memo)
{ 

    print("transfer ", name{from}, " ",  name{to}, " ", quantity, "\n");
    print("sender: ", get_sender(), "\n");
    
    check(global_config.exists(), "contract must be initialized first");
    auto global = global_config.get();
    check(get_sender() == global.native_token_contract, "transfer not permitted from unauthorised token contract");

    //if incoming transfer
    if (from == "eosio.stake"_n) return ; //ignore unstaking transfers
    else if (to == get_self() && from != get_self()){
      //ignore outbound transfers from this contract, as well as inbound transfers of tokens internal to this contract
      //otherwise, locks the tokens in the reserve and calls emitxfer to be used for issue/cancel proof

      check(memo.size() > 0, "memo must contain valid account name");

      check(quantity.amount > 0, "must lock positive quantity");

      add_reserve( quantity );

      auto global = global_config.get();

      wraplock::xfer x = {
        .owner = from,
        .quantity = extended_asset(quantity, global.native_token_contract),
        .beneficiary = name(memo)
      };

      action act(
        permission_level{_self, "active"_n},
        _self, "emitxfer"_n,
        std::make_tuple(x)
      );
      act.send();

    }

}

void wraplock::_withdraw(const name& prover, const bridge::actionproof actionproof){
    auto global = global_config.get();

    wraplock::xfer redeem_act = unpack<wraplock::xfer>(actionproof.action.data);

    check(actionproof.action.account == global.paired_wraptoken_contract, "proof account does not match paired account");

    add_or_assert(actionproof, prover);

    check(actionproof.action.name == "emitxfer"_n, "must provide proof of token retiring before withdrawing");

    sub_reserve(redeem_act.quantity.quantity);

    action act(
      permission_level{_self, "active"_n},
      redeem_act.quantity.contract, "transfer"_n,
      std::make_tuple(_self, redeem_act.beneficiary, redeem_act.quantity.quantity, ""_n )
    );
    act.send();

}

// withdraw tokens (requires a heavy proof of retiring)
void wraplock::withdrawa(const name& prover, const bridge::heavyproof blockproof, const bridge::actionproof actionproof){
    require_auth(prover);

    check(global_config.exists(), "contract must be initialized first");
    auto global = global_config.get();

    check(blockproof.chain_id == global.paired_chain_id, "proof chain does not match paired chain");

    // check proof against bridge
    // will fail tx if prove is invalid
    action checkproof_act(
      permission_level{_self, "active"_n},
      global.bridge_contract, "checkproofb"_n,
      std::make_tuple(blockproof, actionproof)
    );
    checkproof_act.send();

    _withdraw(prover, actionproof);
}

// withdraw tokens (requires a light proof of retiring)
void wraplock::withdrawb(const name& prover, const bridge::lightproof blockproof, const bridge::actionproof actionproof){
    require_auth(prover);

    check(global_config.exists(), "contract must be initialized first");
    auto global = global_config.get();

    check(blockproof.chain_id == global.paired_chain_id, "proof chain does not match paired chain");

    // check proof against bridge
    // will fail tx if prove is invalid
    action checkproof_act(
      permission_level{_self, "active"_n},
      global.bridge_contract, "checkproofc"_n,
      std::make_tuple(blockproof, actionproof)
    );
    checkproof_act.send();

    _withdraw(prover, actionproof);
}

void wraplock::clear()
{ 
  require_auth( _self );

  check(global_config.exists(), "contract must be initialized first");

  // if (global_config.exists()) global_config.remove();

  while (_reservestable.begin() != _reservestable.end()) {
    auto itr = _reservestable.end();
    itr--;
    _reservestable.erase(itr);
  }

  while (_processedtable.begin() != _processedtable.end()) {
    auto itr = _processedtable.end();
    itr--;
    _processedtable.erase(itr);
  }

}

} /// namespace eosio

