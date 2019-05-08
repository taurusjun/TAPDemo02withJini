#ifndef TRADE_H
#define TRADE_H

#include "TapTradeAPI.h"
#include "../Common/SimpleEvent.h"

class Trade : public ITapTradeAPINotify
{
public:
	Trade(void);
	~Trade(void);

	void SetAPI(ITapTradeAPI *pAPI);
	void RunTest();

public:
	//对ITapTradeAPINotify的实现
	virtual void TAP_CDECL OnConnect();
	virtual void TAP_CDECL OnRspLogin(TAPIINT32 errorCode, const TapAPITradeLoginRspInfo *loginRspInfo);
	virtual void TAP_CDECL OnAPIReady();
	virtual void TAP_CDECL OnDisconnect(TAPIINT32 reasonCode);
	virtual void TAP_CDECL OnRspChangePassword(TAPIUINT32 sessionID, TAPIINT32 errorCode);
	virtual void TAP_CDECL OnRspSetReservedInfo(TAPIUINT32 sessionID, TAPIINT32 errorCode, const TAPISTR_50 info);
	virtual void TAP_CDECL OnRspQryAccount(TAPIUINT32 sessionID, TAPIUINT32 errorCode, TAPIYNFLAG isLast, const TapAPIAccountInfo *info);
	virtual void TAP_CDECL OnRspQryFund(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIFundData *info);
	virtual void TAP_CDECL OnRtnFund(const TapAPIFundData *info);
	virtual void TAP_CDECL OnRspQryExchange(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIExchangeInfo *info);
	virtual void TAP_CDECL OnRspQryCommodity(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPICommodityInfo *info);
	virtual void TAP_CDECL OnRspQryContract(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPITradeContractInfo *info);
	virtual void TAP_CDECL OnRtnContract(const TapAPITradeContractInfo *info);
	virtual void TAP_CDECL OnRtnOrder(const TapAPIOrderInfoNotice *info);
	virtual void TAP_CDECL OnRspOrderAction(TAPIUINT32 sessionID, TAPIUINT32 errorCode, const TapAPIOrderActionRsp *info);
	virtual void TAP_CDECL OnRspQryOrder(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIOrderInfo *info);
	virtual void TAP_CDECL OnRspQryOrderProcess(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIOrderInfo *info);
	virtual void TAP_CDECL OnRspQryFill(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIFillInfo *info);
	virtual void TAP_CDECL OnRtnFill(const TapAPIFillInfo *info);
	virtual void TAP_CDECL OnRspQryPosition(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIPositionInfo *info);
	virtual void TAP_CDECL OnRtnPosition(const TapAPIPositionInfo *info);
	virtual void TAP_CDECL OnRspQryClose(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPICloseInfo *info);
	virtual void TAP_CDECL OnRtnClose(const TapAPICloseInfo *info);
	virtual void TAP_CDECL OnRtnPositionProfit(const TapAPIPositionProfitNotice *info);
	virtual void TAP_CDECL OnRspQryDeepQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIDeepQuoteQryRsp *info);
	virtual void TAP_CDECL OnRspQryExchangeStateInfo(TAPIUINT32 sessionID,TAPIINT32 errorCode, TAPIYNFLAG isLast,const TapAPIExchangeStateInfo * info);
	virtual void TAP_CDECL OnRtnExchangeStateInfo(const TapAPIExchangeStateInfoNotice * info);
	virtual void TAP_CDECL OnRtnReqQuoteNotice(const TapAPIReqQuoteNotice *info); //V9.0.2.0 20150520
	virtual void TAP_CDECL OnRspUpperChannelInfo(TAPIUINT32 sessionID,TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIUpperChannelInfo * info);
	virtual void TAP_CDECL OnRspAccountRentInfo(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIAccountRentInfo * info);

private:
	ITapTradeAPI *m_pAPI;
	TAPIUINT32	m_uiSessionID;
	SimpleEvent m_Event;
	bool		m_bIsAPIReady;
};

#endif // TRADE_H
