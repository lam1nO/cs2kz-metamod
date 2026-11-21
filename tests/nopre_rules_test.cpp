#include "kz/style/nopre_rules.h"
#include "kz/style/style_key.h"

#include <cassert>
#include <limits>

int main()
{
	using namespace KZ::style;
	assert(NoprePerfSpeed(500.0f, 287.5f) == 287.5f);
	assert(NoprePerfSpeed(260.0f, 287.5f) == 260.0f);
	assert(NoprePerfSpeed(260.0f, -1.0f) == 260.0f);
	assert(NoprePerfSpeed(500.0f, std::numeric_limits<float>::quiet_NaN()) == 287.5f);
	assert(StyleKey({"LGJ", "ABH", "LGJ"}) == "ABH+LGJ");
	assert(EncodeStyleKey(StyleKey({"NOPRE", "ABH"})) == "ABH%2BNOPRE");
	assert(StyleKey({}).empty());
}
