#include "cyb_replay_common.h"

const char *CybReplayCommon::MapMode(const std::string &shortName)
{
	if (shortName == "CKZ" || shortName == "ckz")
		return "ckz";
	if (shortName == "VNL" || shortName == "vnl")
		return "vnl";
	if (shortName == "KZT" || shortName == "kzt")
		return "kzt";
	return "";
}

bool CybReplayCommon::IsValidMapName(const std::string &name)
{
	if (name.empty() || name.size() > 128)
	{
		return false;
	}
	for (char c : name)
	{
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok)
		{
			return false;
		}
	}
	return true;
}
