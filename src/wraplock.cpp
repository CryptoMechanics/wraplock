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

// return amount of REX that would be returned for proposed purchase
asset token::get_rex_purchase_quantity( const asset& eos_quantity ) {
    auto itr = _rexpooltable.begin();
    asset rex_quantity(0, symbol("REX", 4));
    check( itr->total_lendable.amount > 0, "lendable REX pool is empty" ); // todo - check what to do in this situation
    const int64_t S0 = itr->total_lendable.amount;
    const int64_t S1 = S0 + eos_quantity.amount;
    const int64_t R0 = itr->total_rex.amount;
    const int64_t R1 = (uint128_t(S1) * R0) / S0;
    rex_quantity.amount = R1 - R0;
    return rex_quantity;
}

// return amount of EOS that would be returned for proposed sale
asset token::get_eos_sale_quantity( const asset& rex_quantity ) {
    auto rexitr = _rexpooltable.begin();
    const int64_t S0 = rexitr->total_lendable.amount;
    // print("S0/total_lendable.amount: ", rexitr->total_lendable.amount, "\n");
    const int64_t R0 = rexitr->total_rex.amount;
    // print("R0/total_rex.amount: ", rexitr->total_rex.amount, "\n");
    const int64_t p  = (uint128_t(rex_quantity.amount) * S0) / R0;
    asset proceeds( p, symbol("EOS", 4) );
    return proceeds;
}

// return amount of REX that would be required to return EOS quantity
asset token::get_rex_sale_quantity( const asset& eos_quantity ) {
    auto rexitr = _rexpooltable.begin();
    const int64_t S0 = rexitr->total_lendable.amount;
    const int64_t R0 = rexitr->total_rex.amount;
    asset rex_quantity((uint128_t(eos_quantity.amount) * R0) / S0, symbol("REX", 4));
    return rex_quantity;
}

void token::init(const checksum256& chain_id, const name& bridge_contract, const name& native_token_contract, const symbol& native_token_symbol, const checksum256& paired_chain_id, const name& paired_wraptoken_contract, const symbol& paired_wraptoken_symbol, const name& voting_proxy_contract, const name& reward_target_contract, const uint32_t min_unstaking_period_seconds)
{
    require_auth( _self );

    auto global = global_config.get_or_create(_self, globalrow);
    global.chain_id = chain_id;
    global.bridge_contract = bridge_contract;
    global.native_token_contract = native_token_contract;
    global.native_token_symbol = native_token_symbol;
    global.paired_chain_id = paired_chain_id;
    global.paired_wraptoken_contract = paired_wraptoken_contract;
    global.paired_wraptoken_symbol = paired_wraptoken_symbol;
    global.voting_proxy_contract = voting_proxy_contract;
    global.reward_target_contract = reward_target_contract;
    global.min_unstaking_period_seconds = min_unstaking_period_seconds;
    global_config.set(global, _self);

    // add zero balances to reserves if not present
    if (_reservestable.find( 0 ) == _reservestable.end()) {
        _reservestable.emplace( _self, [&]( auto& r ){
            r.staked_balance = asset(0, global.native_token_symbol);
        });
    }
}

void token::stake(const name& owner,  const asset& quantity, const name& beneficiary) {

  check(global_config.exists(), "contract must be initialized first");

  require_auth(owner);

  check(quantity.amount > 0, "must stake positive quantity");

  auto global = global_config.get();

  sub_liquid_balance( owner, quantity );

  asset xquantity = asset(quantity.amount, global.paired_wraptoken_symbol);
  add_staked_balance( quantity );

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

  token::xfer x = {
    .owner = owner,
    .quantity = extended_asset(xquantity, global.native_token_contract),
    .beneficiary = beneficiary
  };

  action act(
    permission_level{_self, "active"_n},
    _self, "emitxfer"_n,
    std::make_tuple(x)
  );
  act.send();

}

void token::unstake(const name& caller, const checksum256 action_receipt_digest){

    check(global_config.exists(), "contract must be initialized first");

    require_auth( caller );

    token::validproof proof = get_proof(action_receipt_digest);

    token::xfer redeem_act = unpack<token::xfer>(proof.action.data);

    auto global = global_config.get();
    check(proof.chain_id == global.paired_chain_id, "proof chain does not match paired chain");
    check(proof.action.account == global.paired_wraptoken_contract, "proof account does not match paired account");

    add_or_assert(proof, caller);

    check(proof.action.name == "emitxfer"_n, "must provide proof of token retiring before issuing");

    check(redeem_act.quantity.quantity.symbol == global.paired_wraptoken_symbol, "incorrect symbol in transfer");
    asset quantity = asset(redeem_act.quantity.quantity.amount, global.native_token_symbol);

    _unstake( caller, redeem_act.beneficiary, quantity );

}

void token::_unstake( const name& caller, const name& beneficiary, const asset& quantity ) {

    asset eos_quantity = quantity;

    sub_staked_balance( eos_quantity );

    asset rex_quantity = get_rex_sale_quantity(eos_quantity);

    // check rexbal to see whether there's enough matured_rex to return tokens
    asset matured_rex = get_matured_rex();
    print("matured_rex: ", matured_rex, "\n");

    bool empty_unstaking_queue = _unstakingtable.begin() == _unstakingtable.end();

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

void token::sub_staked_balance( const asset& value ){
    const auto& reserve = _reservestable.get( 0, "no balance object found" );

    _reservestable.modify( reserve, same_payer, [&]( auto& a ) {
        a.staked_balance -= value;
    });
}

void token::add_staked_balance( const asset& value ){
    const auto& reserve = _reservestable.get( 0, "no balance object found" );

    _reservestable.modify( reserve, same_payer, [&]( auto& a ) {
        a.staked_balance += value;
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

    auto itr = _accountstable.find( owner.value );
    if( itr == _accountstable.end() ) {
        _accountstable.emplace( ram_payer, [&]( auto& a ){
            a.owner = owner;
            a.liquid_balance = asset(0, global.native_token_symbol);

            a.unstaking_balance = asset(0, global.native_token_symbol);
        });
    }
}

void token::close( const name& owner )
{
   check(global_config.exists(), "contract must be initialized first");

   require_auth( owner );

   auto it = _accountstable.find( owner.value );
   check( it != _accountstable.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( it->liquid_balance.amount == 0, "Cannot close because the liquid balance is not zero." );
   check( it->unstaking_balance.amount == 0, "Cannot close because the unstaking balance is not zero." );
   _accountstable.erase( it );

}

void token::deposit(name from, name to, asset quantity, string memo)
{ 
    check(global_config.exists(), "contract must be initialized first");

    print("transfer ", name{from}, " ",  name{to}, " ", quantity, "\n");
    print("sender: ", get_sender(), "\n");
    
    auto global = global_config.get();
    check(get_sender() == global.native_token_contract, "transfer not permitted from unauthorised token contract");

    //if incoming transfer
    if (from == "eosio.stake"_n) return ; //ignore unstaking transfers
    else if (from == global.voting_proxy_contract) {

        // add voting_rewards_received and current staked_balance to history table record
        const auto& reserve = _reservestable.get( 0, "no reserve balance object found" );
        uint32_t day_start_seconds = (current_time_point().sec_since_epoch() / DAY_SECONDS) * DAY_SECONDS;

        check(_historytable.find(day_start_seconds) == _historytable.end(), "voting rewards already received for today");

        asset original_staked_balance = reserve.staked_balance;

        // deposit and buy rex with voting rewards
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

        // calculate excess rex which can be used for rewards
        asset eos_value_of_total_rex = get_eos_sale_quantity(get_total_rex());
        print("eos_value_of_total_rex:", eos_value_of_total_rex, "\n");

        asset eos_owed_for_rewards = eos_value_of_total_rex - reserve.staked_balance;
        if (eos_owed_for_rewards.amount < 0) eos_owed_for_rewards.amount = 0; // prevent -0.0001 values
        print("eos_owed_for_rewards:", eos_owed_for_rewards, "\n");

        // account for and allocate the amount from vote rewards to rex (adding to reserve.staked_balance)
        add_staked_balance( quantity );

        // if positive, account for and allocate the excess eos amount in rex to the reserve.staked_balance
        if (eos_owed_for_rewards.amount > 0) {
            add_staked_balance( eos_owed_for_rewards );
        }

        // make xfer to send total rewards across bridge to rewarded account
        asset xquantity = asset(quantity.amount + eos_owed_for_rewards.amount, global.paired_wraptoken_symbol);
        token::xfer x = {
            .owner = _self,
            .quantity = extended_asset(xquantity, global.native_token_contract),
            .beneficiary = global.reward_target_contract
        };

        action act(
            permission_level{_self, "active"_n},
            _self, "emitxfer"_n,
            std::make_tuple(x)
        );
        act.send();

        // record instantanious stake and the rewards sent across the bridge
        _historytable.emplace( _self, [&]( auto& h ){
            h.day = time_point(seconds(day_start_seconds));
            h.staked_balance = original_staked_balance;
            h.voting_rewards_received = quantity;
            h.rex_rewards_accrued = eos_owed_for_rewards;
        });

    }
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

    check(global_config.exists(), "contract must be initialized first");
    auto global = global_config.get();

    // anyone can call this

    const auto& rex_balance = _rexbaltable.get( _self.value, "no rex balance object found" );
    const asset matured_rex = get_matured_rex();
    print("matured_rex: ", matured_rex, "\n\n");

    auto _unstakingtable_by_start = _unstakingtable.get_index<"started"_n>();

    asset rex_to_sell = asset(0, symbol("REX", 4));
    for (uint64_t i = 0; i < count; i++) {
        auto itr = _unstakingtable_by_start.begin();
        if ( itr != _unstakingtable_by_start.end() ) {
            if (current_time_point().sec_since_epoch() - itr->started.sec_since_epoch() > (global.min_unstaking_period_seconds)) {
                print("owner: ", itr->owner, "\n");
                asset eos_quantity = itr->quantity;
                print("eos_quantity to unstake: ", eos_quantity, "\n");
                asset rex_quantity = get_rex_sale_quantity(eos_quantity);
                print("rex_quantity to sell: ", rex_quantity, "\n");

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
            } else {
                break;
            }
        }
    }

    if (rex_to_sell.amount > 0) {

        // sell rex
        asset eos_to_withdraw = get_eos_sale_quantity(rex_to_sell);

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

    // return amount of rex immediately available
    asset token::get_total_rex() {
        auto& rb = _rexbaltable.get( _self.value, "no rex balance object found" );
        int64_t matured_rex = rb.matured_rex;
        for (auto m : rb.rex_maturities) {
            matured_rex += m.second;
        }
        return asset(matured_rex, symbol("REX", 4));
    }

    // test action to unstake without proof
    void token::tstunstake( const name& caller, const name& beneficiary, const asset& quantity ) {
        check(global_config.exists(), "contract must be initialized first");
        require_auth( caller );
        _unstake( caller, beneficiary, quantity );
    }

    void token::debug( const bool accrue_rewards ) {

        const auto& rex_balance = _rexbaltable.get( _self.value, "no rex balance object found" );

        print("Simulating sales from unstaking queue, pretending all rex has matured...\n\n");
        asset total_rex_balance = get_total_rex();
        auto _unstakingtable_by_start = _unstakingtable.get_index<"started"_n>();
        auto itr = _unstakingtable_by_start.begin();
        while ( itr != _unstakingtable_by_start.end() ) {
            print("matured_rex_balance: ", total_rex_balance, "\n");
            print("owner: ", itr->owner, "\n");
            asset eos_quantity = itr->quantity;
            print("eos_quantity to unstake: ", eos_quantity, "\n");
            asset rex_quantity = get_rex_sale_quantity(eos_quantity);
            print("rex_quantity already sold: ", rex_quantity, "\n");
            print("Fulfilled!\n\n");
            itr++;
        }

    }

#endif

#ifdef INCLUDE_CLEAR_ACTION

    void token::clear()
    {
      require_auth( _self );

      check(global_config.exists(), "contract must be initialized first");

      // if (global_config.exists()) global_config.remove();

      while (_reservestable.begin() != _reservestable.end()) {
        auto itr = _reservestable.end();
        itr--;
        _reservestable.erase(itr);
      }

      while (_accountstable.begin() != _accountstable.end()) {
        auto itr = _accountstable.end();
        itr--;
        _accountstable.erase(itr);
      }

      while (_historytable.begin() != _historytable.end()) {
        auto itr = _historytable.end();
        itr--;
        _historytable.erase(itr);
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

