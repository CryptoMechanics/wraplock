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

// return amount of rex immediately available
asset token::get_matured_rex() {
    const time_point_sec now = current_time_point();
    auto& rb = _rexbaltable.get( _self.value, "no rex balance object found" );
    int64_t matured_rex = rb.matured_rex;
    for (auto m : rb.rex_maturities) {
        if (m.first <= now) {
            matured_rex += m.second;
        }
    }
    return asset(matured_rex, symbol("REX", 4));
}

uint64_t token::calculated_owed_stake_weighted_days(const asset& staked_balance, const time_point& stake_weighted_days_last_updated) {
    uint32_t seconds_since_last_update = current_time_point().sec_since_epoch() - stake_weighted_days_last_updated.sec_since_epoch();
    uint32_t full_days_since_last_update = seconds_since_last_update / 86400;
    uint64_t owed = full_days_since_last_update * (staked_balance.amount / 10000);
    return owed;
}

void token::init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const symbol& native_token_symbol, const checksum256& paired_chain_id, const name& paired_liquid_wraptoken_contract, const name& paired_staked_wraptoken_contract)
{
    require_auth( _self );

    auto global = global_config.get_or_create(_self, globalrow);
    global.chain_id = chain_id;
    global.bridge_contract = bridge_contract;
    global.native_token_contract = native_token_contract;
    global.native_token_symbol = native_token_symbol;
    global.paired_chain_id = paired_chain_id;
    global.paired_liquid_wraptoken_contract = paired_liquid_wraptoken_contract;
    global.paired_staked_wraptoken_contract = paired_staked_wraptoken_contract;
    global_config.set(global, _self);

    _reservestable.emplace( _self, [&]( auto& r ){
        r.locked_balance = asset(0, global.native_token_symbol);
    });
}

//locks a token amount in the reserve for an interchain transfer
void token::lock(const name& owner,  const asset& quantity, const name& beneficiary, const bool stake){

  check(global_config.exists(), "contract must be initialized first");

  require_auth(owner);

  check(quantity.amount > 0, "must lock positive quantity");

  sub_liquid_balance( owner, quantity );

  if (stake) {
    add_staked_balance( owner, quantity );

    // buy rex
    action deposit_act(
      permission_level{_self, "active"_n},
      "eosio"_n, "deposit"_n,
      std::make_tuple( _self, quantity )
    );
    deposit_act.send();

    action buyrex_act(
      permission_level{_self, "active"_n},
      "eosio"_n, "buyrex"_n,
      std::make_tuple( _self, quantity )
    );
    buyrex_act.send();


  } else {
    add_locked_balance( quantity );
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

    _unlock( redeem_act.beneficiary, redeem_act.quantity.quantity );

}

void token::_unlock( const name& beneficiary, const asset& quantity ) {
    sub_locked_balance( quantity );
    add_liquid_balance( beneficiary, quantity );
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

    _unstake( caller, redeem_act.beneficiary, redeem_act.quantity.quantity );

}

void token::_unstake( const name& caller, const name& beneficiary, const asset& quantity ) {

    asset eos_quantity = quantity;

    sub_staked_balance( beneficiary, eos_quantity );

    // todo - check no overflow issue here (in changing EOS to REX)

    // Calculate REX required to return at least the requested quantity of EOS
    // There may be more EOS returned than needed, in which case the excess will remain in rex system
    // It should never be less, assuming the EOS/REX rate may only increase
    // todo - add action to allow any extra EOS to be reinvested in rex by a management script?
    const auto& rex_pool = _rexpooltable.get( 0, "no rex pool object found" );
    const uint32_t approx_rex_rate = rex_pool.total_rex.amount / rex_pool.total_lendable.amount;
    asset rex_quantity = asset(eos_quantity.amount * approx_rex_rate, symbol("REX", 4));
    print("approx_rex_rate: ", approx_rex_rate, "\n");
    print("rex_quantity: ", rex_quantity, "\n");

    // check rexbal to see whether there's enough matured_rex to return tokens
    asset matured_rex = get_matured_rex();
    print("matured_rex: ", matured_rex, "\n");

    bool empty_unstaking_queue = _unstakingtable.begin() == _unstakingtable.end();

    if (empty_unstaking_queue && (matured_rex >= rex_quantity)) {

        // sell rex
        action sellrex_act(
          permission_level{_self, "active"_n},
          "eosio"_n, "sellrex"_n,
          std::make_tuple( _self, rex_quantity )
        );
        sellrex_act.send();

        action withdraw_act(
          permission_level{_self, "active"_n},
          "eosio"_n, "withdraw"_n,
          std::make_tuple( _self, eos_quantity )
        );
        withdraw_act.send();

        add_liquid_balance( beneficiary, eos_quantity );

        action unstaked_act(
            permission_level{_self, "active"_n},
            _self, "unstaked"_n,
            std::make_tuple(beneficiary, eos_quantity)
        );
        unstaked_act.send();

    } else {

        // add this to queue of unstaking events or replace existing if request already present
        // if unstaking more, the single unstaking event may move down the queue
        auto unstaking_itr = _unstakingtable.find(beneficiary.value);
        if (unstaking_itr == _unstakingtable.end()){
            _unstakingtable.emplace( caller, [&]( auto& u ){
                u.owner = beneficiary;
                u.quantity = eos_quantity;
                u.started = current_time_point();
            });
        } else {
            _unstakingtable.modify( unstaking_itr, same_payer, [&]( auto& u ) {
                u.quantity += eos_quantity;
                u.started = current_time_point();
            });
        }

        add_unstaking_balance( beneficiary, eos_quantity );
    }

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

void token::sub_locked_balance( const asset& value ){
    const auto& account = _reservestable.get( 0, "no balance object found" );

    check( account.locked_balance.amount >= value.amount, "overdrawn locked balance" );
    _reservestable.modify( account, same_payer, [&]( auto& a ) {
        a.locked_balance -= value;
    });
}

void token::add_locked_balance( const asset& value ){
    const auto& account = _reservestable.get( 0, "no balance object found" );

    _reservestable.modify( account, same_payer, [&]( auto& a ) {
        a.locked_balance += value;
    });
}

void token::sub_staked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.staked_balance.amount >= value.amount, "overdrawn staked balance" );

    uint64_t owed = calculated_owed_stake_weighted_days(account.staked_balance, account.stake_weighted_days_last_updated);
    print("calculated_owed_stake_weighted_days: ", owed, "\n");

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.staked_balance -= value;
        a.stake_weighted_days_last_updated = current_time_point();
        a.stake_weighted_days_owed += owed;
    });
}

void token::add_staked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    uint64_t owed = calculated_owed_stake_weighted_days(account.staked_balance, account.stake_weighted_days_last_updated);
    print("calculated_owed_stake_weighted_days: ", owed, "\n");

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.staked_balance += value;
        a.stake_weighted_days_last_updated = current_time_point();
        a.stake_weighted_days_owed += owed;
    });
}

void token::sub_unstaking_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.unstaking_balance.amount >= value.amount, "overdrawn unstaking balance" );
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

void token::open( const name& owner, const name& ram_payer )
{
   check(global_config.exists(), "contract must be initialized first");

   require_auth( ram_payer );

   check( is_account( owner ), "owner account does not exist" );

   auto global = global_config.get();

    _accountstable.emplace( ram_payer, [&]( auto& a ){
        a.owner = owner;
        a.liquid_balance = asset(0, global.native_token_symbol);

        a.staked_balance = asset(0, global.native_token_symbol);
        a.stake_weighted_days_last_updated = current_time_point();
        a.stake_weighted_days_owed = 0;

        a.unstaking_balance = asset(0, global.native_token_symbol);
    });

}

void token::close( const name& owner )
{
   check(global_config.exists(), "contract must be initialized first");

   require_auth( owner );

   auto it = _accountstable.find( owner.value );
   check( it != _accountstable.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( it->liquid_balance.amount == 0, "Cannot close because the liquid balance is not zero." );
   check( it->staked_balance.amount == 0, "Cannot close because the staked balance is not zero." );
   check( it->unstaking_balance.amount == 0, "Cannot close because the unstaking balance is not zero." );
   check( it->stake_weighted_days_owed == 0, "Cannot close because the stake_weighted_days balance is not zero." );
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

        // todo - check whether anything needs doing when rex rewards are returned

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

void token::processqueue( const uint64_t count )
{
    // anyone can call this

    const auto& rex_balance = _rexbaltable.get( _self.value, "no rex balance object found" );
    const asset matured_rex = get_matured_rex();
    print("matured_rex: ", matured_rex, "\n");

    const auto& rex_pool = _rexpooltable.get( 0, "no rex pool object found" );
    const uint32_t approx_rex_rate = rex_pool.total_rex.amount / rex_pool.total_lendable.amount;
    print("approx_rex_rate: ", approx_rex_rate, "\n");

    auto _unstakingtable_by_start = _unstakingtable.get_index<"started"_n>();

    asset rex_to_sell = asset(0, symbol("REX", 4));
    for (uint64_t i = 0; i < count; i++) {
        auto itr = _unstakingtable_by_start.begin();
        if ( itr != _unstakingtable_by_start.end() ) {
            print("owner: ", itr->owner, "\n");
            asset eos_quantity = itr->quantity;
            print("eos_quantity: ", eos_quantity, "\n");
            asset rex_quantity = asset(eos_quantity.amount * approx_rex_rate, symbol("REX", 4));
            print("rex_quantity: ", rex_quantity, "\n");

            if (matured_rex - rex_to_sell >= rex_quantity) {
                sub_unstaking_balance(itr->owner, eos_quantity);
                add_liquid_balance(itr->owner, eos_quantity);

                action unstaked_act(
                    permission_level{_self, "active"_n},
                    _self, "unstaked"_n,
                    std::make_tuple(itr->owner, eos_quantity)
                );
                unstaked_act.send();

                rex_to_sell += rex_quantity;
                _unstakingtable_by_start.erase(itr);
                print("Fulfilled!\n\n");
            } else {
                print("Not Fulfilled!\n");
                break; // stop at first request that can't be fulfilled
            }
        }
    }

    if (rex_to_sell.amount > 0) {

        // sell rex
        asset eos_to_withdraw = asset(rex_to_sell.amount / approx_rex_rate, symbol("EOS", 4));

        action sellrex_act(
          permission_level{_self, "active"_n},
          "eosio"_n, "sellrex"_n,
          std::make_tuple( _self, rex_to_sell )
        );
        sellrex_act.send();

        action withdraw_act(
          permission_level{_self, "active"_n},
          "eosio"_n, "withdraw"_n,
          std::make_tuple( _self, eos_to_withdraw )
        );
        withdraw_act.send();

    }
}

void token::unstaked( const name& owner, const asset& quantity ) {
    check(global_config.exists(), "contract must be initialized first");
    require_auth(_self);
}

#ifdef INCLUDE_TEST_ACTIONS

    // test action to unlock without proof
    void token::tstunlock( const name& caller, const name& beneficiary, const asset& quantity ) {
        check(global_config.exists(), "contract must be initialized first");
        require_auth( caller );
        _unlock( beneficiary, quantity );
    }

    // test action to unstake without proof
    void token::tstunstake( const name& caller, const name& beneficiary, const asset& quantity ) {
        check(global_config.exists(), "contract must be initialized first");
        require_auth( caller );
        _unstake( caller, beneficiary, quantity );
    }

#endif

#ifdef INCLUDE_CLEAR_ACTION

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

      while (_unstakingtable.begin() != _unstakingtable.end()) {
        auto itr = _unstakingtable.end();
        itr--;
        _unstakingtable.erase(itr);
      }

    }

#endif

} /// namespace eosio

