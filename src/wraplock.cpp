#include <wraplock.hpp>

namespace eosio {


//fetches proof from the bridge contract
token::validproof token::get_proof(const checksum256 action_receipt_digest){

  auto global = global_config.get();

  proofstable _proofstable(global.bridge_contract, global.bridge_contract.value);
  auto pid_index = _proofstable.get_index<"digest"_n>();
  auto p_itr = pid_index.find(action_receipt_digest);

  check(p_itr != pid_index.end(), "proof not found");

  return *p_itr;

}


//adds a proof to the list of processed proofs (throws an exception if proof already exists)
void token::add_or_assert(const validproof& proof, const name& payer){

    auto pid_index = _processedtable.get_index<"digest"_n>();

    auto p_itr = pid_index.find(proof.receipt_digest);

    check(p_itr == pid_index.end(), "action already proved");

    _processedtable.emplace( payer, [&]( auto& s ) {
        s.id = _processedtable.available_primary_key();
        s.receipt_digest = proof.receipt_digest;
    });

}

void token::init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const checksum256& paired_chain_id, const name& paired_liquid_wraptoken_contract, const name& paired_staked_wraptoken_contract)
{
    require_auth( _self );

    auto global = global_config.get_or_create(_self, globalrow);
    global.chain_id = chain_id;
    global.bridge_contract = bridge_contract;
    global.native_token_contract = native_token_contract;
    global.paired_chain_id = paired_chain_id;
    global.paired_liquid_wraptoken_contract = paired_liquid_wraptoken_contract;
    global_config.set(global, _self);

}

//locks a token amount in the reserve for an interchain transfer
void token::lock(const name& owner,  const asset& quantity, const name& beneficiary, const bool stake){

  check(global_config.exists(), "contract must be initialized first");

  require_auth(owner);

  check(quantity.amount > 0, "must lock positive quantity");

  sub_liquid_balance( owner, quantity );

  if (stake) {
    add_staked_balance( owner, quantity );
  } else {
    add_locked_balance( owner, quantity );
  }

  auto global = global_config.get();

  token::xfer x = {
    .owner = owner,
    .quantity = extended_asset(quantity, global.native_token_contract),
    .beneficiary = beneficiary,
    .staked = stake
  };

  action act(
    permission_level{_self, "active"_n},
    _self, "emitxfer"_n,
    std::make_tuple(x)
  );
  act.send();

}


void token::unlock(const name& caller, const checksum256 action_receipt_digest){

    check(global_config.exists(), "contract must be initialized first");

    require_auth( caller );

    token::validproof proof = get_proof(action_receipt_digest);

    token::xfer redeem_act = unpack<token::xfer>(proof.action.data);

    auto global = global_config.get();
    check(proof.chain_id == global.paired_chain_id, "proof chain does not match paired chain");
    check(proof.action.account == global.paired_liquid_wraptoken_contract, "proof account does not match paired account");

    add_or_assert(proof, caller);

    check(proof.action.name == "emitxfer"_n, "must provide proof of token retiring before issuing");

    sub_locked_balance( redeem_act.beneficiary, redeem_act.quantity.quantity );
    add_liquid_balance( redeem_act.beneficiary, redeem_act.quantity.quantity );

}

void token::unstake(const name& caller, const checksum256 action_receipt_digest){

    check(global_config.exists(), "contract must be initialized first");

    require_auth( caller );

    token::validproof proof = get_proof(action_receipt_digest);

    token::xfer redeem_act = unpack<token::xfer>(proof.action.data);

    auto global = global_config.get();
    check(proof.chain_id == global.paired_chain_id, "proof chain does not match paired chain");
    check(proof.action.account == global.paired_staked_wraptoken_contract, "proof account does not match paired account");

    add_or_assert(proof, caller);

    check(proof.action.name == "emitxfer"_n, "must provide proof of token retiring before issuing");

    sub_staked_balance( redeem_act.beneficiary, redeem_act.quantity.quantity );
    add_unstaking_balance( redeem_act.beneficiary, redeem_act.quantity.quantity );

}

//emits an xfer receipt to serve as proof in interchain transfers
void token::emitxfer(const token::xfer& xfer){

 check(global_config.exists(), "contract must be initialized first");
 
 require_auth(_self);

}

void token::sub_liquid_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.liquid_balance.amount >= value.amount, "overdrawn liquid balance" );
    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.liquid_balance -= value;
    });
}

void token::add_liquid_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.liquid_balance += value;
    });
}

void token::sub_locked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.locked_balance.amount >= value.amount, "overdrawn liquid balance" );
    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.locked_balance -= value;
    });
}

void token::add_locked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.locked_balance += value;
    });
}

void token::sub_staked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.staked_balance.amount >= value.amount, "overdrawn liquid balance" );
    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.staked_balance -= value;
    });
}

void token::add_staked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.staked_balance += value;
    });
}

void token::sub_unstaking_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.unstaking_balance.amount >= value.amount, "overdrawn liquid balance" );
    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.unstaking_balance -= value;
    });
}

void token::add_unstaking_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.unstaking_balance += value;
    });
}

void token::open( const name& owner, const symbol& symbol, const name& ram_payer )
{
   check(global_config.exists(), "contract must be initialized first");

   require_auth( ram_payer );

   check( is_account( owner ), "owner account does not exist" );

   auto global = global_config.get();

    _accountstable.emplace( ram_payer, [&]( auto& a ){
        a.owner = owner;
        a.liquid_balance = asset(0, symbol);
        a.locked_balance = asset(0, symbol);
        a.staked_balance = asset(0, symbol);
        a.unstaking_balance = asset(0, symbol);
    });

}

void token::close( const name& owner, const symbol& symbol )
{
   check(global_config.exists(), "contract must be initialized first");

   require_auth( owner );

   auto it = _accountstable.find( owner.value );
   check( it != _accountstable.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( it->liquid_balance.amount == 0, "Cannot close because the liquid balance is not zero." );
   check( it->locked_balance.amount == 0, "Cannot close because the locked balance is not zero." );
   check( it->staked_balance.amount == 0, "Cannot close because the staked balance is not zero." );
   check( it->unstaking_balance.amount == 0, "Cannot close because the unstaking balance is not zero." );
   _accountstable.erase( it );

}

void token::deposit(name from, name to, asset quantity, string memo)
{ 

    print("transfer ", name{from}, " ",  name{to}, " ", quantity, "\n");
    print("sender: ", get_sender(), "\n");
    
    auto global = global_config.get();
    check(get_sender() == global.native_token_contract, "transfer not permitted from unauthorised token contract");

    //if incoming transfer
    if (from == "eosio.stake"_n) return ; //ignore unstaking transfers
    else if (from == "eosio.rex"_n) {
        // todo - apportion returns to those with staked balances

    }
    else if (to == get_self() && from != get_self()) {
      //ignore outbound transfers from this contract, as well as inbound transfers of tokens internal to this contract
      //otherwise, means it's a deposit of external token from user

      add_liquid_balance(from, quantity);

    }

}

void token::withdraw( const name& owner, const asset& quantity ){

    check(global_config.exists(), "contract must be initialized first");

    require_auth( owner );

    sub_liquid_balance(owner, quantity);
    
    auto global = global_config.get();
    action act(
      permission_level{_self, "active"_n},
      global.native_token_contract, "transfer"_n,
      std::make_tuple(_self, owner, quantity, ""_n )
    );
    act.send();

}

void token::clear(const name extaccount)
{ 
  require_auth( _self );

  check(global_config.exists(), "contract must be initialized first");

  // if (global_config.exists()) global_config.remove();

  while (_accountstable.begin() != _accountstable.end()) {
    auto itr = _accountstable.end();
    itr--;
    _accountstable.erase(itr);
  }

  while (_processedtable.begin() != _processedtable.end()) {
    auto itr = _processedtable.end();
    itr--;
    _processedtable.erase(itr);
  }

/*
proofstable

*/
}

} /// namespace eosio

