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

void token::accrue_voting_rewards( const name& owner ) {
    auto global = global_config.get();
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    asset rewards_from_voting_proxy = asset(0, global.native_token_symbol);
    uint32_t day_start = ((account.voting_rewards_last_accrued.sec_since_epoch() / 86400) * 86400) + 86400;
    auto itr_i = _historytable.find(day_start);
    while (itr_i != _historytable.end()) {
        double reward_share = itr_i->received.amount / itr_i->staked.amount;
        rewards_from_voting_proxy += reward_share * account.staked_balance;
        itr_i++;
    }

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.voting_rewards_accrued += rewards_from_voting_proxy;
        a.voting_rewards_last_accrued = current_time_point();
    });
}

void token::clear_accrued_voting_rewards( const name& owner ) {
    auto global = global_config.get();
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.voting_rewards_accrued = asset(0, global.native_token_symbol);
        a.voting_rewards_last_accrued = current_time_point();
    });
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
    add_rex_balance( owner, get_rex_purchase_quantity(quantity) );

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

    asset rex_quantity = get_rex_sale_quantity(eos_quantity);

    // check rexbal to see whether there's enough matured_rex to return tokens
    asset matured_rex = get_matured_rex();
    print("matured_rex: ", matured_rex, "\n");

    bool empty_unstaking_queue = _unstakingtable.begin() == _unstakingtable.end();

    if (empty_unstaking_queue && (matured_rex >= rex_quantity)) {

        sub_rex_balance( beneficiary, rex_quantity );

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

    accrue_voting_rewards( owner );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.staked_balance -= value;
    });
}

void token::add_staked_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    accrue_voting_rewards( owner );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.staked_balance += value;
    });
}

void token::sub_rex_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    check( account.rex_balance.amount >= value.amount, "overdrawn locked balance" );
    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.rex_balance -= value;
    });
}

void token::add_rex_balance( const name& owner, const asset& value ){
    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    _accountstable.modify( account, same_payer, [&]( auto& a ) {
        a.rex_balance += value;
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
        a.rex_balance = asset(0, symbol("REX", 4));
        a.voting_rewards_accrued = asset(0, global.native_token_symbol);
        a.voting_rewards_last_accrued = current_time_point();

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
   check( it->rex_balance.amount == 0, "Cannot close because the rex balance is not zero." );
   check( it->voting_rewards_accrued.amount == 0, "Cannot close because the rewards accrued balance is not zero." );
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

void token::claimrewards( const name& owner ) {

    check(global_config.exists(), "contract must be initialized first");

    require_auth( owner );

    auto global = global_config.get();

    const auto& account = _accountstable.get( owner.value, "no balance object found" );

    // subtract extra REX that isn't covering staked EOS at current price, and add it to claim
    asset total_rex_eos_equiv = get_eos_sale_quantity(account.rex_balance);
    print("total_rex_eos_equiv: ", total_rex_eos_equiv, "\n");

    asset eos_rewards_from_rex = total_rex_eos_equiv - (account.staked_balance + account.unstaking_balance);
    print("eos_rewards_from_rex: ", eos_rewards_from_rex, "\n");

    if (eos_rewards_from_rex.amount > 0) {
        asset rex_to_sell = get_rex_sale_quantity(eos_rewards_from_rex);
        print("rex_to_sell: ", rex_to_sell, "\n");

        sub_rex_balance( owner, rex_to_sell );

        // sell rex
        action sellrex_act(
          permission_level{_self, "active"_n},
          "eosio"_n, "sellrex"_n,
          std::make_tuple( _self, rex_to_sell )
        );
        sellrex_act.send();

        action withdraw_act(
          permission_level{_self, "active"_n},
          "eosio"_n, "withdraw"_n,
          std::make_tuple( _self, eos_rewards_from_rex )
        );
        withdraw_act.send();
    }

    // move rex rewards to liquid balance
    if (eos_rewards_from_rex.amount > 0) {
        add_liquid_balance( owner, eos_rewards_from_rex );
    }

    // update voting rewards if there were any since last staking/unstaking action
    accrue_voting_rewards( owner );

    // move voting accrued voting rewards to liquid balance
    if (account.voting_rewards_accrued.amount > 0) {
        add_liquid_balance( owner, account.voting_rewards_accrued );
        clear_accrued_voting_rewards( owner );
    }

    // todo - add inline notification?
}

void token::processqueue( const uint64_t count )
{

    check(global_config.exists(), "contract must be initialized first");

    // anyone can call this

    const auto& rex_balance = _rexbaltable.get( _self.value, "no rex balance object found" );
    const asset matured_rex = get_matured_rex();
    print("matured_rex: ", matured_rex, "\n\n");

    auto _unstakingtable_by_start = _unstakingtable.get_index<"started"_n>();

    asset rex_to_sell = asset(0, symbol("REX", 4));
    for (uint64_t i = 0; i < count; i++) {
        auto itr = _unstakingtable_by_start.begin();
        if ( itr != _unstakingtable_by_start.end() ) {
            print("owner: ", itr->owner, "\n");
            asset eos_quantity = itr->quantity;
            print("eos_quantity to unstake: ", eos_quantity, "\n");
            asset rex_quantity = get_rex_sale_quantity(eos_quantity);
            print("rex_quantity to sell: ", rex_quantity, "\n");

            if (matured_rex - rex_to_sell >= rex_quantity) {
                sub_unstaking_balance(itr->owner, eos_quantity);
                sub_rex_balance( itr->owner, rex_quantity );
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

    void token::debug() {

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

        print("\nSimulating sales from staking accounts, pretending all rex has matured...\n\n");
        auto itra = _accountstable.begin();
        while ( itra != _accountstable.end() ) {
            if (itra->staked_balance.amount > 0) {
                print("matured_rex_balance: ", total_rex_balance, "\n");

                print("owner: ", itra->owner, "\n");
                asset eos_quantity = itra->staked_balance;
                print("eos_quantity to unstake: ", eos_quantity, "\n");
                asset rex_stake_quantity_to_sell = get_rex_sale_quantity(eos_quantity);
                print("rex_quantity to sell to cover stake: ", rex_stake_quantity_to_sell, "\n");
                asset eos_raised_from_rex_stake_sale = get_eos_sale_quantity(rex_stake_quantity_to_sell);
                print("eos_raised_from_rex_stake_sale: ", eos_raised_from_rex_stake_sale, "\n");

                asset rex_reward_quantity_to_sell = itra->rex_balance - rex_stake_quantity_to_sell;
                print("rex_quantity to sell for EOS reward: ", rex_reward_quantity_to_sell, "\n");
                asset eos_raised_from_rex_reward_sale = get_eos_sale_quantity(rex_reward_quantity_to_sell);
                print("eos_raised_from_rex_reward_sale: ", eos_raised_from_rex_reward_sale, "\n");

                asset rex_quantity = rex_stake_quantity_to_sell + rex_reward_quantity_to_sell;
                print("total rex_quantity to sell: ", rex_quantity, "\n");
                asset eos_proceeds_quantity = eos_raised_from_rex_stake_sale + eos_raised_from_rex_reward_sale;
                print("eos_proceeds_quantity: ", eos_proceeds_quantity, "\n");

                if (total_rex_balance >= rex_quantity) {
                    total_rex_balance -= rex_quantity;
                    print("Fulfilled ", eos_proceeds_quantity, " of ", eos_quantity, "\n\n");
                } else {
                    print("Insufficient rex!\n");
                    return; // stop at first request that can't be fulfilled
                }
            }
            itra++;
        }

        print("final matured_rex_balance: ", total_rex_balance, "\n");


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

      while (_historytable.begin() != _historytable.end()) {
        auto itr = _historytable.end();
        itr--;
        _historytable.erase(itr);
      }

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

