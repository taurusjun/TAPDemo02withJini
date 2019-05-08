#include "Trade.h"
#include "TapAPIError.h"
#include "TradeConfig.h"
#include <iostream>
#include <string.h>

using namespace std;

Trade::Trade(void):
	m_pAPI(NULL),
	m_bIsAPIReady(false)
{
}


Trade::~Trade(void)
{
}


void Trade::SetAPI(ITapTradeAPI *pAPI)
{
	m_pAPI = pAPI;
}


void Trade::RunTest()
{
	if(NULL == m_pAPI) {
		cout << "Error: m_pAPI is NULL." << endl;
		return;
	}

	TAPIINT32 iErr = TAPIERROR_SUCCEED;


	//设定服务器IP、端口
	iErr = m_pAPI->SetHostAddress(DEFAULT_IP, DEFAULT_PORT);
	if(TAPIERROR_SUCCEED != iErr) {
		cout << "SetHostAddress Error:" << iErr <<endl;
		return;
	}

	//登录服务器
	TapAPITradeLoginAuth stLoginAuth;
	memset(&stLoginAuth, 0, sizeof(stLoginAuth));
	strcpy(stLoginAuth.UserNo, DEFAULT_USERNAME);
	strcpy(stLoginAuth.Password, DEFAULT_PASSWORD);
	stLoginAuth.ISModifyPassword = APIYNFLAG_NO;
	stLoginAuth.ISDDA = APIYNFLAG_NO;
	iErr = m_pAPI->Login(&stLoginAuth);
	if(TAPIERROR_SUCCEED != iErr) {
		cout << "Login Error:" << iErr <<endl;
		return;
	}
	

	//等待APIReady
	m_Event.WaitEvent();
	if (!m_bIsAPIReady){
		cout << "API is not ready";
		return;
	}

	//下单
	TapAPINewOrder stNewOrder;
	memset(&stNewOrder, 0, sizeof(stNewOrder));

	strcpy(stNewOrder.AccountNo, DEFAULT_ACCOUNT_NO);			
	strcpy(stNewOrder.ExchangeNo, DEFAULT_EXCHANGE_NO);		
	stNewOrder.CommodityType = DEFAULT_COMMODITY_TYPE;		
	strcpy(stNewOrder.CommodityNo, DEFAULT_COMMODITY_NO);		
	strcpy(stNewOrder.ContractNo, DEFAULT_CONTRACT_NO);		
	strcpy(stNewOrder.StrikePrice, "");		
	stNewOrder.CallOrPutFlag = TAPI_CALLPUT_FLAG_NONE;		
	strcpy(stNewOrder.ContractNo2, "");		
	strcpy(stNewOrder.StrikePrice2, "");		
	stNewOrder.CallOrPutFlag2 = TAPI_CALLPUT_FLAG_NONE;	
	stNewOrder.OrderType = DEFAULT_ORDER_TYPE;			
	stNewOrder.OrderSource = TAPI_ORDER_SOURCE_ESUNNY_API;		
	stNewOrder.TimeInForce = TAPI_ORDER_TIMEINFORCE_GFD;		
	strcpy(stNewOrder.ExpireTime, "");		
	stNewOrder.IsRiskOrder = APIYNFLAG_NO;		
	stNewOrder.OrderSide = DEFAULT_ORDER_SIDE;			
	stNewOrder.PositionEffect = DEFAULT_POSITION_EFFECT;	
	stNewOrder.PositionEffect2 = TAPI_PositionEffect_NONE;	
	strcpy(stNewOrder.InquiryNo,"");			
	stNewOrder.HedgeFlag = TAPI_HEDGEFLAG_T;			
	stNewOrder.OrderPrice = DEFAULT_ORDER_PRICE;		
	stNewOrder.OrderPrice2;		
	stNewOrder.StopPrice;			
	stNewOrder.OrderQty = DEFAULT_ORDER_QTY;			
	stNewOrder.OrderMinQty ;		
	stNewOrder.MinClipSize;		
	stNewOrder.MaxClipSize;		
	stNewOrder.RefInt;				
	stNewOrder.RefString;			
	stNewOrder.TacticsType = TAPI_TACTICS_TYPE_NONE;		
	stNewOrder.TriggerCondition = TAPI_TRIGGER_CONDITION_NONE;	
	stNewOrder.TriggerPriceType = TAPI_TRIGGER_PRICE_NONE;	
	stNewOrder.AddOneIsValid = APIYNFLAG_NO;	
	stNewOrder.OrderQty2;
	stNewOrder.HedgeFlag2 = TAPI_HEDGEFLAG_NONE;
	stNewOrder.MarketLevel = TAPI_MARKET_LEVEL_0;
	stNewOrder.FutureAutoCloseFlag = APIYNFLAG_NO; // V9.0.2.0 20150520
	
	m_uiSessionID = 0;
	iErr = m_pAPI->InsertOrder(&m_uiSessionID, &stNewOrder);
	if(TAPIERROR_SUCCEED != iErr) {
		cout << "InsertOrder Error:" << iErr <<endl;
		return;
	}


	while (true)
	{
		m_Event.WaitEvent();
	}	
}

void TAP_CDECL Trade::OnConnect()
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspLogin( TAPIINT32 errorCode, const TapAPITradeLoginRspInfo *loginRspInfo )
{
	if(TAPIERROR_SUCCEED == errorCode) {
		// cout << "登录成功，等待API初始化..." << endl;
		cout << "Login success, wait for API initialization..." << endl;

	} else {
		cout << "登录失败，错误码:" << errorCode << endl;
		m_Event.SignalEvent();	
	}
}

void TAP_CDECL Trade::OnAPIReady()
{
	// cout << "API初始化完成" << endl;
	cout << "API initialization success" << endl;
	m_bIsAPIReady = true;
	m_Event.SignalEvent();	
}

void TAP_CDECL Trade::OnDisconnect( TAPIINT32 reasonCode )
{
	cout << "API断开,断开原因:"<<reasonCode << endl;
}

void TAP_CDECL Trade::OnRspChangePassword( TAPIUINT32 sessionID, TAPIINT32 errorCode )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspSetReservedInfo( TAPIUINT32 sessionID, TAPIINT32 errorCode, const TAPISTR_50 info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryAccount( TAPIUINT32 sessionID, TAPIUINT32 errorCode, TAPIYNFLAG isLast, const TapAPIAccountInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryFund( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIFundData *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnFund( const TapAPIFundData *info )
{
//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryExchange( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIExchangeInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryCommodity( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPICommodityInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryContract( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPITradeContractInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnContract( const TapAPITradeContractInfo *info )
{
//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnOrder( const TapAPIOrderInfoNotice *info )
{
	cout << __FUNCTION__ << " is called." << endl;
	if(NULL == info){
		return;
	}

	if (info->ErrorCode != 0) {
		cout << "服务器返回了一个关于委托信息的错误：" << info->ErrorCode << endl;
	} else {
		if (info->OrderInfo) {
			if (info->SessionID == m_uiSessionID)
			{
				if (0!= info->OrderInfo->ErrorCode){
					cout << "报单失败，"
						<< "错误码:"<<info->OrderInfo->ErrorCode << ","
						<< "委托编号:"<<info->OrderInfo->OrderNo
						<<endl;
				} else{
					cout << "报单成功，"
						<< "状态:"<<info->OrderInfo->OrderState << ","
						<< "委托编号:"<<info->OrderInfo->OrderNo
						<<endl;
				}
				m_Event.SignalEvent();
			}
		}
	}
}

void TAP_CDECL Trade::OnRspOrderAction( TAPIUINT32 sessionID, TAPIUINT32 errorCode, const TapAPIOrderActionRsp *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryOrder( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIOrderInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryOrderProcess( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIOrderInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryFill( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIFillInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnFill( const TapAPIFillInfo *info )
{
//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryPosition( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIPositionInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnPosition( const TapAPIPositionInfo *info )
{
//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryClose( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPICloseInfo *info )
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnClose( const TapAPICloseInfo *info )
{
//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnPositionProfit( const TapAPIPositionProfitNotice *info )
{
//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryDeepQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIDeepQuoteQryRsp *info)
{
	//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspQryExchangeStateInfo(TAPIUINT32 sessionID,TAPIINT32 errorCode, TAPIYNFLAG isLast,const TapAPIExchangeStateInfo * info)
{
	//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnExchangeStateInfo(const TapAPIExchangeStateInfoNotice * info)
{
	//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRtnReqQuoteNotice(const TapAPIReqQuoteNotice *info)
{
	//	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL Trade::OnRspUpperChannelInfo(TAPIUINT32 sessionID,TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIUpperChannelInfo * info)
{

}

void TAP_CDECL Trade::OnRspAccountRentInfo(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIAccountRentInfo * info)
{}