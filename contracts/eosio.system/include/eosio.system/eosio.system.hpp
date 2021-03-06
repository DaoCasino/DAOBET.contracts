#pragma once

#include <eosio/asset.hpp>
#include <eosio/binary_extension.hpp>
#include <eosio/privileged.hpp>
#include <eosio/singleton.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>

#include <eosio.system/contracts.version.hpp>
#include <eosio.system/exchange_state.hpp>
#include <eosio.system/native.hpp>

#include <deque>
#include <optional>
#include <string>
#include <type_traits>

// run cicd/build.sh with `--build-type Debug` option to enable enhanced logging
#ifdef DEBUG_MODE
# define ADD_DEBUG_LOG_MSG(msg) do { _dlogs.data.push_back(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " + (msg)); } while (0)
#else
# define ADD_DEBUG_LOG_MSG(msg) do {} while (0)
#endif // DEBUG_MODE


namespace eosiosystem {

   using eosio::asset;
   using eosio::block_timestamp;
   using eosio::check;
   using eosio::const_mem_fun;
   using eosio::datastream;
   using eosio::indexed_by;
   using eosio::name;
   using eosio::same_payer;
   using eosio::symbol;
   using eosio::symbol_code;
   using eosio::time_point;
   using eosio::time_point_sec;
   using eosio::unsigned_int;

   /// Check if bits in field are enabled among bits in flags interger.
   template<typename E, typename F>
   static inline auto has_field( F flags, E field )
   -> std::enable_if_t< std::is_integral_v<F> && std::is_unsigned_v<F> &&
                        std::is_enum_v<E> && std::is_same_v< F, std::underlying_type_t<E> >, bool>
   {
      return ( (flags & static_cast<F>(field)) != 0 );
   }

   template<typename E, typename F>
   static inline auto set_field( F flags, E field, bool value = true )
   -> std::enable_if_t< std::is_integral_v<F> && std::is_unsigned_v<F> &&
                        std::is_enum_v<E> && std::is_same_v< F, std::underlying_type_t<E> >, F >
   {
      if( value )
         return ( flags | static_cast<F>(field) );
      else
         return ( flags & ~static_cast<F>(field) );
   }

   static constexpr uint32_t seconds_per_year      = 52 * 7 * 24 * 3600;
   static constexpr uint32_t seconds_per_day       = 24 * 3600;
   static constexpr uint32_t seconds_per_hour      = 3600;
   static constexpr int64_t  useconds_per_year     = int64_t(seconds_per_year) * 1000'000ll;
   static constexpr int64_t  useconds_per_day      = int64_t(seconds_per_day) * 1000'000ll;
   static constexpr int64_t  useconds_per_hour     = int64_t(seconds_per_hour) * 1000'000ll;
   static constexpr uint32_t blocks_per_day        = 2 * seconds_per_day;  ///< half seconds per day
   static constexpr uint32_t blocks_per_hour       = 2 * 3600;

   static constexpr int64_t  min_activated_stake   = 25'090'624'0000;      ///< DAO: 15% of total supply (167'270'821 BET)
   static constexpr int64_t  ram_gift_bytes        = 1400;
   /// per vote reward is payed to the claimrewards action caller only if this reward is greater or equal to this value
   static constexpr int64_t  min_pervote_daily_pay = 100'0000;
   static constexpr uint32_t refund_delay_sec      = 14 * seconds_per_day; ///< DAO: stake lock up period = 2 weeks

   static constexpr int64_t  min_producer_activated_stake = 0;   ///< minimum activated stake

   /**
    * eosio.system contract defines the structures and actions needed for blockchain's core functionality.
    * - Users can stake tokens for CPU and Network bandwidth, and then vote for producers or
    *    delegate their vote to a proxy.
    * - Producers register in order to be voted for, and can claim per-block and per-vote rewards.
    * - Users can buy and sell RAM at a market-determined price.
    * - Users can bid on premium names.
    */

   // A name bid, which consists of:
   // - a `newname` name that the bid is for
   // - a `high_bidder` account name that is the one with the highest bid so far
   // - the `high_bid` which is amount of highest bid
   // - and `last_bid_time` which is the time of the highest bid
   struct [[eosio::table, eosio::contract("eosio.system")]] name_bid {
     name       newname;
     name       high_bidder;
     int64_t    high_bid = 0; ///< negative high_bid == closed auction waiting to be claimed
     time_point last_bid_time;

     uint64_t primary_key() const { return newname.value;                    }
     uint64_t by_high_bid() const { return static_cast<uint64_t>(-high_bid); }
   };
   typedef eosio::multi_index< "namebids"_n, name_bid,
                               indexed_by<"highbid"_n, const_mem_fun<name_bid, uint64_t, &name_bid::by_high_bid>  >
                             > name_bid_table;

   // Bid refund table
   struct [[eosio::table, eosio::contract("eosio.system")]] bid_refund {
      name  bidder; ///< account name owning the refund
      asset amount; ///< amount to be refunded

      uint64_t primary_key() const { return bidder.value; }
   };
   typedef eosio::multi_index< "bidrefunds"_n, bid_refund > bid_refund_table;

   /// Global state parameters.
   struct [[eosio::table("global"), eosio::contract("eosio.system")]] eosio_global_state : eosio::blockchain_parameters {
      uint64_t free_ram() const { return max_ram_size - total_ram_bytes_reserved; }

      uint64_t        max_ram_size = 64ll*1024 * 1024 * 1024; ///< maximal RAM supply size (bytes) that may be reserved by blockchain node
      uint64_t        total_ram_bytes_reserved = 0;           ///< currently reserved RAM amount (bytes); should be less or equal to max_ram_size
      int64_t         total_ram_stake = 0;                    ///< currently total staked RAM (asset)

      block_timestamp last_producer_schedule_update;          ///< for cyclic schedule updates
      time_point      last_pervote_bucket_fill;               ///< used to count reward inflation; @see system_contract::claimrewards
      /// tokens amount sent to vpay account (reward for votes) in claimrewards action, excluding reward payed to caller
      int64_t         pervote_bucket = 0;
      int64_t         perblock_bucket = 0;                    ///< reward for unpaid blocks payed to the claimrewards caller
      uint32_t        total_unpaid_blocks = 0;                ///< all blocks which have been produced but not paid
      int64_t         total_activated_stake = 0;              ///< last active_stake value after reaching min_activated_stake
      int64_t         active_stake = 0;                       ///< current total activated stake
      time_point      thresh_activated_stake_time;            ///< timepoint when min_activated_stake is reached
      uint16_t        target_producer_schedule_size = 21;     ///< current maximal number of active BPs
      uint16_t        last_producer_schedule_size = 0;        ///< size of the current producers schedule
      double          total_producer_vote_weight = 0;         ///< the sum of all producer votes
      block_timestamp last_name_close;

      ///@{
      /// @deprecated unused since 1.8
      /// See eosio_global_state4 instead.
      block_timestamp last_target_schedule_size_update;        ///< ts of last producers schedule update
      uint32_t        schedule_update_interval = 60 * 60 * 24; ///< min interval between changes in producer schedule
      ///@}

      uint16_t             schedule_size_step = 3;                  ///< schedule size change step

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE_DERIVED( eosio_global_state, eosio::blockchain_parameters,
                                (max_ram_size)(total_ram_bytes_reserved)(total_ram_stake)
                                (last_producer_schedule_update)(last_pervote_bucket_fill)
                                (pervote_bucket)(perblock_bucket)(total_unpaid_blocks)(total_activated_stake)(active_stake)(thresh_activated_stake_time)
                                (target_producer_schedule_size)(last_producer_schedule_size)(total_producer_vote_weight)(last_name_close)
                                (last_target_schedule_size_update)(schedule_update_interval)(schedule_size_step) )
   };
   typedef eosio::singleton< "global"_n, eosio_global_state > global_state_singleton;

   /// Additional fields to eosio_global_state (since v1.0).
   struct [[eosio::table("global2"), eosio::contract("eosio.system")]] eosio_global_state2 {
      eosio_global_state2(){}

      uint16_t          new_ram_per_block = 0;
      block_timestamp   last_ram_increase;
      block_timestamp   last_block_num;                   ///< @deprecated
      double            total_producer_votepay_share = 0;
      uint8_t           revision = 0;                     ///< used to track version updates in the future.

      EOSLIB_SERIALIZE( eosio_global_state2, (new_ram_per_block)(last_ram_increase)(last_block_num)
                        (total_producer_votepay_share)(revision) )
   };
   typedef eosio::singleton< "global2"_n, eosio_global_state2 > global_state2_singleton;

   /// Additional fields to eosio_global_state2 (since v1.3.0).
   struct [[eosio::table("global3"), eosio::contract("eosio.system")]] eosio_global_state3 {
      eosio_global_state3() { }
      time_point        last_vpay_state_update;
      double            total_vpay_share_change_rate = 0;

      EOSLIB_SERIALIZE( eosio_global_state3, (last_vpay_state_update)(total_vpay_share_change_rate) )
   };
   typedef eosio::singleton< "global3"_n, eosio_global_state3 > global_state3_singleton;

   /// Additional fields to eosio_global_state2 (since v1.8).
   struct [[eosio::table("global4"), eosio::contract("eosio.system")]] eosio_global_state4 {
      eosio_global_state4() {}

      block_timestamp last_schedule_size_decrease;             ///< last producers schedule decrease time
      block_timestamp last_schedule_size_increase;             ///< last producers schedule increase time

      uint32_t schedule_decrease_delay_sec = seconds_per_day;  ///< min interval (seconds) before next producer schedule size decrease
      uint32_t schedule_increase_delay_sec = seconds_per_year; ///< min interval (seconds) before next producer schedule size increase

      EOSLIB_SERIALIZE( eosio_global_state4,
         (last_schedule_size_decrease)(last_schedule_size_increase)
         (schedule_decrease_delay_sec)(schedule_increase_delay_sec) )
   };
   typedef eosio::singleton< "global4"_n, eosio_global_state4 > global_state4_singleton;

   /// Block producer information, stored in `producer_info` (since v1.0).
   struct [[eosio::table, eosio::contract("eosio.system")]] producer_info {
      name              owner;
      double            total_votes = 0;
      eosio::public_key producer_key;      ///< a packed public key object
      bool              is_active = true;
      std::string       url;
      uint32_t          unpaid_blocks = 0;
      time_point        last_claim_time;
      uint16_t          location = 0;

      uint64_t primary_key() const { return owner.value;                             }
      double   by_votes() const    { return is_active ? -total_votes : total_votes;  }
      bool     active() const      { return is_active;                               }
      void     deactivate()        { producer_key = public_key(); is_active = false; }

      EOSLIB_SERIALIZE( producer_info, (owner)(total_votes)(producer_key)(is_active)(url)
                        (unpaid_blocks)(last_claim_time)(location) )
   };
   typedef eosio::multi_index< "producers"_n, producer_info,
                               indexed_by<"prototalvote"_n, const_mem_fun<producer_info, double, &producer_info::by_votes>  >
                             > producers_table;

   /// Additional fields to producer_info structure (since v1.3.0).
   struct [[eosio::table, eosio::contract("eosio.system")]] producer_info2 {
      name       owner;
      double     votepay_share = 0;
      time_point last_votepay_share_update;

      uint64_t primary_key()const { return owner.value; }

      EOSLIB_SERIALIZE( producer_info2, (owner)(votepay_share)(last_votepay_share_update) )
   };
   typedef eosio::multi_index< "producers2"_n, producer_info2 > producers_table2;

   /// Voter information.
   struct [[eosio::table, eosio::contract("eosio.system")]] voter_info {
      name              owner;                  ///< voter account name
      name              proxy;                  ///< proxy set by the voter, if any
      std::vector<name> producers;              ///< producers approved by this voter if no proxy set
      int64_t           staked = 0;             ///< amount staked
      /// Every time a vote is cast we must first "undo" the last vote weight, before casting the
      /// new vote weight. Vote weight is calculated as:
      /// stated.amount * 2^(weeks_since_launch/weeks_per_year)
      double            last_vote_weight = 0;   ///< the vote weight cast the last time the vote was updated
      /// Total vote weight delegated to this voter.
      double            proxied_vote_weight= 0; ///< the total vote weight delegated to this voter as a proxy
      bool              is_proxy = 0;           ///< whether the voter is a proxy for others

      uint32_t          flags1 = 0;
      uint32_t          reserved2 = 0;
      eosio::asset      reserved3;

      uint64_t primary_key()const { return owner.value; }
      bool is_active() const { return producers.size() || proxy; }

      enum class flags1_fields : uint32_t {
         ram_managed = 1,
         net_managed = 2,
         cpu_managed = 4
      };

      eosio::binary_extension<bool> has_voted;  ///< @deprecated since merging with eosio.contracts-1.8.3

      EOSLIB_SERIALIZE( voter_info, (owner)(proxy)(producers)(staked)(last_vote_weight)(proxied_vote_weight)(is_proxy)(flags1)(reserved2)(reserved3)(has_voted) )
   };
   typedef eosio::multi_index< "voters"_n, voter_info > voters_table;

   /// Contracts version table.
   struct [[eosio::table("version"), eosio::contract("eosio.system")]] version_info {
      std::string version = CONTRACTS_VERSION; ///< version string
      EOSLIB_SERIALIZE( version_info, (version) )
   };
   typedef eosio::singleton< "version"_n, version_info > contracts_version_singleton;


   /// Tables user_resources, delegated_bandwidth, and refund_request are designed to be constructed in the scope of
   /// the relevant user, this facilitates simpler API for per-user queries.

   /// User resources: network, CPU, votes & RAM.
   struct [[eosio::table, eosio::contract("eosio.system")]] user_resources {
      name    owner;         ///< user account name
      asset   net_weight;    ///< tokens staked for network bandwidth
      asset   cpu_weight;    ///< tokens staked for CPU bandwidth
      asset   vote_weight;   ///< tokens staked for votes
      int64_t ram_bytes = 0; ///< bytes bought for RAM bandwidth

      bool is_empty() const { return net_weight.amount == 0 && cpu_weight.amount == 0 && vote_weight.amount == 0 && ram_bytes == 0; }
      uint64_t primary_key() const { return owner.value; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( user_resources, (owner)(net_weight)(cpu_weight)(vote_weight)(ram_bytes) )
   };
   typedef eosio::multi_index< "userres"_n, user_resources > user_resources_table;

   /// Every user `from` has a scope/table that uses every receipient `to` as the primary key.
   struct [[eosio::table, eosio::contract("eosio.system")]] delegated_bandwidth {
      name  from;
      name  to;
      asset net_weight;
      asset cpu_weight;
      asset vote_weight;

      bool is_empty() const { return net_weight.amount == 0 && cpu_weight.amount == 0 && vote_weight.amount == 0; }
      uint64_t primary_key() const { return to.value; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( delegated_bandwidth, (from)(to)(net_weight)(cpu_weight)(vote_weight) )
   };
   typedef eosio::multi_index< "delband"_n, delegated_bandwidth > del_bandwidth_table;

   struct [[eosio::table, eosio::contract("eosio.system")]] refund_request {
      name            owner;
      time_point_sec  request_time;
      eosio::asset    net_amount;
      eosio::asset    cpu_amount;
      eosio::asset    vote_amount;

      bool is_empty() const { return net_amount.amount == 0 && cpu_amount.amount == 0 && vote_amount.amount == 0; }
      uint64_t primary_key() const { return owner.value; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( refund_request, (owner)(request_time)(net_amount)(cpu_amount)(vote_amount) )
   };
   typedef eosio::multi_index< "refunds"_n, refund_request > refunds_table;


#ifdef DEBUG_MODE
   /// Some actions (like onblock) do not allow to print anything, so we use this table for debugging.
   struct [[eosio::table, eosio::contract("eosio.system")]] dlogs {
      std::vector<std::string> data; // array of log messages

      EOSLIB_SERIALIZE( dlogs, (data) );
   };
   typedef eosio::singleton< "dlogs"_n, dlogs > dlogs_singleton;
#endif // DEBUG_MODE

   /**
    * The EOSIO system contract. The EOSIO system contract governs RAM market, voters, producers, global state.
    */
   class [[eosio::contract("eosio.system")]] system_contract : public native {

      private:
         voters_table                _voters;
         producers_table             _producers;
         producers_table2            _producers2;
         global_state_singleton      _global;
         global_state2_singleton     _global2;
         global_state3_singleton     _global3;
         global_state4_singleton     _global4;
         eosio_global_state          _gstate;
         eosio_global_state2         _gstate2;
         eosio_global_state3         _gstate3;
         eosio_global_state4         _gstate4;
         rammarket                   _rammarket;
         contracts_version_singleton _contracts_version;
#ifdef DEBUG_MODE
         dlogs                       _dlogs;
         dlogs_singleton             _dlogs_singleton;
#endif // DEBUG_MODE

      public:
         static constexpr eosio::name active_permission{"active"_n};
         static constexpr eosio::name token_account{"eosio.token"_n};
         static constexpr eosio::name ram_account{"eosio.ram"_n};
         static constexpr eosio::name ramfee_account{"eosio.ramfee"_n};
         static constexpr eosio::name stake_account{"eosio.stake"_n};
         static constexpr eosio::name bpay_account{"eosio.bpay"_n};
         static constexpr eosio::name vpay_account{"eosio.vpay"_n};
         static constexpr eosio::name names_account{"eosio.names"_n};
         static constexpr eosio::name saving_account{"eosio.saving"_n};
         static constexpr eosio::name null_account{"eosio.null"_n};
         static constexpr symbol ramcore_symbol = symbol(symbol_code("RAMCORE"), 4);
         static constexpr symbol ram_symbol     = symbol(symbol_code("RAM"), 0);

         system_contract( name s, name code, datastream<const char*> ds );
         ~system_contract();

         /// Returns the core symbol by system account name
         /// @param system_account the system account to get the core symbol for.
         static symbol get_core_symbol( name system_account = "eosio"_n ) {
            rammarket rm(system_account, system_account.value);
            const static auto sym = get_core_symbol( rm );
            return sym;
         }

         // Actions:

         /**
          * `init` action initializes the system contract for a version and a symbol.
          * Only succeeds when:
          * - version is 0, and
          * - symbol is found, and
          * - system token supply is greater than 0, and
          * - system contract wasn't already been initialized.
          *
          * @param version version, has to be 0,
          * @param core    system symbol.
          */
         [[eosio::action]]
         void init( unsigned_int version, const symbol& core );

         /**
          * `onblock` action.
          * This special action is triggered when a block is applied by the given producer
          * and cannot be generated from any other source. It is used to pay producers and calculate
          * missed blocks of other producers. Producer pay is deposited into the producer's stake
          * balance and can be withdrawn over time. If blocknum is the start of a new round this may
          * update the active producer config from the producer votes.
          *
          * @param header block header produced.
          */
         [[eosio::action]]
         void onblock( ignore<block_header> header );

         /**
          * Set account resource limits action.
          *
          * @param account    name of the account whose resource limits to be set,
          * @param ram_bytes  RAM limit in absolute bytes,
          * @param net_weight fractionally proportionate network limit of available resources based on (weight / total_weight_of_all_accounts),
          * @param cpu_weight fractionally proportionate CPU limit of available resources based on (weight / total_weight_of_all_accounts).
          */
         [[eosio::action]]
         void setalimits( const name& account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight );

         /**
          * Set account RAM limits action.
          *
          * @param account   name of the account whose resource limit to be set,
          * @param ram_bytes RAM limit in absolute bytes.
          */
         [[eosio::action]]
         void setacctram( const name& account, const std::optional<int64_t>& ram_bytes );

         /**
          * Set account network limits action.
          *
          * @param account    name of the account whose resource limit to be set,
          * @param net_weight fractionally proportionate network limit of available resources based on (weight / total_weight_of_all_accounts).
          */
         [[eosio::action]]
         void setacctnet( const name& account, const std::optional<int64_t>& net_weight );

         /**
          * Set account CPU limits action.
          *
          * @param account    name of the account whose resource limit to be set,
          * @param cpu_weight fractionally proportionate CPU limit of available resources based on (weight / total_weight_of_all_accounts).
          */
         [[eosio::action]]
         void setacctcpu( const name& account, const std::optional<int64_t>& cpu_weight );


         /**
          * Activate a protocol feature action.
          *
          * @param feature_digest hash of the protocol feature to activate.
          */
         [[eosio::action]]
         void activate( const eosio::checksum256& feature_digest );

         // functions defined in delegate_bandwidth.cpp

         /**
          * Delegate bandwidth and/or CPU action. Stakes SYS from the balance of `from` for the benefit of `receiver`.
          *
          * @param from                account to delegate bandwidth from, that is, account holding tokens to be staked,
          * @param receiver            account to delegate bandwith to, that is, account to whose resources staked tokens are added
          * @param stake_net_quantity  tokens staked for network bandwidth,
          * @param stake_cpu_quantity  tokens staked for CPU bandwidth,
          * @param stake_vote_quantity tokens staked for voting,
          * @param transfer            if true, ownership of staked tokens is transfered to `receiver`.
          *
          * @post All producers `from` account has voted for will have their votes updated immediately.
          */
         [[eosio::action]]
         void delegatebw( name from, name receiver,
                          const asset& stake_net_quantity,
                          const asset& stake_cpu_quantity,
                          const asset& stake_vote_quantity,
                          bool transfer );

         /**
          * Undelegate bandwitdh action.
          * Decreases the total tokens delegated by `from` to `receiver` and/or
          * frees the memory associated with the delegation if there is nothing
          * left to delegate.
          * This will cause an immediate reduction in network/CPU bandwidth of the
          * receiver.
          * A transaction is scheduled to send the tokens back to `from` after
          * the staking period has passed. If existing transaction is scheduled, it
          * will be canceled and a new transaction issued that has the combined
          * undelegated amount.
          * The `from` account loses voting power as a result of this call and
          * all producer tallies are updated.
          *
          * @param from                  account to undelegate bandwidth from, that is, account whose tokens will be unstaked,
          * @param receiver              account to undelegate bandwith to, that is, account to whose benefit tokens have been staked,
          * @param unstake_net_quantity  tokens to be unstaked from network bandwidth,
          * @param unstake_cpu_quantity  tokens to be unstaked from CPU bandwidth,
          * @param unstake_vote_quantity tokens to be unstaked from voting,
          *
          * @post Unstaked tokens are transferred to `from` liquid balance via a deferred transaction with a delay of 3 days.
          * @post If called during the delay period of a previous `undelegatebw` action, pending action is canceled and timer is reset.
          * @post All producers `from` account has voted for will have their votes updated immediately.
          * @post Bandwidth and storage for the deferred transaction are billed to `from`.
          */
         [[eosio::action]]
         void undelegatebw( name from,
                            name receiver,
                            const asset& unstake_net_quantity,
                            const asset& unstake_cpu_quantity,
                            const asset& unstake_vote_quantity );

         /**
          * RAM purchaise action.
          * Increases receiver's RAM quota based upon current price and quantity of tokens provided. An inline transfer
          * from receiver to system contract of tokens will be executed.
          *
          * @param payer    the RAM buyer account name,
          * @param receiver the RAM receiver account name,
          * @param quant    the quntity of tokens to buy RAM with.
          */
         [[eosio::action]]
         void buyram( const name& payer, const name& receiver, const asset& quant );

         /**
          * RAM bytes purchaise action. Increases receiver's RAM in quantity of bytes provided.
          * An inline transfer from receiver to system contract of tokens will be executed.
          *
          * @param payer    RAM buyer account name,
          * @param receiver RAM receiver account name,
          * @param bytes    quntity of RAM (in bytes) to buy.
          */
         [[eosio::action]]
         void buyrambytes( const name& payer, const name& receiver, uint32_t bytes );

         /**
          * Sell RAM action. Reduces quota by bytes and then performs an inline transfer of tokens
          * to receiver based upon the average purchase price of the original quota.
          *
          * @param account RAM seller account name,
          * @param bytes   amount of RAM (in bytes) to sell.
          */
         [[eosio::action]]
         void sellram( const name& account, int64_t bytes );

         /**
          * Refund action. This action is called after the delegation-period to claim all pending
          * unstaked tokens belonging to owner.
          *
          * @param owner owner account name of the tokens claimed.
          */
         [[eosio::action]]
         void refund( const name& owner );

         // functions defined in voting.cpp

         /**
          * Producer registration action. Indicates that a particular account wishes to become a producer,
          * this action will create a `producer_config` and a `producer_info` object for `producer` scope
          * in producers tables.
          *
          * @param producer     account name registering to be a producer candidate,
          * @param producer_key the public key of the block producer, this is the key used by block producer to sign blocks,
          * @param url          the url of the block producer, normally the url of the block producer presentation website,
          * @param location     is the country code as defined in the ISO 3166, https://en.wikipedia.org/wiki/List_of_ISO_3166_country_codes
          *
          * @pre Producer is not already registered
          * @pre Producer to register is an account
          * @pre Authority of producer to register
          */
         [[eosio::action]]
         void regproducer( const name& producer, const public_key& producer_key, const std::string& url, uint16_t location );

         /**
          * Producer undegistration action. Deactivate the block producer with account name `producer`.
          *
          * @details Deactivate the block producer with account name `producer`.
          * @param producer block producer account to unregister.
          */
         [[eosio::action]]
         void unregprod( const name& producer );

         /**
          * Set RAM supply action.
          * @param max_ram_size amount of RAM supply to set.
          */
         [[eosio::action]]
         void setram( uint64_t max_ram_size );

         /**
          * Set RAM rate action. Sets the rate of increase of RAM in bytes per block. It is capped by the uint16_t to
          * a maximum rate of 3 TB per year. If update_ram_supply hasn't been called for the most recent block,
          * then new RAM will be allocated at the old rate up to the present block before switching the rate.
          *
          * @param bytes_per_block amount of bytes per block increase to set.
          */
         [[eosio::action]]
         void setramrate( uint16_t bytes_per_block );

         /**
          * Producer voting action. Votes for a set of producers. This action updates the list of `producers` voted for,
          * for `voter` account. If voting for a `proxy`, the producer votes will not change until the
          * proxy updates their own vote. Voter can vote for a proxy __or__ a list of at most 1 producer.
          * Storage change is billed to `voter`.
          *
          * @param voter     account to change the voted producers for,
          * @param proxy     proxy to change the voted producers for,
          * @param producers list of producers to vote for, a maximum of 1 producer is allowed.
          *
          * @pre Producers must be sorted from lowest to highest and must be registered and active
          * @pre If proxy is set then no producers can be voted for
          * @pre If proxy is set then proxy account must exist and be registered as a proxy
          * @pre Every listed producer or proxy must have been previously registered
          * @pre Voter must authorize this action
          * @pre Voter must have previously staked some EOS for voting
          * @pre Voter->staked must be up to date
          *
          * @post Every producer previously voted for will have vote reduced by previous vote weight
          * @post Every producer newly voted for will have vote increased by new vote amount
          * @post Prior proxy will proxied_vote_weight decremented by previous vote weight
          * @post New proxy will proxied_vote_weight incremented by new vote weight
          */
         [[eosio::action]]
         void voteproducer( const name& voter, const name& proxy, const std::vector<name>& producers );

         /**
          * Proxy regstration action. Set `proxy` account as proxy.
          * An account marked as a proxy can vote with the weight of other accounts which
          * have selected it as a proxy. Other accounts must refresh their voteproducer to
          * update the proxy's weight.
          * Storage change is billed to `proxy`.
          *
          * @param rpoxy   the account registering as voter proxy (or unregistering),
          * @param isproxy if true, proxy is registered; if false, proxy is unregistered.
          *
          * @pre Proxy must have something staked (existing row in voters table)
          * @pre New state must be different than current state
          */
         [[eosio::action]]
         void regproxy( const name& proxy, bool isproxy );

         /**
          * Set the blockchain parameters. By tunning these parameters a degree of customization can be achieved.
          *
          * @param params new blockchain parameters to set.
          */
         [[eosio::action]]
         void setparams( const eosio::blockchain_parameters& params );

         /**
          * Rewards claiming action. Claim block producing and vote rewards.
          *
          * @param owner producer account claiming per-block and per-vote rewards.
          */
         [[eosio::action]]
         void claimrewards( const name& owner );

         /**
          * Set privilege status for an account. Allows to set privilege status for an account (turn it on/off).
          *
          * @param account the account to set the privileged status for.
          * @param is_priv 0 for false, > 0 for true.
          */
         [[eosio::action]]
         void setpriv( const name& account, uint8_t is_priv );

         /**
          * Producer removal action. Deactivates a producer by name, if not found asserts.
          *
          * @param producer the producer account to deactivate.
          */
         [[eosio::action]]
         void rmvproducer( const name& producer );

         /**
          * Revision update action.
          *
          * @param revision it has to be incremented by 1 compared with current revision.
          *
          * @pre Current revision can not be higher than 254, and has to be smaller
          *      than or equal 1 (set upper bound to greatest revision supported in the code).
          */
         [[eosio::action]]
         void updtrevision( uint8_t revision );

         /**
          * Name bidding action. Allows an account `bidder` to place a bid for a name `newname`.
          * @param bidder  account placing the bid,
          * @param newname name the bid is placed for,
          * @param bid     amount of system tokens payed for the bid.
          *
          * @pre Bids can be placed only on top-level suffix,
          * @pre Non empty name,
          * @pre Names longer than 12 chars are not allowed,
          * @pre Names equal with 12 chars can be created without placing a bid,
          * @pre Bid has to be bigger than zero,
          * @pre Bid's symbol must be system token,
          * @pre Bidder account has to be different than current highest bidder,
          * @pre Bid must increase current bid by 10%,
          * @pre Auction must still be opened.
          */
         [[eosio::action]]
         void bidname( const name& bidder, const name& newname, const asset& bid );

         /**
          * Bid refunding action. Allows the account `bidder` to get back the amount it bid so far on a `newname` name.
          *
          * @param bidder  account that gets refunded,
          * @param newname name for which the bid was placed and now it gets refunded for.
          */
         [[eosio::action]]
         void bidrefund( const name& bidder, const name& newname );

         using init_action         = eosio::action_wrapper<"init"_n,         &system_contract::init>;
         using setacctram_action   = eosio::action_wrapper<"setacctram"_n,   &system_contract::setacctram>;
         using setacctnet_action   = eosio::action_wrapper<"setacctnet"_n,   &system_contract::setacctnet>;
         using setacctcpu_action   = eosio::action_wrapper<"setacctcpu"_n,   &system_contract::setacctcpu>;
         using activate_action     = eosio::action_wrapper<"activate"_n,     &system_contract::activate>;
         using delegatebw_action   = eosio::action_wrapper<"delegatebw"_n,   &system_contract::delegatebw>;
         using undelegatebw_action = eosio::action_wrapper<"undelegatebw"_n, &system_contract::undelegatebw>;
         using buyram_action       = eosio::action_wrapper<"buyram"_n,       &system_contract::buyram>;
         using buyrambytes_action  = eosio::action_wrapper<"buyrambytes"_n,  &system_contract::buyrambytes>;
         using sellram_action      = eosio::action_wrapper<"sellram"_n,      &system_contract::sellram>;
         using refund_action       = eosio::action_wrapper<"refund"_n,       &system_contract::refund>;
         using regproducer_action  = eosio::action_wrapper<"regproducer"_n,  &system_contract::regproducer>;
         using unregprod_action    = eosio::action_wrapper<"unregprod"_n,    &system_contract::unregprod>;
         using setram_action       = eosio::action_wrapper<"setram"_n,       &system_contract::setram>;
         using setramrate_action   = eosio::action_wrapper<"setramrate"_n,   &system_contract::setramrate>;
         using voteproducer_action = eosio::action_wrapper<"voteproducer"_n, &system_contract::voteproducer>;
         using regproxy_action     = eosio::action_wrapper<"regproxy"_n,     &system_contract::regproxy>;
         using claimrewards_action = eosio::action_wrapper<"claimrewards"_n, &system_contract::claimrewards>;
         using rmvproducer_action  = eosio::action_wrapper<"rmvproducer"_n,  &system_contract::rmvproducer>;
         using updtrevision_action = eosio::action_wrapper<"updtrevision"_n, &system_contract::updtrevision>;
         using bidname_action      = eosio::action_wrapper<"bidname"_n,      &system_contract::bidname>;
         using bidrefund_action    = eosio::action_wrapper<"bidrefund"_n,    &system_contract::bidrefund>;
         using setpriv_action      = eosio::action_wrapper<"setpriv"_n,      &system_contract::setpriv>;
         using setalimits_action   = eosio::action_wrapper<"setalimits"_n,   &system_contract::setalimits>;
         using setparams_action    = eosio::action_wrapper<"setparams"_n,    &system_contract::setparams>;

      private:
         // Implementation details:

         static symbol get_core_symbol( const rammarket& rm ) {
            auto itr = rm.find(ramcore_symbol.raw());
            check(itr != rm.end(), "system contract must first be initialized");
            return itr->quote.balance.symbol;
         }

         // defined in eosio.system.cpp
         static eosio_global_state get_default_parameters();
         symbol core_symbol()const;
         void update_ram_supply();

         // defined in delegate_bandwidth.cpp
         void changebw( name from, name receiver,
                        const asset& stake_net_quantity,
                        const asset& stake_cpu_quantity,
                        const asset& stake_vote_quantity,
                        bool transfer );
         void update_voting_power( const name& voter, const asset& total_update );

         // defined in voting.hpp
         void update_elected_producers( const block_timestamp& timestamp );
         void update_votes( const name& voter, const name& proxy, const std::vector<name>& producers, bool voting );
         void propagate_weight_change( const voter_info& voter );
         double update_producer_votepay_share( const producers_table2::const_iterator& prod_itr,
                                               const time_point& ct,
                                               double shares_rate, bool reset_to_zero = false );
         double update_total_votepay_share( const time_point& ct,
                                            double additional_shares_delta = 0.0, double shares_rate_delta = 0.0 );

         template <auto system_contract::*...Ptrs>
         class registration {
            public:
               template <auto system_contract::*P, auto system_contract::*...Ps>
               struct for_each {
                  template <typename... Args>
                  static constexpr void call( system_contract* this_contract, Args&&... args )
                  {
                     std::invoke( P, this_contract, args... );
                     for_each<Ps...>::call( this_contract, std::forward<Args>(args)... );
                  }
               };
               template <auto system_contract::*P>
               struct for_each<P> {
                  template <typename... Args>
                  static constexpr void call( system_contract* this_contract, Args&&... args )
                  {
                     std::invoke( P, this_contract, std::forward<Args>(args)... );
                  }
               };

               template <typename... Args>
               constexpr void operator() ( Args&&... args )
               {
                  for_each<Ptrs...>::call( this_contract, std::forward<Args>(args)... );
               }

               system_contract* this_contract;
         };
   };

}
