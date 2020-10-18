#include <crlpool.hpp>
#include <math.h>

extern "C" {
    void apply(uint64_t receiver, uint64_t code, uint64_t action) {
        if (code == receiver) {
            switch (action) {
                EOSIO_DISPATCH_HELPER(crlpool, (create)(claim)(withdraw)(harvest)(claimrewards)(afterclaim)(sendrewards))
            }
        } else {
            if (action == name("transfer").value) {
                crlpool inst(name(receiver), name(code), datastream<const char *>(nullptr, 0));
                const auto t = unpack_action_data<transfer_args>();
                inst.handle_transfer(t.from, t.to, t.quantity, t.memo, name(code));
            }
        }
    }
}

void crlpool::create(name contract, symbol sym, asset reward, uint32_t epoch_time, uint32_t duration, asset min_staked) {
    require_auth("coralmanager"_n);

    pools_mi pools_tbl(_self, _self.value);
    auto itr = pools_tbl.begin();
    
    while (itr != pools_tbl.end()) {
        auto exists = itr->contract == contract && itr->sym == sym;
        check(!exists, "Token exists");
        itr++;
    }

    check(reward.symbol == SSH_SYMBOL, "Reward symbol error");
    check(min_staked.symbol == sym, "Min-staked symbol error");

    auto total = reward;
    itr = pools_tbl.begin();
    while (itr != pools_tbl.end()) {
        total += itr->total_reward;
        itr++;
    }
    check(total.amount <= 300000000000000, "Reach the max circulation");

    auto pool_id = pools_tbl.available_primary_key();
    if (pool_id == 0) {
        pool_id = 1;
    }
    pools_tbl.emplace(_self, [&]( auto& a ) {
        a.id = pool_id;
        a.contract = contract;
        a.sym = sym;
        a.total_staked = asset(0, sym);
        a.total_reward = reward;
        a.released_reward = asset(0, reward.symbol);
        a.epoch_time = epoch_time;
        a.duration = duration;
        a.min_staked = min_staked;
        a.last_harvest_time = epoch_time;
    });
}

void crlpool::claim(name owner, uint64_t pool_id) {
    require_auth(owner);

    pools_mi pools_tbl(_self, _self.value);
    auto p_itr = pools_tbl.find(pool_id);  
    check(p_itr != pools_tbl.end(), "Pool not exists");

    miners_mi miners_tbl(_self, pool_id);
    auto m_itr = miners_tbl.find(owner.value);
    check(m_itr != miners_tbl.end(), "No this miner");
    check(m_itr->unclaimed.amount > 0, "No unclaimed");

    auto quantity = m_itr->unclaimed;
    miners_tbl.modify(m_itr, same_payer, [&]( auto& s) {
        s.claimed += quantity;
        s.unclaimed = asset(0, quantity.symbol);
    });
    
    utils::inline_transfer(SSH_CONTRACT, _self, owner, quantity, string("Minner claimed"));
}

void crlpool::withdraw(name owner, uint64_t pool_id) {
    require_auth(owner);

    pools_mi pools_tbl(_self, _self.value);
    auto p_itr = pools_tbl.find(pool_id);  
    check(p_itr != pools_tbl.end(), "Pool not exists");

    rounds_mi rounds_tbl(_self, _self.value);
    auto r_itr = rounds_tbl.find(pool_id);
    if (r_itr != rounds_tbl.end()) {
        check(r_itr->completed, "Harvesting, please wait");
    }

    miners_mi miners_tbl(_self, pool_id);
    auto m_itr = miners_tbl.find(owner.value);
    check(m_itr != miners_tbl.end(), "No this miner");
    auto unclaimed = m_itr->unclaimed;

    auto quantity = m_itr->staked;
    pools_tbl.modify(p_itr, same_payer, [&]( auto& s) {
        s.total_staked -= quantity;
    });
    miners_tbl.erase(m_itr);

    utils::inline_transfer(p_itr->contract, _self, owner, quantity, string("Minner withdraw"));
    if (unclaimed.amount > 0) {
        utils::inline_transfer(SSH_CONTRACT, _self, owner, unclaimed, string("Minner claimed"));
    }
}

void crlpool::harvest(uint64_t pool_id, uint64_t round_no, uint32_t limit) {
    require_auth("coralmanager"_n);

    pools_mi pools_tbl(_self, _self.value);
    auto itr = pools_tbl.find(pool_id);  
    check(itr != pools_tbl.end(), "Pool not exists");

    auto now_time = current_time_point().sec_since_epoch();
    check(now_time >= itr->epoch_time, "Mining hasn't started yet");
    check(now_time <= itr->epoch_time + itr->duration, "Mining is over");
    
    auto supply_per_second = itr->total_reward.amount / itr->duration;
    auto time_elapsed = now_time - itr->last_harvest_time;
    if (time_elapsed == 0) {
        return;
    }

    rounds_mi rounds_tbl(_self, _self.value);
    auto r_itr = rounds_tbl.find(pool_id);
    if (r_itr == rounds_tbl.end()) {
        r_itr = rounds_tbl.emplace(_self, [&]( auto& a) {
            a.pool_id = pool_id;
            a.no = 0;
            a.offset = name("");
            a.amount = 0;
            a.completed = true;
        });
    }
    
    auto offset = r_itr->offset;
    auto token_issued_amount = r_itr->amount;
    if (round_no != r_itr->no) {
        // new round
        check(r_itr->completed, "Last round not completed.");
        offset = name("");
        token_issued_amount = time_elapsed * supply_per_second;

        pools_tbl.modify(itr, same_payer, [&]( auto& s) {
            s.released_reward.amount += token_issued_amount;
            s.last_harvest_time = now_time;
        });
        
        auto data = make_tuple(_self, asset(token_issued_amount, itr->released_reward.symbol), string("Issue token"));
        action(permission_level{_self, "active"_n}, SSH_CONTRACT, "issue"_n, data).send();
    } else {
        check(!r_itr->completed, "This round is completed.");
    }

    
    // update every miner
    miners_mi miners_tbl(_self, itr->id);
    auto m_itr = miners_tbl.begin();
    check(m_itr != miners_tbl.end(), "No miners");
    
    if (offset != name("")) {
        m_itr = miners_tbl.find(offset.value);
    }
    int index = 0;
    while (m_itr != miners_tbl.end()) {
        double radio = (double)(m_itr->staked.amount) / itr->total_staked.amount;
        uint64_t amount = (uint64_t)(token_issued_amount * radio);
        miners_tbl.modify(m_itr, same_payer, [&]( auto& a) {
            a.unclaimed.amount += amount;
        });
        m_itr++;
        if (++index == limit) {
            break;
        }
    }
    auto completed = m_itr == miners_tbl.end();
    auto next_offset = completed ? name("") : m_itr->owner;
    rounds_tbl.modify(r_itr, same_payer, [&]( auto& s) {
        s.no = round_no;
        s.offset = next_offset;
        s.amount = token_issued_amount;
        s.completed = completed;
    });
}

void crlpool::claimrewards(uint8_t type) {
    require_auth(FEES_ACCOUNT);

    if (type == 0 || type == 1) {
        auto data = make_tuple(_self, symbol_code(""), 100);
        action(permission_level{_self, "active"_n}, BOX_LP_CONTRACT, "claimall"_n, data).send();
    }
    if (type == 0 || type == 2) {
        auto data = make_tuple(_self);
        action(permission_level{_self, "active"_n}, HBG_LP_CONTRACT, "updateall"_n, data).send();
        action(permission_level{_self, "active"_n}, HBG_LP_CONTRACT, "claim"_n, data).send();
    }
    action(permission_level{_self, "active"_n}, _self, "afterclaim"_n, make_tuple()).send();
}

void crlpool::afterclaim() {
    require_auth(_self);
    action(permission_level{_self, "active"_n}, _self, "sendrewards"_n, make_tuple()).send();
}

void crlpool::sendrewards() {
    require_auth(_self);
    auto bal1 = utils::get_balance(BOX_TOKEN_CONTRACT, _self, symbol("BOX", 6));
    if (bal1.amount > 0) {
        utils::inline_transfer(BOX_TOKEN_CONTRACT, _self, FEES_ACCOUNT, bal1, string("BOX rewards"));
    }
    auto bal2 = utils::get_balance(HBG_TOKEN_CONTRACT, _self, symbol("HBG", 6));
    if (bal2.amount > 0) {
        utils::inline_transfer(HBG_TOKEN_CONTRACT, _self, FEES_ACCOUNT, bal2, string("HBG rewards"));
    }
}

void crlpool::handle_transfer(name from, name to, asset quantity, string memo, name code) {
    if (from == _self || to != _self) {
        return;
    }
    if (from == BOX_LP_CONTRACT) {
        return;
    }
    if (from == HBG_LP_CONTRACT) {
        return;
    }
    if (from == SSH_CONTRACT) {
        return;
    }
    if (from == FEES_ACCOUNT) {
        return;
    }
    require_auth(from);
    auto sym = quantity.symbol;
    pools_mi pools_tbl(_self, _self.value);
    auto itr = pools_tbl.begin();
    while (itr != pools_tbl.end()) {
        if (itr->contract == code && itr->sym == sym) {
            break;
        }
        itr++;
    }
    check(itr != pools_tbl.end(), "Pool not found");
    check(itr->contract == code && itr->sym == sym, "Error token"); // recheck, actually don’t need to do this.
    check(quantity >= itr->min_staked, "The amount of staked is too small");
    auto now_time = current_time_point().sec_since_epoch();
    check(now_time <= itr->epoch_time + itr->duration, "Mining is over");

    pools_tbl.modify(itr, same_payer, [&]( auto& s) {
        s.total_staked += quantity;
    });

    rounds_mi rounds_tbl(_self, _self.value);
    auto r_itr = rounds_tbl.find(itr->id);
    if (r_itr != rounds_tbl.end()) {
        check(r_itr->completed, "Harvesting, please wait");
    }

    miners_mi miners_tbl(_self, itr->id);
    auto m_itr = miners_tbl.find(from.value);
    if (m_itr == miners_tbl.end()) {
        auto zero_crl = asset(0, SSH_SYMBOL);
        miners_tbl.emplace(_self, [&]( auto& a) {
            a.owner = from;
            a.staked = quantity;
            a.claimed = zero_crl;
            a.unclaimed = zero_crl;
        });
    } else {
        miners_tbl.modify(m_itr, same_payer, [&]( auto& a) {
            a.staked += quantity;
        });
    }
}