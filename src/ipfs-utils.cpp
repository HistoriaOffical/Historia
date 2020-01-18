#include "ipfs-utils.h"
#include "governance.h"
#include "governance-validators.h"
#include "json.hpp"
bool IsIpfsPeerIdValid(const std::string& ipfsId, CAmount collateralAmount) {
    /** All alphanumeric characters except for "0", "I", "O", and "l" */
    std::string base58chars =
	"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    if (ipfsId == "0" && collateralAmount != 100 * COIN)
	return false;
    
    /** https://docs.ipfs.io/guides/concepts/cid/ CID v0 */
    if (ipfsId.size() != 46 || ipfsId[0] !='Q' || ipfsId[1] !='m') {
	return false;
    }

    int l = ipfsId.length();
    for (int i = 0; i < l; i++)
	if (base58chars.find(ipfsId[i]) == -1)
	    return false;

    return true;
}

bool IsIpfsIdValid(const std::string& ipfsId)
{
    /** All alphanumeric characters except for "0", "I", "O", and "l" */
    std::string base58chars =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    /** https://docs.ipfs.io/guides/concepts/cid/ CID v0 */
    if (ipfsId.size() != 46 || ipfsId[0] != 'Q' || ipfsId[1] != 'm') {
        return false;
    }

    int l = ipfsId.length();
    for (int i = 0; i < l; i++)
        if (base58chars.find(ipfsId[i]) == -1)
            return false;

    return true;
}

bool IsIpfsIdDuplicate(const std::string& ipfsId)
{
    std::vector<const CGovernanceObject*> objs = governance.GetAllNewerThan(0);
    std::string ipfsCid;

    for (auto& pGovObj : objs) {
        std::string const plainData = pGovObj->GetDataAsPlainString();
        nlohmann::json jsonData = nlohmann::json::parse(plainData);
        nlohmann::json ipfsCid = jsonData["ipfscid"];
        if (ipfsCid == ipfsId) {
            return true;
        }
    }
    return false;
}
