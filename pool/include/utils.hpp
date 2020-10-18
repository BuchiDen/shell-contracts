#include <structs.hpp>

namespace utils {

    void inline_transfer(name contract, name from, name to, asset quantity, string memo) {
        auto data = make_tuple(from, to, quantity, memo);
        action(permission_level{from, "active"_n}, contract, "transfer"_n, data).send();
    }

    asset get_balance(const name &token_contract, const name &owner, const symbol &sym) {
        asset ret = asset( 0, sym );
        accounts accounts_table( token_contract, owner.value );
        auto accounts_it = accounts_table.find( sym.code().raw() );
        if ( accounts_it != accounts_table.end() ) {
            ret = accounts_it->balance;
        }
        return ret;
    }
}