#pragma once

namespace KZ::mode
{
	struct KztAutoBhopContact
	{
		float landingTime;
		float takeoffFrameEnd;
		float jumpPressTime;
		bool latestLanding;
		bool jumpHeld;
		bool walkMoved;
		bool positiveImpulse;
	};

	inline bool ResolveKztPerf(bool strictPerf, bool structuralPerf, bool structuralEnabled, bool autobhop,
							   const KztAutoBhopContact &contact, float tickInterval)
	{
		if (structuralEnabled)
		{
			return structuralPerf;
		}
		const float elapsed = contact.takeoffFrameEnd - contact.landingTime;
		return strictPerf
			|| (autobhop && structuralPerf && contact.latestLanding && contact.jumpHeld && !contact.walkMoved && contact.positiveImpulse
				&& contact.jumpPressTime < contact.landingTime && elapsed >= 0.0f && elapsed <= tickInterval);
	}
}
