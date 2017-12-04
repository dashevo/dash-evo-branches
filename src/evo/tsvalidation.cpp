// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/validation.h"
#include "tsvalidation.h"
#include "tsmempool.h"
#include "txmempool.h"

static bool CheckTransitionSignatures(const CTransition &ts, const CEvoUser &user, CValidationState &state) {
    std::string err;
    if (!user.VerifySig(ts.MakeSignMessage(), ts.vchUserSig, err))
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-usersig", false, err);

    // TODO check MN quorum sigs
    return true;
}

static bool Process_UpdateData(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    user.SetLastTransition(ts.GetHash());
    return true;
}

static bool Process_ResetKey(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    user.PushPubKey(ts.newPubKey);
    return true;
}

static bool Process_CloseAccount(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    user.SetClosed(true);
    return true;
}

static bool Undo_UpdateData(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    if (user.GetLastTransition() != ts.GetHash()) {
        return state.Error(strprintf("unexpected last subtx %s for user %s", user.GetLastTransition().ToString(), user.GetRegTxId().ToString()));
    }
    user.SetLastTransition(ts.hashPrevTransition);
    return true;
}

static bool Undo_ResetKey(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    CPubKey key = user.PopPubKey();
    if (key != ts.newPubKey)
        return state.Error(strprintf("unexpected key %s popped from user %s", HexStr(key.begin(), key.end()), user.GetRegTxId().ToString()));
    return true;
}

static bool Undo_CloseAccount(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    if (!user.IsClosed())
        return state.Error(strprintf("expected user %s to be closed", user.GetRegTxId().ToString()));
    user.SetClosed(false);
    return true;
}

bool CheckTransitionForUser(const CTransition &ts, const CEvoUser &user, bool checkSigs, CValidationState &state) {
    size_t tsSize = ::GetSerializeSize(ts, SER_DISK, CLIENT_VERSION);
    if (tsSize > EVO_TS_MAX_SIZE) {
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-size");
    }

    if (user.IsClosed()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-accountclosed");
    }

    // TODO min fee depending on TS size
    if (ts.nFee < EVO_TS_MIN_FEE || ts.nFee > EVO_TS_MAX_FEE) {
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-fee");
    }

    if (user.GetCreditBalance() < ts.nFee) {
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-nocredits");
    }

    if (ts.hashPrevTransition != user.GetLastTransition()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-ancestor");
    }

    if (checkSigs && !CheckTransitionSignatures(ts, user, state))
        return false;

    return true;
}

bool ProcessTransitionForUser(const CTransition &ts, CEvoUser &user, CValidationState &state) {
    switch (ts.action) {
        case Transition_UpdateData:
            if (!Process_UpdateData(ts, user, state))
                return false;
            break;
        case Transition_ResetKey:
            if (!Process_ResetKey(ts, user, state))
                return false;
            break;
        case Transition_CloseAccount:
            if (!Process_CloseAccount(ts, user, state))
                return false;
            break;
        default:
            return state.DoS(100, false, REJECT_INVALID, "bad-ts-action");
    }
    user.AddSpend(ts.nFee);
    return true;
}

template<typename RegTxIds>
static bool GetUsers(const RegTxIds &regTxIds, std::map<uint256, CEvoUser> &users) {
    bool anyError = false;
    for (const auto &regTxId : regTxIds) {
        if (users.count(regTxId))
            continue;
        auto it = users.emplace(regTxId, CEvoUser());
        CEvoUser &user = it.first->second;
        if (!evoUserDB->GetUser(regTxId, user)) {
            anyError = true;
            users.erase(it.first);
        }
    }
    return !anyError;
}

static bool GetUsersFromBlock(const CBlock &block, std::map<uint256, CEvoUser> &users) {
    std::set<uint256> regTxIds;
    for (const CTransition &ts : block.vts) {
        regTxIds.emplace(ts.hashRegTx);
    }
    return GetUsers(regTxIds, users);
}

static bool WriteUsers(const std::map<uint256, CEvoUser> &users, CValidationState &state) {
    for (auto &p : users) {
        const CEvoUser &user = p.second;
        if (!evoUserDB->WriteUser(user)) {
            return state.Error(strprintf("WriteUsers() -- failed to write user %s", user.GetRegTxId().ToString()));
        }
    }
    return true;
}

static bool ProcessTransitionsInBlockForUsers(const CBlock &block, std::map<uint256, CEvoUser> &users, CValidationState &state) {
    std::set<uint256> tsHashes;

    // duplication checks first
    for (int i = 0; i < block.vts.size(); i++) {
        const CTransition &ts = block.vts[i];

        // duplicate TS check
        if (tsHashes.count(ts.GetHash())) {
            return state.DoS(100, false, REJECT_INVALID, "bad-ts-dup");
        }
        tsHashes.insert(ts.GetHash());
    }

    for (int i = 0; i < block.vts.size(); i++) {
        const CTransition &ts = block.vts[i];
        CEvoUser &user = users[ts.hashRegTx];
        if (!CheckTransitionForUser(ts, user, true, state))
            return false;
        if (!ProcessTransitionForUser(ts, user, state))
            return false;
    }
    return true;
}

bool ProcessTransitionsInBlock(const CBlock &block, bool onlyCheck, CValidationState &state) {
    std::map<uint256, CEvoUser> users;

    // get all users first
    if (!GetUsersFromBlock(block, users))
        return state.DoS(100, false, REJECT_INVALID, "bad-ts-nouser");

    if (!ProcessTransitionsInBlockForUsers(block, users, state))
        return false;

    if (!onlyCheck) {
        for (const auto &ts : block.vts) {
            if (!evoUserDB->WriteTransition(ts)) {
                return error("ProcessTransitionsInBlock() -- WriteTransition failed: %s", ts.ToString());
            }
            if (!evoUserDB->WriteTransitionBlockHash(ts.GetHash(), block.GetHash())) {
                return error("ProcessTransitionsInBlock() -- WriteTransitionBlockHash failed: %s", ts.ToString());
            }
        }

        if (!WriteUsers(users, state))
            return false;
    }

    return true;
}

static bool UndoTransitionForUser(const CTransition &ts, CEvoUser &user, CValidationState &state) {

    switch (ts.action) {
        case Transition_UpdateData:
            if (!Undo_UpdateData(ts, user, state))
                return false;
            break;
        case Transition_ResetKey:
            if (!Undo_ResetKey(ts, user, state))
                return false;
            break;
        case Transition_CloseAccount:
            if (!Undo_CloseAccount(ts, user, state))
                return false;
            break;
        default:
            return state.Error(strprintf("UndoTransition() -- unexpected transition action %d", ts.action));
    }

    user.AddSpend(-ts.nFee);
    if (user.GetSpentCredits() < 0) {
        return state.Error("UndoTransition() -- Unexpected negative spent credits");
    }

    return true;
}

bool UndoTransitionsInBlock(const CBlock &block, CValidationState &state) {
    std::map<uint256, CEvoUser> users;
    if (!GetUsersFromBlock(block, users))
        return state.Error("GetUsersFromBlock() failed");

    // undo in reversed order
    for (int i = block.vts.size() - 1; i >= 0; i--) {
        const CTransition &ts = block.vts[i];
        if (!UndoTransitionForUser(ts, users[ts.hashRegTx], state))
            return false;

        if (!evoUserDB->DeleteTransition(ts.GetHash())) {
            return state.Error(strprintf("UndoTransitionsInBlock(): DeleteTransition failed for %s", ts.hashRegTx.ToString()));
        }
        if (!evoUserDB->DeleteTransitionBlockHash(ts.GetHash())) {
            return state.Error(strprintf("UndoTransitionsInBlock(): DeleteTransitionBlockHash failed for %s", ts.hashRegTx.ToString()));
        }

        if (!tsMempool.AddTransition(ts)) {
            LogPrintf("UndoTransitionsInBlock(): AddTransition for %s failed\n", ts.GetHash().ToString());
        }
    }

    if (!WriteUsers(users, state))
        return false;

    return true;
}