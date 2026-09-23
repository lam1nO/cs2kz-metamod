#pragma once
#include "irecipientfilter.h"
#include "utils/utils.h"
#include "interfaces/interfaces.h"
#include "inetchannel.h"
#include "sdk/entity/cbaseentity.h"
#include "sdk/serversideclient.h"
#include "interfaces/interfaces.h"
#include "iserver.h"

class CRecipientFilter : public IRecipientFilter
{
public:
	CRecipientFilter(NetChannelBufType_t nBufType = BUF_RELIABLE, bool bInitMessage = false) : m_nBufType(nBufType), m_bInitMessage(bInitMessage) {}

	CRecipientFilter(IRecipientFilter *source, CPlayerSlot exceptSlot = {-1})
	{
		m_Recipients = source->GetRecipients();
		m_nBufType = source->GetNetworkBufType();
		m_bInitMessage = source->IsInitMessage();

		if (exceptSlot.Get() != -1)
		{
			m_Recipients.Clear(exceptSlot.Get());
		}
	}

	~CRecipientFilter() override {}

	NetChannelBufType_t GetNetworkBufType(void) const override
	{
		return m_nBufType;
	}

	bool IsInitMessage(void) const override
	{
		return m_bInitMessage;
	}

	const CPlayerBitVec &GetRecipients(void) const override
	{
		return m_Recipients;
	}

	virtual CPlayerSlot GetPredictedPlayerSlot() const override
	{
		return m_slotPlayerExcludedDueToPrediction;
	}

	void SetRecipients(uint64 nRecipients)
	{
		m_Recipients.Set(0UL, static_cast<uint32>(nRecipients & 0xFFFFFFFF));
		m_Recipients.Set(1UL, static_cast<uint32>(nRecipients >> 32));
	}

	void AddRecipient(CPlayerSlot slot)
	{
		if (slot.Get() >= 0 && slot.Get() < ABSOLUTE_PLAYER_LIMIT)
		{
			m_Recipients.Set(slot.Get());
		}
	}

	void RemoveRecipient(CPlayerSlot slot)
	{
		if (slot.Get() >= 0 && slot.Get() < ABSOLUTE_PLAYER_LIMIT)
		{
			m_Recipients.Clear(slot.Get());
		}
	}

	void AddPlayersFromBitMask(const CPlayerBitVec &playerbits)
	{
		int index = playerbits.FindNextSetBit(0);

		while (index > -1)
		{
			AddRecipient(index);

			index = playerbits.FindNextSetBit(index + 1);
		}
	}

	void RemovePlayersFromBitMask(const CPlayerBitVec &playerbits)
	{
		int index = playerbits.FindNextSetBit(0);

		while (index > -1)
		{
			RemoveRecipient(index);

			index = playerbits.FindNextSetBit(index + 1);
		}
	}

public:
	void CopyFrom(const CRecipientFilter &src)
	{
		m_Recipients = src.m_Recipients;
		m_bInitMessage = src.m_bInitMessage;
	}

	void Reset(void)
	{
		m_Recipients.ClearAll();
		m_bInitMessage = false;
	}

	void MakeInitMessage(void)
	{
		m_bInitMessage = true;
	}

	void MakeReliable(void);

	void AddAllPlayers(void)
	{
		m_Recipients.ClearAll();

		// GetClientList() отдаёт nullptr, когда проверка правдоподобия отвергла список
		// (неверный ClientOffset или движок ещё не поднял вектор). Без этой проверки
		// широковещательный фильтр падал на разыменовании нуля — то есть поломка офсета
		// превращалась из отказа в крэш.
		auto *clients = g_pKZUtils->GetClientList();
		if (!clients)
		{
			return;
		}
		for (int i = 0; i < clients->Count(); i++)
		{
			CServerSideClient *client = clients->Element(i);
			if (client && client->IsInGame())
			{
				AddRecipient(client->GetPlayerSlot());
			}
		}
	}

	void RemoveAllRecipients(void)
	{
		m_Recipients.ClearAll();
	}

protected:
	CPlayerBitVec m_Recipients;
	CPlayerSlot m_slotPlayerExcludedDueToPrediction = -1;
	NetChannelBufType_t m_nBufType = BUF_DEFAULT;
	bool m_bInitMessage;
	bool m_bDoNotSuppressPrediction; // unused
};

class CBroadcastRecipientFilter : public CRecipientFilter
{
public:
	CBroadcastRecipientFilter(void)
	{
		AddAllPlayers();
	}
};

class CSingleRecipientFilter : public CRecipientFilter
{
public:
	CSingleRecipientFilter(CPlayerSlot slot)
	{
		AddRecipient(slot);
	}
};
