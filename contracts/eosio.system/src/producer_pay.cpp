#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   using eosio::current_time_point;
   using eosio::microseconds;
   using eosio::token;

   void system_contract::onblock( ignore<block_header> ) {
      using namespace eosio;

      require_auth(get_self());

      block_timestamp timestamp;
      name producer;
      _ds >> timestamp >> producer;

      // _gstate2.last_block_num is not used anywhere in the system contract code anymore.
      // Although this field is deprecated, we will continue updating it for now until the last_block_num field
      // is eventually completely removed, at which point this line can be removed.
      _gstate2.last_block_num = timestamp;

      /** until activation, no new rewards are paid */
      // if( _gstate.thresh_activated_stake_time == time_point() )
      //   return;

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end() ) {
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, same_payer, [&](auto& p ) {
               p.unpaid_blocks++;
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         update_elected_producers( timestamp );

         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {
            name_bid_table bids(get_self(), get_self().value);
            auto idx = bids.get_index<"highbid"_n>();
            auto highest = idx.lower_bound( std::numeric_limits<uint64_t>::max()/2 );
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                _gstate.thresh_activated_stake_time > time_point() &&
                (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
            ) {
               _gstate.last_name_close = timestamp;
               channel_namebid_to_rex( highest->high_bid );
               idx.modify( highest, same_payer, [&]( auto& b ){
                  b.high_bid = -b.high_bid;
               });
            }
         }
      }
   }
   
   
   void system_contract::claimrewards( const name& owner ) {
      require_auth( owner );
      const auto ct = current_time_point();

      check( ct < time_point(eosio::microseconds(payment_lockdown)), "payments locks down after 10.06.2025" );
      eosio::print("Claiming rewards; ");
      const auto& prod = _producers.get( owner.value );
      check( prod.active(), "producer does not have an active key" ); // метод может быть вызван только нодой
      
      eosio::print(" Chain activated: ");
      eosio::print(_gstate.thresh_activated_stake_time != time_point());
      eosio::print("; ");

      //check( _gstate.thresh_activated_stake_time != time_point(),
      //              "cannot claim rewards until the chain is activated (at least 15% of all tokens participate in voting)" ); // ? проверяем, можно ли выплатить стейк


      // check( ct - prod.last_claim_time > microseconds(useconds_per_hour * 3), "already claimed rewards within past 3 hours" ); // выплата наград не чаще, чем раз в 3 часа (MAINNET)
      //check( ct - prod.last_claim_time > microseconds(useconds_per_hour / 60), "already claimed rewards within past minute" ); // выплата наград не чаще, чем раз в минуту (TESTNET)

      const asset token_supply   = token::get_supply(token_account, cbs_symbol.code() ); // получить системный токен
      const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();    // сколько времени прошло после предыдущего пересчета наград

      bool is_validator = false;                            // это нода или валидатор
      auto idx = _prodextra.get_index<"bytotalstake"_n>();  
      int i = 0;

      // ищем ноду в топ-21, если она там есть - это валидатор
      for( auto it = idx.cbegin(); it != idx.cend() && i < 21; ++it ) {
          if (it->total_stake > 0) {
            if (it->owner == owner) {
                is_validator = true;
                eosio::print("This node is a validator; ");
                break;
            }
            
            ++i;
          }
      }

      auto prod2 = _producers2.find( owner.value );

      /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and
      /// time duration the vote weight has been held into one metric.
      const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);

      bool crossed_threshold       = (last_claim_plus_3days <= ct);
      bool updated_after_threshold = true;
      if ( prod2 != _producers2.end() ) {
         updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
      } else {
         prod2 = _producers2.emplace( owner, [&]( producer_info2& info  ) {
            info.owner                     = owner;
            info.last_votepay_share_update = ct;
         });
      }

      // получаем список стейкеров
      auto prod3 = _prodextra.find( owner.value );
      if ( prod3 == _prodextra.end() ) {
         // если нет записи в producer_table3, создаем ее
         prod3 = _prodextra.emplace( owner, [&]( prod_extra& info ) {
            info.owner = owner;
            info.fees  = 0;
            info.slots = std::vector<prod_slot>();
            info.total_stake = 0;
         });
      }

      double new_votepay_share = update_producer_votepay_share( prod2,
         ct,
         updated_after_threshold ? 0.0 : prod.total_votes,
         true // reset votepay_share to zero after updating
      );

      update_total_votepay_share( ct, -new_votepay_share, (updated_after_threshold ? prod.total_votes : 0.0) );

      int64_t new_tokens = 0; // Сколько токенов должно быть выпущено за месяц
      int64_t fee_rate = 0;   // Комиссия

      // Временный массив с наградами
      std::vector<std::pair<name, int64_t>> rewards;
      
      /* !!! OLD
      // Множитель по времени последней выплаты в %
      double time_mul = (double)((ct - prod.last_claim_time).count() / 1000) / (double)( (useconds_per_day / 1000) * 30 ); // mainnet
      //double time_mul = (ct - prod.last_claim_time).count() / ( useconds_per_hour / 30 ); // testnet
      eosio::print("Time multiplier: ");
      eosio::print((ct - prod.last_claim_time).count());
      eosio::print(" / ");
      eosio::print( useconds_per_day * 30 );
      eosio::print(" <--> ");
      eosio::print(time_mul);
      eosio::print("; ");
      */
      
      auto slots = prod3->slots;
      
      eosio::print("Checking slots (count=");
      eosio::print(slots.size());
      eosio::print("); ");
      for ( auto it = slots.begin(); it < slots.end(); it++ ) {
         eosio::print("Slot: ");
         eosio::print(it->stake_holder);
         eosio::print("; ");
         
         eosio::print("Stake: ");
         eosio::print(it->value);
         eosio::print("; time - last_pay: ");
         eosio::print((ct - it->last_pay).count() / 1000);
         
         if ( ct - it->last_pay < microseconds(useconds_per_hour * 3) ) {
             eosio::print(" :: Already claimed for this slot at past 3 hour, skip; ");
             continue;
         }

         int64_t reward = it->value;  // награда стейкера
         double time_mul = (double)( (ct - it->last_pay).count() ) / (double)( useconds_per_day * 30 );
         
         eosio::print("; time_mul: ");
         eosio::print(time_mul);
         eosio::print("; ");
         
         if ( is_validator ) {
            reward *= vote_mul;  // x7
         } else {
            reward *= node_mul;  // x6
         }
         
         reward = (int64_t)(time_mul * (double)reward);         // рассчитываем награду с учетом времени с прошлой выплаты
         
         eosio::print("Reward: ");
         eosio::print(reward);
         eosio::print("; ");

         int64_t fee = (int64_t)(prod3->fees * (double)reward); // рассчитываем комиссию
         eosio::print("Fee: ");
         eosio::print(fee);
         eosio::print("; ");

         reward -= fee;                // убиаем комиссию из награды
         fee_rate += fee;              // комиссию с этого стейкера плюсуем к общей комисии
         new_tokens += reward + fee;   // прибавляем награду и комиссию к общему объему эмисии

         rewards.push_back(std::make_pair(it->stake_holder, reward));  // записываем сколько выплатить награды этому стейкеру
         
         if ( reward > 0 ) {
             it->last_pay = ct;
             
            eosio::print("ct - last pay after: ");
            eosio::print((ct - it->last_pay).count());
            eosio::print("; ");
         }
      }

      eosio::print("New tokens: ");
      eosio::print(new_tokens);
      eosio::print("; ");
      
      if (new_tokens > 0) {
         {
            // выпускаем токены
            token::issue_action issue_act{ token_account, { {get_self(), active_permission} } };
            issue_act.send( get_self(), asset(new_tokens, cbsch_symbol), "issue tokens for pay to stake holders" );
         }
         {
            token::transfer_action transfer_act{ token_account, { {get_self(), active_permission} } };

            // переводим комиссию ноде/валидатору
            if ( fee_rate > 0 ) {
               transfer_act.send( get_self(), owner, asset(fee_rate, cbsch_symbol), "node or validator fee" );
            }

            // переводим награду стейкерам
            for ( auto & kv : rewards ) {
               if ( kv.second > 0 ) {
                  transfer_act.send( get_self(), kv.first, asset(kv.second, cbsch_symbol), "stake holder payment" );
               }
            }
         }
         
        // save last claim time
        _producers.modify( prod, same_payer, [&]( producer_info& info ){
            info.last_claim_time = ct;
        });
        
        std::sort( slots.rbegin(), slots.rend() );
        _prodextra.modify( prod3, same_payer, [&]( auto & info ) {
            info.slots = slots;
        });
      }
      
      eosio::print("Done.");
   }

} //namespace eosiosystem
