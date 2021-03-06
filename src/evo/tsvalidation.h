// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_TSVALIDATION_H
#define DASH_TSVALIDATION_H

#include "transition.h"
#include "validation.h"

class CEvoUser;
class CNode;

// TODO define good min/max fees
static const CAmount EVO_TS_MIN_FEE = 1000; // TODO find good min fee
static const CAmount EVO_TS_MAX_FEE = EVO_TS_MIN_FEE * 10; // TODO find good max fee
static const size_t EVO_TS_MAX_SIZE = 1500; // TODO find correct max size

bool CheckTransition(const CTransition &ts, bool checkSigs, bool includeMempool, CValidationState &state);

bool CheckTransitionForUser(const CTransition &ts, const CEvoUser &user, bool checkSigs, CValidationState &state);
bool ProcessTransitionForUser(const CTransition &ts, CEvoUser &user, CValidationState &state);
bool ProcessTransitionsInBlock(const CBlock &block, bool onlyCheck, CValidationState &state);
bool UndoTransitionsInBlock(const CBlock &block, CValidationState &state);

void RelayNowValidTransitions();
void HandleIncomingTransition(CNode *pfrom, const CTransition &ts);

bool BuildUserFromMempool(const uint256 &regTxId, CEvoUser &user);
bool TopupUserFromMempool(CEvoUser &user);
bool ApplyUserTransitionsFromMempool(CEvoUser &user, const uint256 &stopAtTs = uint256());

void AddMempoolTransitionsToBlock(CBlock &block, uint64_t maxTsSpace, uint64_t maxBlockSize);

CAmount CalcTransitionFeesForBlock(const CBlock &block);

#endif //DASH_TSVALIDATION_H
