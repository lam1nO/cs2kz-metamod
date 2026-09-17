#include "cyb_replay_common.h"

#include <string>

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

std::string CybReplayCommon::CourseText(int cyberCourseNumber)
{
	if (cyberCourseNumber == 0)
	{
		return "Main";
	}
	// 1..99 — бонусы (номер из имени курса), 100+ — прочий именованный курс: его имени на
	// этой стороне нет, показываем номер, чтобы отказ всё равно называл конкретный курс.
	if (cyberCourseNumber >= 1 && cyberCourseNumber <= 99)
	{
		return "Bonus " + std::to_string(cyberCourseNumber);
	}
	return "#" + std::to_string(cyberCourseNumber);
}
