#pragma once
class KZPlayer;
class Jump;
struct RunSubmission;

namespace KZ::analytics
{
	void InitEvidenceDelivery();
	void FlushPendingEvidence();
	void Reset(KZPlayer *player);
	void Start(KZPlayer *player);
	void Stop(KZPlayer *player);
	void EnterPrac(KZPlayer *player);
	void Teleport(KZPlayer *player);
	void OnJumpStart(Jump *jump);
	void OnJumpFinish(Jump *jump);
	void OnRunFinished(KZPlayer *player, const RunSubmission &submission);
} // namespace KZ::analytics
