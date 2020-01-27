// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governance.h"
#include "client.h"
#include "consensus/validation.h"
#include "governance-classes.h"
#include "governance-object.h"
#include "governance-validators.h"
#include "governance-vote.h"
#include "init.h"
#include "masternode-meta.h"
#include "masternode-sync.h"
#include "messagesigner.h"
#include "net_processing.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#include "spork.h"
#include "util.h"
#include "validation.h"
#include "validationinterface.h"

CGovernanceManager governance;

int nSubmittedFinalBudget;

const std::string CGovernanceManager::SERIALIZATION_VERSION_STRING = "CGovernanceManager-Version-15";
const int CGovernanceManager::MAX_TIME_FUTURE_DEVIATION = 60 * 60;
const int CGovernanceManager::RELIABLE_PROPAGATION_TIME = 60;

CGovernanceManager::CGovernanceManager() :
    nTimeLastDiff(0),
    nCachedBlockHeight(0),
    mapObjects(),
    mapErasedGovernanceObjects(),
    mapMasternodeOrphanObjects(),
    cmapVoteToObject(MAX_CACHE_SIZE),
    cmapInvalidVotes(MAX_CACHE_SIZE),
    cmmapOrphanVotes(MAX_CACHE_SIZE),
    mapLastMasternodeObject(),
    setRequestedObjects(),
    fRateChecksEnabled(true),
    cs()
{
}

// Accessors for thread-safe access to maps
bool CGovernanceManager::HaveObjectForHash(const uint256& nHash) const
{
    LOCK(cs);
    return (mapObjects.count(nHash) == 1 || mapPostponedObjects.count(nHash) == 1);
}

bool CGovernanceManager::SerializeObjectForHash(const uint256& nHash, CDataStream& ss) const
{
    LOCK(cs);
    object_m_cit it = mapObjects.find(nHash);
    if (it == mapObjects.end()) {
        it = mapPostponedObjects.find(nHash);
        if (it == mapPostponedObjects.end())
            return false;
    }
    ipfs::Client ipfsclient("localhost", 5001);
    ss << it->second;
    return true;
}

bool CGovernanceManager::HaveVoteForHash(const uint256& nHash) const
{
    LOCK(cs);

    CGovernanceObject* pGovobj = nullptr;
    return cmapVoteToObject.Get(nHash, pGovobj) && pGovobj->GetVoteFile().HasVote(nHash);
}

int CGovernanceManager::GetVoteCount() const
{
    LOCK(cs);
    return (int)cmapVoteToObject.GetSize();
}

bool CGovernanceManager::SerializeVoteForHash(const uint256& nHash, CDataStream& ss) const
{
    LOCK(cs);

    CGovernanceObject* pGovobj = nullptr;
    return cmapVoteToObject.Get(nHash, pGovobj) && pGovobj->GetVoteFile().SerializeVoteToStream(nHash, ss);
}

template<class UnaryFunction>
void CGovernanceManager::RecursiveIPFSIterate(const json& j, UnaryFunction f)
{
	for (auto it = j.begin(); it != j.end(); ++it)
	{
		if (it->is_structured())
		{
			RecursiveIPFSIterate(*it, f);
		}
		else
		{
			f(it);
		}
	}
}


void CGovernanceManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    // lite mode is not supported
    if (fLiteMode) return;
    if (!masternodeSync.IsBlockchainSynced()) return;

    // ANOTHER USER IS ASKING US TO HELP THEM SYNC GOVERNANCE OBJECT DATA
    if (strCommand == NetMsgType::MNGOVERNANCESYNC) {
        if (pfrom->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) {
            LogPrint("gobject", "MNGOVERNANCESYNC -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE, strprintf("Version must be %d or greater", MIN_GOVERNANCE_PEER_PROTO_VERSION)));
            return;
        }

        // Ignore such requests until we are fully synced.
        // We could start processing this after masternode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!masternodeSync.IsSynced()) return;

        uint256 nProp;
        CBloomFilter filter;

        vRecv >> nProp;

        if (pfrom->nVersion >= GOVERNANCE_FILTER_PROTO_VERSION) {
            vRecv >> filter;
            filter.UpdateEmptyFull();
        } else {
            filter.clear();
        }

        if (nProp == uint256()) {
            SyncObjects(pfrom, connman);
        } else {
            SyncSingleObjVotes(pfrom, nProp, filter, connman);
        }
        LogPrint("gobject", "MNGOVERNANCESYNC -- syncing governance objects to our peer at %s\n", pfrom->addr.ToString());
    }

    // A NEW GOVERNANCE OBJECT HAS ARRIVED
    else if (strCommand == NetMsgType::MNGOVERNANCEOBJECT) {
        // MAKE SURE WE HAVE A VALID REFERENCE TO THE TIP BEFORE CONTINUING

        CGovernanceObject govobj;
        vRecv >> govobj;

        uint256 nHash = govobj.GetHash();

        {
            LOCK(cs_main);
            connman.RemoveAskFor(nHash);
        }

        if (pfrom->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) {
            LogPrint("gobject", "MNGOVERNANCEOBJECT -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE, strprintf("Version must be %d or greater", MIN_GOVERNANCE_PEER_PROTO_VERSION)));
            return;
        }

        if (!masternodeSync.IsBlockchainSynced()) {
            LogPrint("gobject", "MNGOVERNANCEOBJECT -- masternode list not synced\n");
            return;
        }

        std::string strHash = nHash.ToString();

        LogPrint("gobject", "MNGOVERNANCEOBJECT -- Received object: %s\n", strHash);

        if (!AcceptObjectMessage(nHash)) {
            LogPrintf("MNGOVERNANCEOBJECT -- Received unrequested object: %s\n", strHash);
            return;
        }

        LOCK2(cs_main, cs);

        if (mapObjects.count(nHash) || mapPostponedObjects.count(nHash) ||
            mapErasedGovernanceObjects.count(nHash) || mapMasternodeOrphanObjects.count(nHash)) {
            // TODO - print error code? what if it's GOVOBJ_ERROR_IMMATURE?
            LogPrint("gobject", "MNGOVERNANCEOBJECT -- Received already seen object: %s\n", strHash);
            return;
        }

        bool fRateCheckBypassed = false;
        if (!MasternodeRateCheck(govobj, true, false, fRateCheckBypassed)) {
            LogPrintf("MNGOVERNANCEOBJECT -- masternode rate check failed - %s - (current block height %d) \n", strHash, nCachedBlockHeight);
            return;
        }

        std::string strError = "";
        // CHECK OBJECT AGAINST LOCAL BLOCKCHAIN

        bool fMasternodeMissing = false;
        bool fMissingConfirmations = false;
        bool fIsValid = govobj.IsValidLocally(strError, fMasternodeMissing, fMissingConfirmations, true);

        if (fRateCheckBypassed && (fIsValid || fMasternodeMissing)) {
            if (!MasternodeRateCheck(govobj, true)) {
                LogPrintf("MNGOVERNANCEOBJECT -- masternode rate check failed (after signature verification) - %s - (current block height %d)\n", strHash, nCachedBlockHeight);
                return;
            }
        }

        if (!fIsValid) {
            if (fMasternodeMissing) {
                int& count = mapMasternodeOrphanCounter[govobj.GetMasternodeOutpoint()];
                if (count >= 10) {
                    LogPrint("gobject", "MNGOVERNANCEOBJECT -- Too many orphan objects, missing masternode=%s\n", govobj.GetMasternodeOutpoint().ToStringShort());
                    // ask for this object again in 2 minutes
                    CInv inv(MSG_GOVERNANCE_OBJECT, govobj.GetHash());
                    pfrom->AskFor(inv);
                    return;
                }

                count++;
                ExpirationInfo info(pfrom->GetId(), GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME);
                mapMasternodeOrphanObjects.insert(std::make_pair(nHash, object_info_pair_t(govobj, info)));
                LogPrintf("MNGOVERNANCEOBJECT -- Missing masternode for: %s, strError = %s\n", strHash, strError);
            } else if (fMissingConfirmations) {
                if (ValidIPFSHash(govobj)) {
                    AddPostponedObject(govobj);
                    AddIPFSHash(govobj);
                    LogPrintf("MNGOVERNANCEOBJECT -- Not enough fee confirmations for: %s, strError = %s\n", strHash, strError);
                } else {
                    LogPrintf("MNGOVERNANCEOBJECT -- IPFS hash NOT valid\n");
                    return;
                }
            } else {
                LogPrintf("MNGOVERNANCEOBJECT -- Governance object is invalid - %s\n", strError);
                // apply node's ban score
                Misbehaving(pfrom->GetId(), 20);
            }

            return;
        }

        if(ValidIPFSHash(govobj)) {
            AddIPFSHash(govobj);
            AddGovernanceObject(govobj, connman, pfrom);
        } else {
            LogPrintf("MNGOVERNANCEOBJECT -- IPFS hash NOT valid\n");
            return;
        } 
    }

    // A NEW GOVERNANCE OBJECT VOTE HAS ARRIVED
    else if (strCommand == NetMsgType::MNGOVERNANCEOBJECTVOTE) {
        CGovernanceVote vote;
        vRecv >> vote;

        uint256 nHash = vote.GetHash();

        {
            LOCK(cs_main);
            connman.RemoveAskFor(nHash);
        }

        if (pfrom->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) {
            LogPrint("gobject", "MNGOVERNANCEOBJECTVOTE -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE, strprintf("Version must be %d or greater", MIN_GOVERNANCE_PEER_PROTO_VERSION)));
        }

        // Ignore such messages until masternode list is synced
        if (!masternodeSync.IsBlockchainSynced()) {
            LogPrint("gobject", "MNGOVERNANCEOBJECTVOTE -- masternode list not synced\n");
            return;
        }

        LogPrint("gobject", "MNGOVERNANCEOBJECTVOTE -- Received vote: %s\n", vote.ToString());

        std::string strHash = nHash.ToString();

        if (!AcceptVoteMessage(nHash)) {
            LogPrint("gobject", "MNGOVERNANCEOBJECTVOTE -- Received unrequested vote object: %s, hash: %s, peer = %d\n",
                vote.ToString(), strHash, pfrom->GetId());
            return;
        }

        CGovernanceException exception;
        if (ProcessVote(pfrom, vote, exception, connman)) {
            LogPrint("gobject", "MNGOVERNANCEOBJECTVOTE -- %s new\n", strHash);
            masternodeSync.BumpAssetLastTime("MNGOVERNANCEOBJECTVOTE");
            vote.Relay(connman);
        } else {
            LogPrint("gobject", "MNGOVERNANCEOBJECTVOTE -- Rejected vote, error = %s\n", exception.what());
            if ((exception.GetNodePenalty() != 0) && masternodeSync.IsSynced()) {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), exception.GetNodePenalty());
            }
            return;
        }
        // SEND NOTIFICATION TO SCRIPT/ZMQ
        GetMainSignals().NotifyGovernanceVote(vote);
    }
}

void CGovernanceManager::CheckOrphanVotes(CGovernanceObject& govobj, CGovernanceException& exception, CConnman& connman)
{
    uint256 nHash = govobj.GetHash();
    std::vector<vote_time_pair_t> vecVotePairs;
    cmmapOrphanVotes.GetAll(nHash, vecVotePairs);

    ScopedLockBool guard(cs, fRateChecksEnabled, false);

    int64_t nNow = GetAdjustedTime();
    for (auto& pairVote : vecVotePairs) {
        bool fRemove = false;
        CGovernanceVote& vote = pairVote.first;
        CGovernanceException exception;
        if (pairVote.second < nNow) {
            fRemove = true;
        } else if (govobj.ProcessVote(nullptr, vote, exception, connman)) {
            vote.Relay(connman);
            fRemove = true;
        }
        if (fRemove) {
            cmmapOrphanVotes.Erase(nHash, pairVote);
        }
    }
}

void CGovernanceManager::AddIPFSHash(CGovernanceObject& govobj)
{
    if (fMasternodeMode) {
        LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash -- Record Or Proposal Check\n");
        if (govobj.GetObjectType() == GOVERNANCE_OBJECT_RECORD || govobj.GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL) {
            LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash -- Record Or Proposal -- PASS\n");
            ipfs::Client ipfsclient("localhost", 5001);
            std::string ipfsHash = "empty";
            try {
                UniValue Jobj = govobj.GetJSONObject();
                ipfsHash = "/ipfs/" + Jobj["ipfscid"].get_str();
            } catch (std::exception& e) {
                LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash -- Could not get IPFS Hash: %s\n", ipfsHash);
            }
            //  if ((!govobj.IsSetCachedDelete() || !govobj.IsSetExpired()) && govobj.IsSetRecordLocked()) {
            LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash -- NameHash: %s\n", ipfsHash);

            bool IPFSSizeCheck = false;
            bool IPFSDaemonCheck = false;
            //PIN IPFS HASH
            try {
                ipfs::Json ls_result;
                ipfsclient.FilesLs(ipfsHash, &ls_result);
                IPFSDaemonCheck = true;
                int IPFSTempSize = 0;
                long long IPFSSize = 0;
                RecursiveIPFSIterate(ls_result,
                    [&IPFSTempSize, &IPFSSize](json::const_iterator it) {
                        if (it.key() == "Size") {
                            LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash::IPFSFileSizeCheck: %s %s\n", it.key(), it.value());
                            IPFSTempSize = it.value();
                            IPFSSize += IPFSTempSize;
                        }
                    });

                if (IPFSSize <= 10000000) {
                    IPFSSizeCheck = true;
                    LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash::IPFSFileSizeCheck -- Maxium Size: 10000000 bytes (10MB), Pass Size: %d bytes\n", IPFSSize);
                } else {
                    IPFSSizeCheck = false;
                    LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash::IPFSFileSizeCheck -- -- Maxium Size: 10000000 bytes (10MB), Fail Size Too Big: %d bytes\n", IPFSSize);
                }
            } catch (std::exception& e) {
                LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash::PinHash -- IPFS Hash: %s Is Not Valid IPFS object directory OR this masternode does not require IPFS pinning\n", ipfsHash);
            }

            // Weird error with IPFS daemon, exception is thrown even though IPFS hash has been or is pinning properly pinned. This is ugly work around.
            try {
                if (IPFSSizeCheck && IPFSDaemonCheck) {
                    LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash::PinHash -- IPFS Hash: %s Pin Attempt\n", ipfsHash);
                    ipfsclient.PinAdd(ipfsHash);
                }
            } catch (std::exception& e) {
                LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash::PinHash -- IPFS Pin Hash: %s Success, check on console by running 'ipfs pin ls %s'\n", ipfsHash, ipfsHash);
            }

        } else {
            LogPrintf("MNGOVERNANCEOBJECT::AddIPFShash -- RecordCheck -- FAIL: Not a record or proposal, ObjectType: %d \n", govobj.GetObjectType());
        }
    }
}

void CGovernanceManager::AddGovernanceObject(CGovernanceObject& govobj, CConnman& connman, CNode* pfrom)
{
    uint256 nHash = govobj.GetHash();
    std::string strHash = nHash.ToString();

    // UPDATE CACHED VARIABLES FOR THIS OBJECT AND ADD IT TO OUR MANANGED DATA

    govobj.UpdateSentinelVariables(); //this sets local vars in object

    LOCK2(cs_main, cs);
    std::string strError = "";

    // MAKE SURE THIS OBJECT IS OK

    if (!govobj.IsValidLocally(strError, true)) {
        LogPrintf("CGovernanceManager::AddGovernanceObject -- invalid governance object - %s - (nCachedBlockHeight %d) \n", strError, nCachedBlockHeight);
        return;
    }

    LogPrint("gobject", "CGovernanceManager::AddGovernanceObject -- Adding object: hash = %s, type = %d\n", nHash.ToString(), govobj.GetObjectType());

    // INSERT INTO OUR GOVERNANCE OBJECT MEMORY
    // IF WE HAVE THIS OBJECT ALREADY, WE DON'T WANT ANOTHER COPY
    auto objpair = mapObjects.emplace(nHash, govobj);

    if (!objpair.second) {
        LogPrintf("CGovernanceManager::AddGovernanceObject -- already have governance object %s\n", nHash.ToString());
        return;
    }

    // SHOULD WE ADD THIS OBJECT TO ANY OTHER MANANGERS?

    LogPrint("gobject", "CGovernanceManager::AddGovernanceObject -- Before trigger block, GetDataAsPlainString = %s, nObjectType = %d\n",
                govobj.GetDataAsPlainString(), govobj.nObjectType);

    if (govobj.nObjectType == GOVERNANCE_OBJECT_TRIGGER) {
        if (!triggerman.AddNewTrigger(nHash)) {
            LogPrint("gobject", "CGovernanceManager::AddGovernanceObject -- undo adding invalid trigger object: hash = %s\n", nHash.ToString());
            CGovernanceObject& objref = objpair.first->second;
            objref.fCachedDelete = true;
            if (objref.nDeletionTime == 0) {
                objref.nDeletionTime = GetAdjustedTime();
            }
            return;
        }
    }

    LogPrintf("CGovernanceManager::AddGovernanceObject -- %s new, received from %s\n", strHash, pfrom ? pfrom->GetAddrName() : "nullptr");
    govobj.Relay(connman);

    // Update the rate buffer
    MasternodeRateUpdate(govobj);

    masternodeSync.BumpAssetLastTime("CGovernanceManager::AddGovernanceObject");

    // WE MIGHT HAVE PENDING/ORPHAN VOTES FOR THIS OBJECT

    CGovernanceException exception;
    CheckOrphanVotes(govobj, exception, connman);

    // SEND NOTIFICATION TO SCRIPT/ZMQ
    GetMainSignals().NotifyGovernanceObject(govobj);
}

void CGovernanceManager::UpdateCachesAndClean()
{
    LogPrint("gobject", "CGovernanceManager::UpdateCachesAndClean\n");

    std::vector<uint256> vecDirtyHashes = mmetaman.GetAndClearDirtyGovernanceObjectHashes();

    LOCK2(cs_main, cs);

    for (const uint256& nHash : vecDirtyHashes) {
        object_m_it it = mapObjects.find(nHash);
        if (it == mapObjects.end()) {
            continue;
        }
        it->second.ClearMasternodeVotes();
        it->second.fDirtyCache = true;
    }

    ScopedLockBool guard(cs, fRateChecksEnabled, false);

    // Clean up any expired or invalid triggers
    triggerman.CleanAndRemove();

    object_m_it it = mapObjects.begin();
    int64_t nNow = GetAdjustedTime();

    while (it != mapObjects.end()) {
        CGovernanceObject* pObj = &((*it).second);

        if (!pObj) {
            ++it;
            continue;
        }

        uint256 nHash = it->first;
        std::string strHash = nHash.ToString();

        // IF CACHE IS NOT DIRTY, WHY DO THIS?
        if (pObj->IsSetDirtyCache()) {
            // UPDATE LOCAL VALIDITY AGAINST CRYPTO DATA
            pObj->UpdateLocalValidity();

            // UPDATE SENTINEL SIGNALING VARIABLES
            pObj->UpdateSentinelVariables();
        }

        // IF DELETE=TRUE, THEN CLEAN THE MESS UP!

        int64_t nTimeSinceDeletion = nNow - pObj->GetDeletionTime();

        LogPrint("gobject", "CGovernanceManager::UpdateCachesAndClean -- Checking object for deletion: %s, deletion time = %d, time since deletion = %d, delete flag = %d, expired flag = %d, record CacheLocked flag = %d, record PermLocked flag = %d\n",
            strHash, pObj->GetDeletionTime(), nTimeSinceDeletion, pObj->IsSetCachedDelete(), pObj->IsSetExpired(), pObj->IsSetRecordLocked(), pObj->IsSetPermLocked());

        if ((pObj->IsSetCachedDelete() || pObj->IsSetExpired()) && (!pObj->IsSetPermLocked() || !pObj->IsSetRecordLocked()) &&          
           (nTimeSinceDeletion >= GOVERNANCE_DELETION_DELAY)) {
            LogPrintf("CGovernanceManager::UpdateCachesAndClean -- erase obj %s\n", (*it).first.ToString());
            mmetaman.RemoveGovernanceObject(pObj->GetHash());

            //REMOVE IPFS HASH
            if(pObj->nObjectType == GOVERNANCE_OBJECT_RECORD){
                UniValue Jobj = pObj->GetJSONObject();
                std::string ipfsHash = Jobj["ipfscid"].get_str();
                try
                {		
                    ipfs::Client ipfsclient("localhost", 5001);
                    if ((pObj->IsSetCachedDelete() || pObj->IsSetExpired()) && (!pObj->IsSetRecordLocked() || !pObj->IsSetPermLocked())) {
                        //Remove IPFS PIN
                        ipfsclient.PinRm(ipfsHash, ipfs::Client::PinRmOptions::RECURSIVE);
                        LogPrintf("CGovernanceManager::RemoveIPFShash -- IPFS Hash: %s\n", ipfsHash);
                    }
                 }
                 catch(std::exception& e)
                 {
                    LogPrintf("MNGOVERNANCEOBJECT::RemoveIPFShash::PinHash -- IPFS Hash: %s Is Not Valid IPFS object directory\n", ipfsHash);
                 }
            }

            // Remove vote references
            const object_ref_cm_t::list_t& listItems = cmapVoteToObject.GetItemList();
            object_ref_cm_t::list_cit lit = listItems.begin();
            while (lit != listItems.end()) {
                if (lit->value == pObj) {
                    uint256 nKey = lit->key;
                    ++lit;
                    cmapVoteToObject.Erase(nKey);
                } else {
                    ++lit;
                }
            }

            int64_t nTimeExpired{0};

            if (pObj->GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL || pObj->GetObjectType() == GOVERNANCE_OBJECT_RECORD) {
                // keep hashes of deleted proposals or records forever
                nTimeExpired = std::numeric_limits<int64_t>::max();
            } else {
                int64_t nSuperblockCycleSeconds = Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;
                nTimeExpired = pObj->GetCreationTime() + 2 * nSuperblockCycleSeconds + GOVERNANCE_DELETION_DELAY;
            }

            mapErasedGovernanceObjects.insert(std::make_pair(nHash, nTimeExpired));
            mapObjects.erase(it++);
        } else {
            // NOTE: triggers are handled via triggerman
            if (pObj->GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL || (pObj->GetObjectType() == GOVERNANCE_OBJECT_RECORD && (!pObj->IsSetRecordLocked() || !pObj->IsSetPermLocked()))) {
                CProposalValidator validator(pObj->GetDataAsHexString(), true);
                if (!validator.Validate()) {
                    LogPrintf("CGovernanceManager::UpdateCachesAndClean -- set for deletion expired obj %s\n", strHash);
                    pObj->fCachedDelete = true;
                    if (pObj->nDeletionTime == 0) {
                        pObj->nDeletionTime = nNow;
                    }
                }
            }
            ++it;
        }
    }

    // forget about expired deleted objects
    hash_time_m_it s_it = mapErasedGovernanceObjects.begin();
    while (s_it != mapErasedGovernanceObjects.end()) {
        if (s_it->second < nNow) {
            mapErasedGovernanceObjects.erase(s_it++);
        } else {
            ++s_it;
        }
    }

    LogPrintf("CGovernanceManager::UpdateCachesAndClean -- %s\n", ToString());
}

CGovernanceObject* CGovernanceManager::FindGovernanceObject(const uint256& nHash)
{
    LOCK(cs);

    if (mapObjects.count(nHash)) return &mapObjects[nHash];

    return nullptr;
}

std::vector<CGovernanceVote> CGovernanceManager::GetCurrentVotes(const uint256& nParentHash, const COutPoint& mnCollateralOutpointFilter) const
{
    LOCK(cs);
    std::vector<CGovernanceVote> vecResult;

    // Find the governance object or short-circuit.
    object_m_cit it = mapObjects.find(nParentHash);
    if (it == mapObjects.end()) return vecResult;
    const CGovernanceObject& govobj = it->second;

    auto mnList = deterministicMNManager->GetListAtChainTip();
    std::map<COutPoint, CDeterministicMNCPtr> mapMasternodes;
    if (mnCollateralOutpointFilter.IsNull()) {
        mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
            mapMasternodes.emplace(dmn->collateralOutpoint, dmn);
        });
    } else {
        auto dmn = mnList.GetMNByCollateral(mnCollateralOutpointFilter);
        if (dmn) {
            mapMasternodes.emplace(dmn->collateralOutpoint, dmn);
        }
    }

    // Loop thru each MN collateral outpoint and get the votes for the `nParentHash` governance object
    for (const auto& mnpair : mapMasternodes) {
        // get a vote_rec_t from the govobj
        vote_rec_t voteRecord;
        if (!govobj.GetCurrentMNVotes(mnpair.first, voteRecord)) continue;

        for (const auto& voteInstancePair : voteRecord.mapInstances) {
            int signal = voteInstancePair.first;
            int outcome = voteInstancePair.second.eOutcome;
            int64_t nCreationTime = voteInstancePair.second.nCreationTime;

            CGovernanceVote vote = CGovernanceVote(mnpair.first, nParentHash, (vote_signal_enum_t)signal, (vote_outcome_enum_t)outcome);
            vote.SetTime(nCreationTime);

            vecResult.push_back(vote);
        }
    }

    return vecResult;
}

std::vector<const CGovernanceObject*> CGovernanceManager::GetAllNewerThan(int64_t nMoreThanTime) const
{
    LOCK(cs);

    std::vector<const CGovernanceObject*> vGovObjs;

    for (const auto& objPair : mapObjects) {
        // IF THIS OBJECT IS OLDER THAN TIME, CONTINUE
        if (objPair.second.GetCreationTime() < nMoreThanTime) {
            continue;
        }

        // ADD GOVERNANCE OBJECT TO LIST
        const CGovernanceObject* pGovObj = &(objPair.second);
        vGovObjs.push_back(pGovObj);
    }

    return vGovObjs;
}

//
// Sort by votes, if there's a tie sort by their feeHash TX
//
struct sortProposalsByVotes {
    bool operator()(const std::pair<CGovernanceObject*, int>& left, const std::pair<CGovernanceObject*, int>& right)
    {
        if (left.second != right.second) return (left.second > right.second);
        return (UintToArith256(left.first->GetCollateralHash()) > UintToArith256(right.first->GetCollateralHash()));
    }
};

void CGovernanceManager::DoMaintenance(CConnman& connman)
{
    if (fLiteMode || !masternodeSync.IsSynced() || ShutdownRequested()) return;

    // CHECK OBJECTS WE'VE ASKED FOR, REMOVE OLD ENTRIES

    CleanOrphanObjects();

    RequestOrphanObjects(connman);

    // CHECK AND REMOVE - REPROCESS GOVERNANCE OBJECTS

    UpdateCachesAndClean();
}

bool CGovernanceManager::ConfirmInventoryRequest(const CInv& inv)
{
    // do not request objects until it's time to sync
    if (!masternodeSync.IsBlockchainSynced()) return false;

    LOCK(cs);

    LogPrint("gobject", "CGovernanceManager::ConfirmInventoryRequest inv = %s\n", inv.ToString());

    // First check if we've already recorded this object
    switch (inv.type) {
    case MSG_GOVERNANCE_OBJECT: {
        if (mapObjects.count(inv.hash) == 1 || mapPostponedObjects.count(inv.hash) == 1) {
            LogPrint("gobject", "CGovernanceManager::ConfirmInventoryRequest already have governance object, returning false\n");
            return false;
        }
        break;
    } 
    case MSG_GOVERNANCE_OBJECT_VOTE: {
        if (cmapVoteToObject.HasKey(inv.hash)) {
            LogPrint("gobject", "CGovernanceManager::ConfirmInventoryRequest already have governance vote, returning false\n");
            return false;
        }
        break;
    } 
    default:
        LogPrint("gobject", "CGovernanceManager::ConfirmInventoryRequest unknown type, returning false\n");
        return false;
    }


    hash_s_t* setHash = nullptr;
    switch (inv.type) {
    case MSG_GOVERNANCE_OBJECT:
        setHash = &setRequestedObjects;
        break;
    case MSG_GOVERNANCE_OBJECT_VOTE:
        setHash = &setRequestedVotes;
        break;
    default:
        return false;
    }

    hash_s_cit it = setHash->find(inv.hash);
    if (it == setHash->end()) {
        setHash->insert(inv.hash);
        LogPrint("gobject", "CGovernanceManager::ConfirmInventoryRequest added inv to requested set\n");
    }

    LogPrint("gobject", "CGovernanceManager::ConfirmInventoryRequest reached end, returning true\n");
    return true;
}

void CGovernanceManager::SyncSingleObjVotes(CNode* pnode, const uint256& nProp, const CBloomFilter& filter, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!masternodeSync.IsSynced()) return;

    int nVoteCount = 0;

    // SYNC GOVERNANCE OBJECTS WITH OTHER CLIENT

    LogPrint("gobject", "CGovernanceManager::%s -- syncing single object to peer=%d, nProp = %s\n", __func__, pnode->id, nProp.ToString());

    LOCK2(cs_main, cs);

    // single valid object and its valid votes
    object_m_it it = mapObjects.find(nProp);
    if (it == mapObjects.end()) {
        LogPrint("gobject", "CGovernanceManager::%s -- no matching object for hash %s, peer=%d\n", __func__, nProp.ToString(), pnode->id);
        return;
    }
    CGovernanceObject& govobj = it->second;
    std::string strHash = it->first.ToString();

    LogPrint("gobject", "CGovernanceManager::%s -- attempting to sync govobj: %s, peer=%d\n", __func__, strHash, pnode->id);

    if ((govobj.IsSetCachedDelete() || govobj.IsSetExpired()) && govobj.GetObjectType() != GOVERNANCE_OBJECT_RECORD) {
        LogPrintf("CGovernanceManager::%s -- not syncing deleted/expired govobj: %s, peer=%d\n", __func__,
            strHash, pnode->id);
        return;
    }

    auto fileVotes = govobj.GetVoteFile();

    for (const auto& vote : fileVotes.GetVotes()) {
        uint256 nVoteHash = vote.GetHash();

        bool onlyVotingKeyAllowed = false;
        if (govobj.GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL || govobj.GetObjectType() == GOVERNANCE_OBJECT_RECORD) {
            if (vote.GetSignal() == VOTE_SIGNAL_FUNDING) {
                onlyVotingKeyAllowed = true;
            }
        }

        if (filter.contains(nVoteHash) || !vote.IsValid(onlyVotingKeyAllowed)) {
             continue;
        }
        pnode->PushInventory(CInv(MSG_GOVERNANCE_OBJECT_VOTE, nVoteHash));
        ++nVoteCount;
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_GOVOBJ_VOTE, nVoteCount));
    LogPrintf("CGovernanceManager::%s -- sent %d votes to peer=%d\n", __func__, nVoteCount, pnode->id);
}

void CGovernanceManager::SyncObjects(CNode* pnode, CConnman& connman) const
{
    // do not provide any data until our node is synced
    if (!masternodeSync.IsSynced()) return;

    if (netfulfilledman.HasFulfilledRequest(pnode->addr, NetMsgType::MNGOVERNANCESYNC)) {
        LOCK(cs_main);
        // Asking for the whole list multiple times in a short period of time is no good
        LogPrint("gobject", "CGovernanceManager::%s -- peer already asked me for the list\n", __func__);
        Misbehaving(pnode->GetId(), 20);
        return;
    }
    netfulfilledman.AddFulfilledRequest(pnode->addr, NetMsgType::MNGOVERNANCESYNC);

    int nObjCount = 0;

    // SYNC GOVERNANCE OBJECTS WITH OTHER CLIENT

    LogPrint("gobject", "CGovernanceManager::%s -- syncing all objects to peer=%d\n", __func__, pnode->id);

    LOCK2(cs_main, cs);

    // all valid objects, no votes
    for (const auto& objPair : mapObjects) {
        uint256 nHash = objPair.first;
        const CGovernanceObject& govobj = objPair.second;
        std::string strHash = nHash.ToString();

        LogPrint("gobject", "CGovernanceManager::%s -- attempting to sync govobj: %s, peer=%d\n", __func__, strHash, pnode->id);

        if ((govobj.IsSetCachedDelete() || govobj.IsSetExpired()) && govobj.GetObjectType() != GOVERNANCE_OBJECT_RECORD) {
            LogPrintf("CGovernanceManager::%s -- not syncing deleted/expired govobj: %s, peer=%d\n", __func__,
                strHash, pnode->id);
            continue;
        }

        // Push the inventory budget proposal message over to the other client
        LogPrint("gobject", "CGovernanceManager::%s -- syncing govobj: %s, peer=%d\n", __func__, strHash, pnode->id);
        pnode->PushInventory(CInv(MSG_GOVERNANCE_OBJECT, nHash));
        ++nObjCount;
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_GOVOBJ, nObjCount));
    LogPrintf("CGovernanceManager::%s -- sent %d objects to peer=%d\n", __func__, nObjCount, pnode->id);
}

void CGovernanceManager::MasternodeRateUpdate(const CGovernanceObject& govobj)
{
    if (govobj.GetObjectType() != GOVERNANCE_OBJECT_TRIGGER) return;

    const COutPoint& masternodeOutpoint = govobj.GetMasternodeOutpoint();
    txout_m_it it = mapLastMasternodeObject.find(masternodeOutpoint);

    if (it == mapLastMasternodeObject.end()) {
        it = mapLastMasternodeObject.insert(txout_m_t::value_type(masternodeOutpoint, last_object_rec(true))).first;
    }

    int64_t nTimestamp = govobj.GetCreationTime();
    it->second.triggerBuffer.AddTimestamp(nTimestamp);

    if (nTimestamp > GetTime() + MAX_TIME_FUTURE_DEVIATION - RELIABLE_PROPAGATION_TIME) {
        // schedule additional relay for the object
        setAdditionalRelayObjects.insert(govobj.GetHash());
    }

    it->second.fStatusOK = true;
}

bool CGovernanceManager::MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus)
{
    bool fRateCheckBypassed;
    return MasternodeRateCheck(govobj, fUpdateFailStatus, true, fRateCheckBypassed);
}

bool CGovernanceManager::MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus, bool fForce, bool& fRateCheckBypassed)
{
    LOCK(cs);

    fRateCheckBypassed = false;

    if (!masternodeSync.IsSynced() || !fRateChecksEnabled) {
        return true;
    }

    if (govobj.GetObjectType() != GOVERNANCE_OBJECT_TRIGGER) {
        return true;
    }

    const COutPoint& masternodeOutpoint = govobj.GetMasternodeOutpoint();
    int64_t nTimestamp = govobj.GetCreationTime();
    int64_t nNow = GetAdjustedTime();
    int64_t nSuperblockCycleSeconds = Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;

    std::string strHash = govobj.GetHash().ToString();

    if (nTimestamp < nNow - 2 * nSuperblockCycleSeconds) {
        LogPrintf("CGovernanceManager::MasternodeRateCheck -- object %s rejected due to too old timestamp, masternode = %s, timestamp = %d, current time = %d\n",
            strHash, masternodeOutpoint.ToStringShort(), nTimestamp, nNow);
        return false;
    }

    if (nTimestamp > nNow + MAX_TIME_FUTURE_DEVIATION) {
        LogPrintf("CGovernanceManager::MasternodeRateCheck -- object %s rejected due to too new (future) timestamp, masternode = %s, timestamp = %d, current time = %d\n",
            strHash, masternodeOutpoint.ToStringShort(), nTimestamp, nNow);
        return false;
    }

    txout_m_it it = mapLastMasternodeObject.find(masternodeOutpoint);
    if (it == mapLastMasternodeObject.end()) return true;

    if (it->second.fStatusOK && !fForce) {
        fRateCheckBypassed = true;
        return true;
    }

    // Allow 1 trigger per mn per cycle, with a small fudge factor
    double dMaxRate = 2 * 1.1 / double(nSuperblockCycleSeconds);

    // Temporary copy to check rate after new timestamp is added
    CRateCheckBuffer buffer = it->second.triggerBuffer;

    buffer.AddTimestamp(nTimestamp);
    double dRate = buffer.GetRate();

    if (dRate < dMaxRate) {
        return true;
    }

    LogPrintf("CGovernanceManager::MasternodeRateCheck -- Rate too high: object hash = %s, masternode = %s, object timestamp = %d, rate = %f, max rate = %f\n",
        strHash, masternodeOutpoint.ToStringShort(), nTimestamp, dRate, dMaxRate);

    if (fUpdateFailStatus) {
        it->second.fStatusOK = false;
    }

    return false;
}

bool CGovernanceManager::ProcessVote(CNode* pfrom, const CGovernanceVote& vote, CGovernanceException& exception, CConnman& connman)
{
    ENTER_CRITICAL_SECTION(cs);
    uint256 nHashVote = vote.GetHash();
    uint256 nHashGovobj = vote.GetParentHash();

    if (cmapVoteToObject.HasKey(nHashVote)) {
        LogPrint("gobject", "CGovernanceObject::ProcessVote -- skipping known valid vote %s for object %s\n", nHashVote.ToString(), nHashGovobj.ToString());
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    if (cmapInvalidVotes.HasKey(nHashVote)) {
        std::ostringstream ostr;
        ostr << "CGovernanceManager::ProcessVote -- Old invalid vote "
             << ", MN outpoint = " << vote.GetMasternodeOutpoint().ToStringShort()
             << ", governance object hash = " << nHashGovobj.ToString();
        LogPrintf("%s\n", ostr.str());
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_PERMANENT_ERROR, 20);
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    object_m_it it = mapObjects.find(nHashGovobj);
    if (it == mapObjects.end()) {
        std::ostringstream ostr;
        ostr << "CGovernanceManager::ProcessVote -- Unknown parent object " << nHashGovobj.ToString()
             << ", MN outpoint = " << vote.GetMasternodeOutpoint().ToStringShort();
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_WARNING);
        if (cmmapOrphanVotes.Insert(nHashGovobj, vote_time_pair_t(vote, GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME))) {
            LEAVE_CRITICAL_SECTION(cs);
            RequestGovernanceObject(pfrom, nHashGovobj, connman);
            LogPrintf("%s\n", ostr.str());
            return false;
        }

        LogPrint("gobject", "%s\n", ostr.str());
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    CGovernanceObject& govobj = it->second;
    //Check to see if vote for record are past voting period and should be dropped
    if (govobj.nObjectType == GOVERNANCE_OBJECT_RECORD) {
 
        LogPrint("gobject", "CGovernanceObject::ProcessVote -- EpochTime of Vote = %d\n", vote.GetTimestamp());
        // Check if epochtime of votes is past epoch time of superblock for records
        int govObjSuperBlockHeight = govobj.GetCollateralNextSuperBlock();
        if (govObjSuperBlockHeight < 0 || govObjSuperBlockHeight > chainActive.Height()) {
            LogPrint("gobject", "CGovernanceObject::ProcessVote -- Block height out of rangehash = %s\n", govObjSuperBlockHeight);
        } else {
            CBlockIndex* pblockindex = chainActive[govObjSuperBlockHeight];
            LogPrint("gobject", "CGovernanceObject::ProcessVote -- Super/Block time= %s, %s\n", pblockindex->GetBlockTime(), vote.GetTimestamp());
        
            // Drop if it is, allow if it isn't.
            if (vote.GetTimestamp() < pblockindex->GetBlockTime()) {
                LogPrint("gobject", "CGovernanceObject::ProcessVote -- submission vote for record VoteTimeStamp %s < SuperblockTimeStamp %s\n", vote.GetTimestamp(), pblockindex->GetBlockTime());
            } else {
                LogPrint("gobject", "CGovernanceObject::ProcessVote -- ignoring of vote for record VoteTimeStamp %s > SuperblockTimeStamp %s\n", vote.GetTimestamp(), pblockindex->GetBlockTime());
                LEAVE_CRITICAL_SECTION(cs);
                return false;
            }
        }
    } else {
        //Process all other types of government objects
        if (govobj.IsSetCachedDelete() || govobj.IsSetExpired()) {
            LogPrint("gobject", "CGovernanceObject::ProcessVote -- ignoring vote for expired or deleted object, hash = %s\n", nHashGovobj.ToString());
            LEAVE_CRITICAL_SECTION(cs);
            return false;
        }
    }

    bool fOk = govobj.ProcessVote(pfrom, vote, exception, connman) && cmapVoteToObject.Insert(nHashVote, &govobj);
    LEAVE_CRITICAL_SECTION(cs);
    return fOk;
}

void CGovernanceManager::CheckMasternodeOrphanVotes(CConnman& connman)
{
    LOCK2(cs_main, cs);

    ScopedLockBool guard(cs, fRateChecksEnabled, false);

    for (auto& objPair : mapObjects) {
        objPair.second.CheckOrphanVotes(connman);
    }
}

void CGovernanceManager::CheckMasternodeOrphanObjects(CConnman& connman)
{
    LOCK2(cs_main, cs);
    int64_t nNow = GetAdjustedTime();
    ScopedLockBool guard(cs, fRateChecksEnabled, false);
    object_info_m_it it = mapMasternodeOrphanObjects.begin();
    while (it != mapMasternodeOrphanObjects.end()) {
        object_info_pair_t& pair = it->second;
        CGovernanceObject& govobj = pair.first;

        if (pair.second.nExpirationTime >= nNow) {
            std::string strError;
            bool fMasternodeMissing = false;
            bool fConfirmationsMissing = false;
            bool fIsValid = govobj.IsValidLocally(strError, fMasternodeMissing, fConfirmationsMissing, true);

            if (fIsValid) {
                AddGovernanceObject(govobj, connman);
                AddIPFSHash(govobj);
            } else if (fMasternodeMissing) {
                ++it;
                continue;
            }
        } else {
            // apply node's ban score
            Misbehaving(pair.second.idFrom, 20);
        }

        auto it_count = mapMasternodeOrphanCounter.find(govobj.GetMasternodeOutpoint());
        if (--it_count->second == 0)
            mapMasternodeOrphanCounter.erase(it_count);

        mapMasternodeOrphanObjects.erase(it++);
    }
}

void CGovernanceManager::CheckPostponedObjects(CConnman& connman)
{
    if (!masternodeSync.IsSynced()) return;

    LOCK2(cs_main, cs);

    // Check postponed proposals
    for (object_m_it it = mapPostponedObjects.begin(); it != mapPostponedObjects.end();) {
        const uint256& nHash = it->first;
        CGovernanceObject& govobj = it->second;

        assert(govobj.GetObjectType() != GOVERNANCE_OBJECT_TRIGGER);

        std::string strError;
        bool fMissingConfirmations;
        if (govobj.IsCollateralValid(strError, fMissingConfirmations)) {
            if (govobj.IsValidLocally(strError, false)) {
                AddGovernanceObject(govobj, connman);
                AddIPFSHash(govobj);
            } else {
                LogPrintf("CGovernanceManager::CheckPostponedObjects -- %s invalid\n", nHash.ToString());
            }

        } else if (fMissingConfirmations) {
            // wait for more confirmations
            ++it;
            continue;
        }

        // remove processed or invalid object from the queue
        mapPostponedObjects.erase(it++);
    }


    // Perform additional relays for triggers
    int64_t nNow = GetAdjustedTime();
    int64_t nSuperblockCycleSeconds = Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;

    for (hash_s_it it = setAdditionalRelayObjects.begin(); it != setAdditionalRelayObjects.end();) {
        object_m_it itObject = mapObjects.find(*it);
        if (itObject != mapObjects.end()) {
            CGovernanceObject& govobj = itObject->second;

            int64_t nTimestamp = govobj.GetCreationTime();

            bool fValid = (nTimestamp <= nNow + MAX_TIME_FUTURE_DEVIATION) && (nTimestamp >= nNow - 2 * nSuperblockCycleSeconds);
            bool fReady = (nTimestamp <= nNow + MAX_TIME_FUTURE_DEVIATION - RELIABLE_PROPAGATION_TIME);

            if (fValid) {
                if (fReady) {
                    LogPrintf("CGovernanceManager::CheckPostponedObjects -- additional relay: hash = %s\n", govobj.GetHash().ToString());
                    govobj.Relay(connman);
                } else {
                    it++;
                    continue;
                }
            }

        } else {
            LogPrintf("CGovernanceManager::CheckPostponedObjects -- additional relay of unknown object: %s\n", it->ToString());
        }

        setAdditionalRelayObjects.erase(it++);
    }
}

void CGovernanceManager::RequestGovernanceObject(CNode* pfrom, const uint256& nHash, CConnman& connman, bool fUseFilter)
{
    if (!pfrom) {
        return;
    }

    LogPrint("gobject", "CGovernanceManager::RequestGovernanceObject -- nHash %s peer=%d\n", nHash.ToString(), pfrom->GetId());

    CNetMsgMaker msgMaker(pfrom->GetSendVersion());

    if (pfrom->nVersion < GOVERNANCE_FILTER_PROTO_VERSION) {
        connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::MNGOVERNANCESYNC, nHash));
        return;
    }

    CBloomFilter filter;
    filter.clear();

    int nVoteCount = 0;
    if (fUseFilter) {
        LOCK(cs);
        CGovernanceObject* pObj = FindGovernanceObject(nHash);

        if (pObj) {
            filter = CBloomFilter(Params().GetConsensus().nGovernanceFilterElements, GOVERNANCE_FILTER_FP_RATE, GetRandInt(999999), BLOOM_UPDATE_ALL);
            std::vector<CGovernanceVote> vecVotes = pObj->GetVoteFile().GetVotes();
            nVoteCount = vecVotes.size();
            for (const auto& vote : vecVotes) {
                filter.insert(vote.GetHash());
            }
        }
    }

    LogPrint("gobject", "CGovernanceManager::RequestGovernanceObject -- nHash %s nVoteCount %d peer=%d\n", nHash.ToString(), nVoteCount, pfrom->id);
    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::MNGOVERNANCESYNC, nHash, filter));
}

int CGovernanceManager::RequestGovernanceObjectVotes(CNode* pnode, CConnman& connman)
{
    if (pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) return -3;
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(pnode);
    return RequestGovernanceObjectVotes(vNodesCopy, connman);
}

int CGovernanceManager::RequestGovernanceObjectVotes(const std::vector<CNode*>& vNodesCopy, CConnman& connman)
{
    static std::map<uint256, std::map<CService, int64_t> > mapAskedRecently;

    if (vNodesCopy.empty()) return -1;

    int64_t nNow = GetTime();
    int nTimeout = 60 * 60;
    size_t nPeersPerHashMax = 3;

    std::vector<uint256> vTriggerObjHashes;
    std::vector<uint256> vOtherObjHashes;

    // This should help us to get some idea about an impact this can bring once deployed on mainnet.
    // Testnet is ~40 times smaller in masternode count, but only ~1000 masternodes usually vote,
    // so 1 obj on mainnet == ~10 objs or ~1000 votes on testnet. However we want to test a higher
    // number of votes to make sure it's robust enough, so aim at 2000 votes per masternode per request.
    // On mainnet nMaxObjRequestsPerNode is always set to 1.
    int nMaxObjRequestsPerNode = 1;
    size_t nProjectedVotes = 2000;
    if (Params().NetworkIDString() != CBaseChainParams::MAIN) {
        nMaxObjRequestsPerNode = std::max(1, int(nProjectedVotes / std::max(1, (int)deterministicMNManager->GetListAtChainTip().GetValidMNsCount())));
    }

    {
        LOCK2(cs_main, cs);

        if (mapObjects.empty()) return -2;

        for (const auto& objPair : mapObjects) {
            uint256 nHash = objPair.first;
            if (mapAskedRecently.count(nHash)) {
                auto it = mapAskedRecently[nHash].begin();
                while (it != mapAskedRecently[nHash].end()) {
                    if (it->second < nNow) {
                        mapAskedRecently[nHash].erase(it++);
                    } else {
                        ++it;
                    }
                }
                if (mapAskedRecently[nHash].size() >= nPeersPerHashMax) continue;
            }

            if (objPair.second.nObjectType == GOVERNANCE_OBJECT_TRIGGER) {
                vTriggerObjHashes.push_back(nHash);
            } else {
                vOtherObjHashes.push_back(nHash);
            }
        }
    }

    LogPrint("gobject", "CGovernanceManager::RequestGovernanceObjectVotes -- start: vTriggerObjHashes %d vOtherObjHashes %d mapAskedRecently %d\n",
        vTriggerObjHashes.size(), vOtherObjHashes.size(), mapAskedRecently.size());

    FastRandomContext insecure_rand;
    std::random_shuffle(vTriggerObjHashes.begin(), vTriggerObjHashes.end(), insecure_rand);
    std::random_shuffle(vOtherObjHashes.begin(), vOtherObjHashes.end(), insecure_rand);

    for (int i = 0; i < nMaxObjRequestsPerNode; ++i) {
        uint256 nHashGovobj;

        // ask for triggers first
        if (vTriggerObjHashes.size()) {
            nHashGovobj = vTriggerObjHashes.back();
        } else {
            if (vOtherObjHashes.empty()) break;
            nHashGovobj = vOtherObjHashes.back();
        }
        bool fAsked = false;
        for (const auto& pnode : vNodesCopy) {
            // Only use regular peers, don't try to ask from outbound "masternode" connections -
            // they stay connected for a short period of time and it's possible that we won't get everything we should.
            // Only use outbound connections - inbound connection could be a "masternode" connection
            // initiated from another node, so skip it too.
            if (pnode->fMasternode || (fMasternodeMode && pnode->fInbound)) continue;
            // only use up to date peers
            if (pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) continue;
            // stop early to prevent setAskFor overflow
            {
                LOCK(cs_main);
                size_t nProjectedSize = pnode->setAskFor.size() + nProjectedVotes;
                if (nProjectedSize > SETASKFOR_MAX_SZ / 2) continue;
                // to early to ask the same node
                if (mapAskedRecently[nHashGovobj].count(pnode->addr)) continue;
            }

            RequestGovernanceObject(pnode, nHashGovobj, connman, true);
            mapAskedRecently[nHashGovobj][pnode->addr] = nNow + nTimeout;
            fAsked = true;
            // stop loop if max number of peers per obj was asked
            if (mapAskedRecently[nHashGovobj].size() >= nPeersPerHashMax) break;
        }
        // NOTE: this should match `if` above (the one before `while`)
        if (vTriggerObjHashes.size()) {
            vTriggerObjHashes.pop_back();
        } else {
            vOtherObjHashes.pop_back();
        }
        if (!fAsked) i--;
    }
    LogPrint("gobject", "CGovernanceManager::RequestGovernanceObjectVotes -- end: vTriggerObjHashes %d vOtherObjHashes %d mapAskedRecently %d\n",
        vTriggerObjHashes.size(), vOtherObjHashes.size(), mapAskedRecently.size());

    return int(vTriggerObjHashes.size() + vOtherObjHashes.size());
}

bool CGovernanceManager::AcceptObjectMessage(const uint256& nHash)
{
    LOCK(cs);
    return AcceptMessage(nHash, setRequestedObjects);
}

bool CGovernanceManager::AcceptVoteMessage(const uint256& nHash)
{
    LOCK(cs);
    return AcceptMessage(nHash, setRequestedVotes);
}

bool CGovernanceManager::AcceptMessage(const uint256& nHash, hash_s_t& setHash)
{
    hash_s_it it = setHash.find(nHash);
    if (it == setHash.end()) {
        // We never requested this
        return false;
    }
    // Only accept one response
    setHash.erase(it);
    return true;
}

void CGovernanceManager::RebuildIndexes()
{
    LOCK(cs);

    cmapVoteToObject.Clear();
    for (auto& objPair : mapObjects) {
        CGovernanceObject& govobj = objPair.second;
        std::vector<CGovernanceVote> vecVotes = govobj.GetVoteFile().GetVotes();
        for (size_t i = 0; i < vecVotes.size(); ++i) {
            cmapVoteToObject.Insert(vecVotes[i].GetHash(), &govobj);
        }
    }
}

void CGovernanceManager::AddCachedTriggers()
{
    LOCK(cs);

    for (auto& objpair : mapObjects) {
        CGovernanceObject& govobj = objpair.second;

        if (govobj.nObjectType != GOVERNANCE_OBJECT_TRIGGER) {
            continue;
        }

        if (!triggerman.AddNewTrigger(govobj.GetHash())) {
            govobj.fCachedDelete = true;
            if (govobj.nDeletionTime == 0) {
                govobj.nDeletionTime = GetAdjustedTime();
            }
        }
    }
}

void CGovernanceManager::InitOnLoad()
{
    LOCK(cs);
    int64_t nStart = GetTimeMillis();
    LogPrintf("Preparing masternode indexes and governance triggers...\n");
    RebuildIndexes();
    AddCachedTriggers();
    LogPrintf("Masternode indexes and governance triggers prepared  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("     %s\n", ToString());
}

std::string CGovernanceManager::ToString() const
{
    LOCK(cs);

    int nProposalCount = 0;
    int nTriggerCount = 0;
    int nOtherCount = 0;
    int nRecordCount = 0;

    for (const auto& objPair : mapObjects) {
        switch (objPair.second.GetObjectType()) {
        case GOVERNANCE_OBJECT_PROPOSAL:
            nProposalCount++;
            break;
        case GOVERNANCE_OBJECT_TRIGGER:
            nTriggerCount++;
            break;
        case GOVERNANCE_OBJECT_RECORD:
            nRecordCount++;
            break;    
        default:
            nOtherCount++;
            break;
        }
    }

    return strprintf("Governance Objects: %d (Proposals: %d, Records: %d, Triggers: %d, Other: %d; Erased: %d), Votes: %d",
        (int)mapObjects.size(),
        nProposalCount, nRecordCount, nTriggerCount, nOtherCount, (int)mapErasedGovernanceObjects.size(),
        (int)cmapVoteToObject.GetSize());
}

UniValue CGovernanceManager::ToJson() const
{
    LOCK(cs);

    int nProposalCount = 0;
    int nRecordCount = 0;
    int nTriggerCount = 0;
    int nOtherCount = 0;

    for (const auto& objpair : mapObjects) {
        switch (objpair.second.GetObjectType()) {
        case GOVERNANCE_OBJECT_PROPOSAL:
            nProposalCount++;
            break;
        case GOVERNANCE_OBJECT_RECORD:
            nRecordCount++;
            break;
        case GOVERNANCE_OBJECT_TRIGGER:
            nTriggerCount++;
            break;
        default:
            nOtherCount++;
            break;
        }
    }

    UniValue jsonObj(UniValue::VOBJ);
    jsonObj.push_back(Pair("objects_total", (int)mapObjects.size()));
    jsonObj.push_back(Pair("proposals", nProposalCount));
    jsonObj.push_back(Pair("records", nRecordCount));
    jsonObj.push_back(Pair("triggers", nTriggerCount));
    jsonObj.push_back(Pair("other", nOtherCount));
    jsonObj.push_back(Pair("erased", (int)mapErasedGovernanceObjects.size()));
    jsonObj.push_back(Pair("votes", (int)cmapVoteToObject.GetSize()));
    return jsonObj;
}

void CGovernanceManager::UpdatedBlockTip(const CBlockIndex* pindex, CConnman& connman)
{
    // Note this gets called from ActivateBestChain without cs_main being held
    // so it should be safe to lock our mutex here without risking a deadlock
    // On the other hand it should be safe for us to access pindex without holding a lock
    // on cs_main because the CBlockIndex objects are dynamically allocated and
    // presumably never deleted.
    if (!pindex) {
        return;
    }

    nCachedBlockHeight = pindex->nHeight;
    LogPrint("gobject", "CGovernanceManager::UpdatedBlockTip -- nCachedBlockHeight: %d\n", nCachedBlockHeight);

    if (deterministicMNManager->IsDIP3Enforced(pindex->nHeight)) {
        RemoveInvalidVotes();
    }

    CheckPostponedObjects(connman);

    CSuperblockManager::ExecuteBestSuperblock(pindex->nHeight);
}

void CGovernanceManager::RequestOrphanObjects(CConnman& connman)
{
    std::vector<CNode*> vNodesCopy = connman.CopyNodeVector(CConnman::FullyConnectedOnly);

    std::vector<uint256> vecHashesFiltered;
    {
        std::vector<uint256> vecHashes;
        LOCK(cs);
        cmmapOrphanVotes.GetKeys(vecHashes);
        for (const uint256& nHash : vecHashes) {
            if (mapObjects.find(nHash) == mapObjects.end()) {
                vecHashesFiltered.push_back(nHash);
            }
        }
    }

    LogPrint("gobject", "CGovernanceObject::RequestOrphanObjects -- number objects = %d\n", vecHashesFiltered.size());
    for (const uint256& nHash : vecHashesFiltered) {
        for (CNode* pnode : vNodesCopy) {
            if (pnode->fMasternode) {
                continue;
            }
            RequestGovernanceObject(pnode, nHash, connman);
        }
    }

    connman.ReleaseNodeVector(vNodesCopy);
}

void CGovernanceManager::CleanOrphanObjects()
{
    LOCK(cs);
    const vote_cmm_t::list_t& items = cmmapOrphanVotes.GetItemList();

    int64_t nNow = GetAdjustedTime();

    vote_cmm_t::list_cit it = items.begin();
    while (it != items.end()) {
        vote_cmm_t::list_cit prevIt = it;
        ++it;
        const vote_time_pair_t& pairVote = prevIt->value;
        if (pairVote.second < nNow) {
            cmmapOrphanVotes.Erase(prevIt->key, prevIt->value);
        }
    }
}

void CGovernanceManager::RemoveInvalidVotes()
{
    if (!masternodeSync.IsSynced()) {
        return;
    }

    LOCK(cs);

    auto curMNList = deterministicMNManager->GetListAtChainTip();
    auto diff = lastMNListForVotingKeys.BuildDiff(curMNList);

    std::vector<COutPoint> changedKeyMNs;
    for (const auto& p : diff.updatedMNs) {
        auto oldDmn = lastMNListForVotingKeys.GetMNByInternalId(p.first);
        if ((p.second.fields & CDeterministicMNStateDiff::Field_keyIDVoting) && p.second.state.keyIDVoting != oldDmn->pdmnState->keyIDVoting) {
            changedKeyMNs.emplace_back(oldDmn->collateralOutpoint);
        } else if ((p.second.fields & CDeterministicMNStateDiff::Field_pubKeyOperator) && p.second.state.pubKeyOperator != oldDmn->pdmnState->pubKeyOperator) {
            changedKeyMNs.emplace_back(oldDmn->collateralOutpoint);
        }
    }
    for (const auto& id : diff.removedMns) {
        auto oldDmn = lastMNListForVotingKeys.GetMNByInternalId(id);
        changedKeyMNs.emplace_back(oldDmn->collateralOutpoint);
    }

    int nBlockHeight = 0;
    {
        nBlockHeight = (int)chainActive.Height();
    }
    
    for (const auto& outpoint : changedKeyMNs) {
        for (auto& p : mapObjects) {
            if (p.second.GetObjectType() == GOVERNANCE_OBJECT_RECORD && (nBlockHeight < p.second.GetCollateralNextSuperBlock())) {
                auto removed = p.second.RemoveInvalidVotes(outpoint);
                if (removed.empty()) {
                    continue;
                }
                for (auto& voteHash : removed) {
                    cmapVoteToObject.Erase(voteHash);
                    cmapInvalidVotes.Erase(voteHash);
                    cmmapOrphanVotes.Erase(voteHash);
                    setRequestedVotes.erase(voteHash);
                }
            } else if (p.second.GetObjectType() != GOVERNANCE_OBJECT_RECORD) {
                auto removed = p.second.RemoveInvalidVotes(outpoint);
                if (removed.empty()) {
                    continue;
                }
                for (auto& voteHash : removed) {
                    cmapVoteToObject.Erase(voteHash);
                    cmapInvalidVotes.Erase(voteHash);
                    cmmapOrphanVotes.Erase(voteHash);
                    setRequestedVotes.erase(voteHash);
                }
                
            }            
        }
    }

    // store current MN list for the next run so that we can determine which keys changed
    lastMNListForVotingKeys = curMNList;
}

bool CGovernanceManager::ValidIPFSHash(CGovernanceObject& govobj)
{
    ipfs::Client ipfsclient("localhost", 5001);
    std::string ipfsHash = "empty";
    try {
        UniValue Jobj = govobj.GetJSONObject();
        ipfsHash = Jobj["ipfscid"].get_str();
        if (ipfsHash.length() < 50) {
            LogPrintf("MNGOVERNANCEOBJECT::ValidIPFSHash -- Valid IPFS hash\n");
            return true;
        } else {
            LogPrintf("MNGOVERNANCEOBJECT::ValidIPFSHash -- IPFS hash NOT valid\n");
            return false;
        }
    } catch (std::exception& e) {
        LogPrintf("MNGOVERNANCEOBJECT::ValidIPFSHash -- Could not get IPFS Hash: %s\n", ipfsHash);
        return false;
    }
        
}


uint256 CGovernanceManager::CollateralHashBlock(const uint256& nCollateralHash)
{
    CTransactionRef tx;
    uint256 hashBlock = uint256();

    if (!GetTransaction(nCollateralHash, tx, Params().GetConsensus(), hashBlock, true)) {
        LogPrintf("CGovernanceManager::CollateralHashBlock -- Can't get transaction\n");
    } else {
        LogPrintf("CGovernanceManager::CollateralHashBlock hashblock: %s\n", hashBlock.ToString());
        return hashBlock;
    }
    return hashBlock;
}
