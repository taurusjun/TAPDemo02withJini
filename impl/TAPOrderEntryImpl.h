#ifndef TAPORDERENTRYIMPL_H
#define TAPORDERENTRYIMPL_H

#include "TapTradeAPI.h"
#include "TapAPIError.h"
#include "TAPSimpleEvent.h"

#include "jni/JNIThreadManager.h"
#include "collections/CollectionsHelper.hpp"
#include "enum/EnumHelper.hpp"
#include "codec/ByteCodec.hpp"
#include "codec/EncoderBinding.h"
#include "codec/DecoderBinding.h"
#include "codec/CoreMessageEncoder.hpp"
#include "codec/CoreMessageDecoder.hpp"
#include "instrument/SecurityDefinition.hpp"
#include "message/orderentry/OrderAction.h"
#include "message/statistics/StatisticsAction.h"
#include "message/statistics/bindings/StatisticsMessageDecoderBinding.hpp"
#include "message/statistics/bindings/StatisticsMessageEncoderBinding.hpp"
#include "message/orderentry/Side.h"
#include "message/orderentry/TimeInForce.h"
#include "message/orderentry/PositionType.h"
#include "message/orderentry/RejectType.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <string.h>
#include <math.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/atomic.hpp>
#include <boost/unordered_set.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/locale.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

using namespace std;
typedef std::string Shortname;
typedef std::string SecurityID;

typedef void(*CallbackFunc)(long,int);
typedef void(*LoggerFunc)(std::string);
typedef bool(*ShortnameLookupFunc)(std::string,SecurityDefinition*);
typedef bool(*SecurityIDLookupFunc)(std::string,std::string,SecurityDefinition*);

enum class TAPOrderStatus
{
	PENDING_NEW, WORKING, PENDING_REPLACE_PHASE_1, PENDING_REPLACE_PHASE_2, PENDING_CANCEL, CANCELLED, REJECTED
};

enum class TAPOrderAction
{
	INSERT, CANCEL
};

class TAPOrderMessageEncoderBinding;
class TAPOrderMessageDecoderBinding;

struct TapContractDefinition
{
	TAPISTR_10              ExchangeNo;
	TAPICommodityType		CommodityType;
	TAPISTR_10				CommodityNo;
	TAPISTR_10				ContractNo;
};

struct TAPOrderRequest
{
	const OrderAction* 					orderAction;
	std::string							orderID;

	TapAPINewOrder* 			reqNew;
	TapAPIOrderCancelReq* 		reqCancel;
};

struct TAPOrderUpdate
{
	const OrderAction* 			orderAction;
	std::string					orderID;

	const TapAPIFillInfo*		tradeUpdate;

	const RejectType*			rejectType;
	std::string					rejectReason;
};

struct TAPOrderIDMapping
{
	int			 	refID;
	std::string		orderID;
};

struct TAPOrderStatusMapping
{
	int			 			refID;
	TAPOrderStatus			status;
};

struct TAPReplaceCompletion
{
	int			 				refID;
	std::function<void()> 		func;
};

class SecurityCache
{
	public:
		virtual ~SecurityCache() {};

		virtual bool getSecurityDefinitionBySecurityID(std::string,SecurityDefinition*&) = 0;
		virtual bool getSecurityDefinitionByShortname(std::string,SecurityDefinition*&) = 0;
		virtual bool getContractDefinitionBySecurityID(std::string,TapContractDefinition*&) = 0;
};

class TAPOrderEntryImpl : public ITapTradeAPINotify, public SecurityCache
{
	public:
		TAPOrderEntryImpl(JNIThreadManager*,CallbackFunc,SecurityIDLookupFunc,ShortnameLookupFunc,LoggerFunc);
		virtual ~TAPOrderEntryImpl();

		void configure(std::string _address, std::string _port, std::string _brokerID, std::string _userID, std::string _password, std::string _exdest, std::string _authcode);

		bool connect();
		void disconnect();

		void request(char*, int);

		//implementation of ITapTradeAPINotify 
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

		//implementation of SecurityCache 
		virtual bool getSecurityDefinitionBySecurityID(std::string,SecurityDefinition*&);
		virtual bool getSecurityDefinitionByShortname(std::string,SecurityDefinition*&);
		virtual bool getContractDefinitionBySecurityID(std::string,TapContractDefinition*&);

		//Error handler:
		void OnOrderInsertError(int refID, int errorCode );
		void OnOrderCancelError(int refID, int errorCode );
		
		// inline
		// static std::string getErrorString(CThostFtdcRspInfoField* _rspInfo)
		// {
		// 	std::ostringstream os;
		// 	os << "[" << _rspInfo->ErrorID << "] " << _rspInfo->ErrorMsg;
		// 	return os.str();
		// }
		inline
		static std::string getErrorString(TAPIINT32 errorCode)
		{
			std::ostringstream os;
			os << "[ErrorCode: ] " << errorCode;
			return os.str();
		}

	private:
		TAPSimpleEvent m_Event;
		bool		m_bIsAPIReady;

		ITapTradeAPI* traderApi;

		TAPOrderMessageEncoderBinding* encOrderBinding;
		TAPOrderMessageDecoderBinding* decOrderBinding;

		StatisticsMessageEncoderBinding* encStatBinding;
		StatisticsMessageDecoderBinding* decStatBinding;

		char* fieldPresence_Order_Ack;
		int fpLength_Order_Ack;

		char* fieldPresence_Order_Rej;
		int fpLength_Order_Rej;

		char* fieldPresence_Order_Fill;
		int fpLength_Order_Fill;

		char* fieldPresence_Statistics_Position;
		int fpLength_Statistics_Position;

		char* fieldPresence_Statistics_Account;
		int fpLength_Statistics_Account;

		char* fieldPresence_Statistics_Connection;
		int fpLength_Statistics_Connection;

		boost::unordered_map<Shortname, SecurityDefinition*>* securityCacheByShortname;
		boost::unordered_map<SecurityID, SecurityDefinition*>* securityCacheBySecurityID;
		boost::unordered_set<Shortname>* arbitrageProducts;
		boost::unordered_map<SecurityID, TapContractDefinition*>* contractCacheBySecurityID;

		boost::mutex securityCacheFileMutex;

		boost::lockfree::spsc_queue<TAPOrderIDMapping>* orderIDToRefIDQueue;
		boost::unordered_map<std::string, int>*	orderIDToRefIDMap;

		boost::lockfree::spsc_queue<TAPOrderIDMapping>* refIDToOrderIDQueue;
		boost::unordered_map<int, std::string>*	refIDToOrderIDMap;

		boost::lockfree::spsc_queue<TAPOrderStatusMapping>* refIDToOrderStatusQueue;
		boost::unordered_map<int, TAPOrderStatus>*	refIDToOrderStatusMap;

		boost::unordered_map<int, std::string>*	refIDToOrderNoMap;
		boost::unordered_map<int, TAPOrderAction>*	refIDToOrderActionMap;

		boost::lockfree::spsc_queue<TAPReplaceCompletion*>* replaceQueue;
		boost::unordered_map<int, std::function<void()>>* replaceFunctionMap;

		std::string address;
		unsigned short port;
		std::string brokerID;
		std::string userID;
		std::string password;
		std::string exdest;
		std::string authcode;

		int frontID;
		int sessionID;

		unsigned int reqID;
		boost::atomic_int refID;

		JNIThreadManager* jniThreadManager;
		NativeThreadID mainCallbackThreadID;

		CallbackFunc callback;
		LoggerFunc logger;
		SecurityIDLookupFunc securityByID;
		ShortnameLookupFunc securityByShortname;

		bool login();
		void confirmLogin(TapAPITradeLoginRspInfo*);
		void retrievePositions();
		void resetPosition(std::string, const Side*, const PositionType*, int);
		void retrieveAccountDetails();
		void resetAccountDetails(double, double, double, double);
		void notifyConnectionStatus(bool);
		void loadSecurityCache(std::string);
		int getNextRequestID();
		int getNextRefID();

		void enrichNew(TapAPINewOrder*, int);
		void enrichCancel(TapAPIOrderCancelReq*, int);

		void ackNew(int);
		void ackCancel(int);

		void reject(int,const RejectType*,std::string);

		void updatePosition(StatisticsMessage*);
		void updateAccountDetails(StatisticsMessage*);
		void updateConnectionStatus(StatisticsMessage*);

		void updateOrderIDToRefIDMapping()
		{
			if(this->orderIDToRefIDQueue->read_available() > 0)
			{
				TAPOrderIDMapping mapping;
				while(this->orderIDToRefIDQueue->pop(mapping))
				{
					std::string orderID = mapping.orderID;
					int refID = mapping.refID;

					(*this->orderIDToRefIDMap)[orderID] = refID;
				}
			}
		}

		void updateRefIDToOrderIDMapping()
		{
			if(this->refIDToOrderIDQueue->read_available() > 0)
			{
				TAPOrderIDMapping mapping;
				while(this->refIDToOrderIDQueue->pop(mapping))
				{
					std::string orderID = mapping.orderID;
					int refID = mapping.refID;

					(*this->refIDToOrderIDMap)[refID] = orderID;
				}
			}
		}

		void updateRefIDToOrderStatusMapping()
		{
			if(this->refIDToOrderStatusQueue->read_available() > 0)
			{
				TAPOrderStatusMapping mapping;
				while(this->refIDToOrderStatusQueue->pop(mapping))
				{
					int refID = mapping.refID;
					TAPOrderStatus status = mapping.status;

					(*this->refIDToOrderStatusMap)[refID] = status;
				}
			}
		}

		void updateRefIDToReplaceFunctionMapping()
		{
			if(this->replaceQueue->read_available() > 0)
			{
				TAPReplaceCompletion* completion;
				while(this->replaceQueue->pop(completion))
				{
					int refID = completion->refID;
					std::function<void()> replaceFunction = completion->func;
					(*this->replaceFunctionMap)[refID] = replaceFunction;

					delete completion;
				}
			}
		}

		inline
		static void copyNewRequest(TapAPINewOrder* _dest, TapAPINewOrder* _src)
		{
			strcpy(_dest->CommodityNo, _src->CommodityNo);
			strcpy(_dest->ContractNo, _src->ContractNo);
			_dest->TriggerPriceType = _src->TriggerPriceType;
			_dest->TriggerCondition = _src->TriggerCondition;
			_dest->StopPrice = _src->StopPrice;
			_dest->CallOrPutFlag = _src->CallOrPutFlag;
			_dest->OrderType = _src->OrderType;
			_dest->IsRiskOrder = _src->IsRiskOrder;
			_dest->OrderSide = _src->OrderSide;
			_dest->PositionEffect = _src->PositionEffect;
			_dest->HedgeFlag = _src->HedgeFlag;
			_dest->OrderPrice = _src->OrderPrice;
			_dest->TimeInForce = _src->TimeInForce;
			_dest->OrderMinQty = _src->OrderMinQty;
			_dest->MinClipSize = _src->MinClipSize;
			_dest->MaxClipSize = _src->MaxClipSize;
			_dest->TacticsType = _src->TacticsType;
		}

		void printNewOrderRequest(TapAPINewOrder* _field)
		{
			std::ostringstream os;
			os << "[Field=TapAPINewOrder]";
			os << "[AccountNo=" << _field->AccountNo << "]";
			os << "[ExchangeNo=" << _field->ExchangeNo << "]";
			os << "[CommodityType=" << _field->CommodityType << "]";
			os << "[CommodityNo=" << _field->CommodityNo << "]";
			os << "[ContractNo=" << _field->ContractNo << "]";
			os << "[StrikePrice=" << _field->StrikePrice << "]";
			os << "[CallOrPutFlag=" << _field->CallOrPutFlag << "]";
			os << "[ContractNo2=" << _field->ContractNo2 << "]";
			os << "[StrikePrice2=" << _field->StrikePrice2 << "]";
			os << "[CallOrPutFlag2=" << _field->CallOrPutFlag2 << "]";
			os << "[OrderType=" << _field->OrderType << "]";
			os << "[OrderSource=" << _field->OrderSource << "]";
			os << "[TimeInForce=" << _field->TimeInForce << "]";
			os << "[ExpireTime=" << _field->ExpireTime << "]";
			os << "[IsRiskOrder=" << _field->IsRiskOrder << "]";
			os << "[OrderSide=" << _field->OrderSide << "]";
			os << "[PositionEffect=" << _field->PositionEffect << "]";
			os << "[PositionEffect2=" << _field->PositionEffect2 << "]";
			os << "[InquiryNo=" << _field->InquiryNo << "]";
			os << "[HedgeFlag=" << _field->HedgeFlag << "]";
			os << "[OrderPrice=" << _field->OrderPrice << "]";
			os << "[OrderPrice2=" << _field->OrderPrice2 << "]";
			os << "[StopPrice=" << _field->StopPrice << "]";
			os << "[OrderQty=" << _field->OrderQty << "]";
			os << "[OrderMinQty=" << _field->OrderMinQty << "]";
			os << "[MinClipSize=" << _field->MinClipSize << "]";
			os << "[MaxClipSize=" << _field->MaxClipSize << "]";
			os << "[RefInt=" << _field->RefInt << "]";
			os << "[RefString=" << _field->RefString << "]";
			os << "[TacticsType=" << _field->TacticsType << "]";
			os << "[TriggerCondition=" << _field->TriggerCondition << "]";
			os << "[TriggerPriceType=" << _field->TriggerPriceType << "]";
			os << "[AddOneIsValid=" << _field->AddOneIsValid << "]";
			os << "[OrderQty2=" << _field->OrderQty2 << "]";
			os << "[HedgeFlag2=" << _field->HedgeFlag2 << "]";
			os << "[MarketLevel=" << _field->MarketLevel << "]";
			os << "[FutureAutoCloseFlag=" << _field->FutureAutoCloseFlag << "]";
			os << "[UpperChannelNo=" << _field->UpperChannelNo << "]";
			os << endl;
			this->logger(os.str());
		}

		void printCancelOrderRequest(TapAPIOrderCancelReq* _field)
		{
			std::ostringstream os;
			os << "[Field=TapAPIOrderCancelReq]";
			os << "[RefInt=" << _field->RefInt << "]";
			os << "[RefString=" << _field->RefString << "]";
			os << "[ServerFlag=" << _field->ServerFlag << "]";
			os << "[OrderNo=" << _field->OrderNo << "]";
			os << endl;
			this->logger(os.str());
		}
};

class TAPOrderMessageEncoderBinding : public EncoderBinding<TAPOrderUpdate>
{
	public:
		TAPOrderMessageEncoderBinding(SecurityCache* _securityCache)
		{
			this->securityCache = _securityCache;
		}
		~TAPOrderMessageEncoderBinding(){}

		virtual char getByteValue(const ByteField* const _field, unsigned int _idx, TAPOrderUpdate* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_ORDER_REJECT_TYPE:
				{
					return _message->rejectType->value;
				}
				default:
				{
					return 0;
				}
			}
		}

		virtual short getShortValue(const ByteField* const _field, unsigned int _idx, TAPOrderUpdate* _message)
		{
			return 0;
		}

		virtual int getIntValue(const ByteField* const _field, unsigned int _idx, TAPOrderUpdate* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_ORDER_ACTION:
				{
					return _message->orderAction->value;
				}
				case ByteField::Identifier::ID_ORDER_EXECUTED_QUANTITY:
				{
					return _message->tradeUpdate->MatchQty;
				}
				case ByteField::Identifier::ID_PRICE_EXPONENT:
				{
					SecurityDefinition* definition = 0;
					string c1=_message->tradeUpdate->CommodityNo;
					string c2=_message->tradeUpdate->ContractNo;
					string instrumentID=c1+c2;
					this->securityCache->getSecurityDefinitionBySecurityID(instrumentID, definition);
					return definition->pxExponent;
				}
				default:
				{
					return 0;
				}
			}
		}

		virtual long getLongValue(const ByteField* const _field, unsigned int _idx, TAPOrderUpdate* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_ORDER_FILL_PRICE:
				{
					SecurityDefinition* definition = 0;
					string c1=_message->tradeUpdate->CommodityNo;
					string c2=_message->tradeUpdate->ContractNo;
					string instrumentID=c1+c2;
					this->securityCache->getSecurityDefinitionBySecurityID(instrumentID, definition);
					return getMantissa(_message->tradeUpdate->MatchPrice, definition->pxExponent);
				}
				default:
				{
					return 0;
				}
			}
		}

		virtual std::string getStringValue(const ByteField* const _field, unsigned int _idx, TAPOrderUpdate* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_ORDER_ID:
				{
					return _message->orderID;
				}
				case ByteField::Identifier::ID_ORDER_EXECUTION_ID:
				{
					std::string execID = std::string(_message->tradeUpdate->OrderNo);
					return execID.substr(execID.find_first_not_of(' '));
				}
				case ByteField::Identifier::ID_SHORTNAME:
				{
					SecurityDefinition* definition = 0;
					string c1=_message->tradeUpdate->CommodityNo;
					string c2=_message->tradeUpdate->ContractNo;
					string instrumentID=c1+c2;
					this->securityCache->getSecurityDefinitionBySecurityID(instrumentID, definition);
					return definition->shortname;
				}
				case ByteField::Identifier::ID_ORDER_REJECT_REASON:
				{
					std::string encoded = boost::locale::conv::to_utf<char>(_message->rejectReason, "GB2312");
					return encoded;
				}
				default:
				{
					return "";
				}
			}
		}

		virtual short getVarDataLength(const ByteField* const _field, unsigned int _idx, TAPOrderUpdate* _message)
		{
			return 0;
		}
	private:
		SecurityCache* securityCache;

		static long getMantissa(double _raw, int _exponent)
		{
			return (long)round(_raw * pow(10, _exponent));
		}
};

class TAPOrderMessageDecoderBinding : public DecoderBinding<TAPOrderRequest>
{
	public:
		TAPOrderMessageDecoderBinding(SecurityCache* _securityCache, boost::unordered_set<Shortname>* _arbitrageProducts)
		{
			this->securityCache = _securityCache;
			this->arbitrageProducts = _arbitrageProducts;
		}
		~TAPOrderMessageDecoderBinding(){}

		virtual void onField(const ByteField* const _field, unsigned int _idx, char* _bytes, unsigned int _offset, TAPOrderRequest* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_ORDER_ACTION:
				{
					_message->orderAction = OrderAction::getAction(ByteCodec::getInt(_bytes, _offset));
					break;
				}
				case ByteField::Identifier::ID_ORDER_ID:
				{
					_message->orderID = ByteCodec::getString(_bytes, _offset, ByteField::ORDER_ID->length);
					break;
				}
				case ByteField::Identifier::ID_SHORTNAME:
				{
					std::string shortname = ByteCodec::getString(_bytes, _offset, ByteField::SHORTNAME->length);
					SecurityDefinition* definition = 0;
					if(!this->securityCache->getSecurityDefinitionByShortname(shortname, definition))
					{
						return;
					}

					if(_message->orderAction == OrderAction::NEW || _message->orderAction == OrderAction::REPLACE)
					{
						TapContractDefinition* contractDef = 0;
						if(!this->securityCache->getContractDefinitionBySecurityID(definition->securityID, contractDef)){
							return;
						}
						strcpy(_message->reqNew->ExchangeNo, contractDef->ExchangeNo);
						_message->reqNew->CommodityType = contractDef->CommodityType;
						strcpy(_message->reqNew->CommodityNo, contractDef->CommodityNo);
						strcpy(_message->reqNew->ContractNo, contractDef->ContractNo);

						if(CollectionsHelper::containsItem(this->arbitrageProducts, shortname))
						{
							_message->reqNew->HedgeFlag = TAPI_HEDGEFLAG_L;
						}
						else
						{
							_message->reqNew->HedgeFlag = TAPI_HEDGEFLAG_T;
						}
					}

					if(_message->orderAction == OrderAction::CANCEL || _message->orderAction == OrderAction::REPLACE)
					{
						//TODO: cancel order 应该不需要合约信息
						// strcpy(_message->reqCancel->InstrumentID, definition->securityID.c_str());
					}

					break;
				}
				case ByteField::Identifier::ID_ORDER_LIMIT_PRICE:
				{
					_message->reqNew->TriggerPriceType = TAPI_TRIGGER_PRICE_NONE;

					long mantissa = ByteCodec::getLong(_bytes, _offset);
					_message->reqNew->OrderPrice = (double)mantissa;
					break;
				}
				case ByteField::Identifier::ID_PRICE_EXPONENT:
				{
					int pxExponent = ByteCodec::getInt(_bytes, _offset);
					long mantissa = (long)_message->reqNew->OrderPrice;
					_message->reqNew->OrderPrice = mantissa * pow10(pxExponent);

					break;
				}
				case ByteField::Identifier::ID_ORDER_SIZE:
				{
					_message->reqNew->OrderQty = ByteCodec::getInt(_bytes, _offset);
					break;
				}
				case ByteField::Identifier::ID_ORDER_SIDE:
				{
					const Side* side = Side::getSide(_bytes[_offset]);
					switch(side->ID)
					{
						case Side::Identifier::ID_BUY:
						{
							_message->reqNew->OrderSide = TAPI_SIDE_BUY;
							break;
						}
						case Side::Identifier::ID_SELL:
						{
							_message->reqNew->OrderSide = TAPI_SIDE_SELL;
							break;
						}
						default:
						{
						}
					}
					break;
				}
				case ByteField::Identifier::ID_ORDER_TIME_IN_FORCE:
				{
					const TimeInForce* tif = TimeInForce::getTimeInForce(_bytes[_offset]);
					switch(tif->ID)
					{
						case TimeInForce::Identifier::ID_FAK:
						{
							_message->reqNew->TimeInForce = TAPI_ORDER_TIMEINFORCE_FAK;
							// _message->reqNew->VolumeCondition = THOST_FTDC_VC_AV;
							break;
						}

						case TimeInForce::Identifier::ID_FOK:
						{
							_message->reqNew->TimeInForce = TAPI_ORDER_TIMEINFORCE_FOK;
							// _message->reqNew->VolumeCondition = THOST_FTDC_VC_CV;
							break;
						}

						case TimeInForce::Identifier::ID_DAY:
						default:
						{
							_message->reqNew->TimeInForce = TAPI_ORDER_TIMEINFORCE_GFD;
							// _message->reqNew->VolumeCondition = THOST_FTDC_VC_AV;
							break;
						}
					}
					break;
				}
				case ByteField::Identifier::ID_ORDER_POSITION_TYPE:
				{
					const PositionType* positionType = PositionType::getPositionType(_bytes[_offset]);
					switch(positionType->ID)
					{
						case PositionType::Identifier::ID_CLOSE:
						{
							_message->reqNew->PositionEffect = TAPI_PositionEffect_COVER;
							break;
						}
						case PositionType::Identifier::ID_CLOSE_TODAY:
						{
							_message->reqNew->PositionEffect = TAPI_PositionEffect_COVER_TODAY;
							break;
						}
						//TODO: TAP API not support close yesterday
						// case PositionType::Identifier::ID_CLOSE_OVERNIGHT:
						// {
						// 	_message->reqNew->PositionEffect = THOST_FTDC_OF_CloseYesterday;
						// 	break;
						// }
						case PositionType::Identifier::ID_OPEN:
						default:
						{
							_message->reqNew->PositionEffect = TAPI_PositionEffect_OPEN;
							break;
						}
					}
					break;
				}
				default:
				{
					break;
				}
			}
		}

	private:
		SecurityCache* securityCache;
		boost::unordered_set<Shortname>* arbitrageProducts;
};

#endif /* TAPORDERENTRYIMPL_H */