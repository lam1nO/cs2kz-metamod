#ifndef KZ_REPLAYSESSION_H
#define KZ_REPLAYSESSION_H
#include "common.h"
#include <string>

class KZPlayer;
class CCSPlayerController;

namespace KZ::replaysystem::session
{
	// Ноль — пустой контекст; остальные индексы принадлежат слотам людей.
	constexpr int Count = MAXPLAYERS + 2;
	int Current();
	uint64_t Generation();
	int ForPlayer(KZPlayer *player);

	struct ProgressAnchor
	{
		bool active {};
		Vector origin {};
		std::string course;
	};

	void CaptureProgress(KZPlayer *player);
	ProgressAnchor TakeProgress(KZPlayer *player);
	void Bind(KZPlayer *player);
	bool Owns(KZPlayer *player);
	bool CanControlObserved(KZPlayer *player);
	void Release();
	void ProcessFrames();

	// Контекст локален потоку и восстанавливается после вложенных вызовов.
	class Scope
	{
		int previous;

	public:
		explicit Scope(int index);
		explicit Scope(KZPlayer *player);
		~Scope();
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;
	};
} // namespace KZ::replaysystem::session
#endif
