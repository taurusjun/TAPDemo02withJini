/* 
 * File:   TAPMarketDataImpl.h
 * Author: jeremy
 *
 * Created on May 12, 2019, 16:20 
 */

#ifndef TAPMARKETDATAIMPL_H
#define TAPMARKETDATAIMPL_H

#include "TapQuoteAPI.h"
#include "TapAPIError.h"
#include "TAPSimpleEvent.h"

#include "jni/JNIThreadManager.h"
#include "codec/ByteCodec.hpp"
#include "codec/EncoderBinding.h"
#include "codec/CoreMessageEncoder.hpp"
#include "message/marketdata/MarketDataAction.h"
#include "message/orderentry/Side.h"
#include "message/statistics/bindings/StatisticsMessageEncoderBinding.hpp"
#include "instrument/SecurityDefinition.hpp"

#include <iostream>
#include <sstream>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <chrono>
#include <boost/thread/mutex.hpp>
#include <boost/unordered_set.hpp>
#include <codec/RawByteField.hpp>

#if ENABLE_RECORDING == 1
#include <io/AsyncOut.h>
#endif

typedef std::string Shortname;
typedef std::string SecurityID;

typedef void(*CallbackFunc)(long,int);
typedef void(*LoggerFunc)(std::string);
typedef bool(*ShortnameLookupFunc)(std::string,SecurityDefinition*);
typedef bool(*SecurityIDLookupFunc)(std::string,std::string,SecurityDefinition*);

class TAPMarketDataMessageEncoderBinding;

class TAPMarketDataImpl : public ITapQuoteAPINotify
{
	public:
		TAPMarketDataImpl(JNIThreadManager*,CallbackFunc,SecurityIDLookupFunc,LoggerFunc);
		virtual ~TAPMarketDataImpl();

		void configure(std::string, std::string, std::string, std::string, std::string);

		bool connect();
		void disconnect();

		void subscribe(std::string);
		void unsubscribe(std::string);

		//implementation of ITapQuoteAPINotify
		virtual void TAP_CDECL OnRspLogin(TAPIINT32 errorCode, const TapAPIQuotLoginRspInfo *info);
		virtual void TAP_CDECL OnAPIReady();
		virtual void TAP_CDECL OnDisconnect(TAPIINT32 reasonCode);
		virtual void TAP_CDECL OnRspChangePassword(TAPIUINT32 sessionID, TAPIINT32 errorCode);
		virtual void TAP_CDECL OnRspQryExchange(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIExchangeInfo *info);
		virtual void TAP_CDECL OnRspQryCommodity(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIQuoteCommodityInfo *info);
		virtual void TAP_CDECL OnRspQryContract(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIQuoteContractInfo *info);
		virtual void TAP_CDECL OnRtnContract(const TapAPIQuoteContractInfo *info);
		virtual void TAP_CDECL OnRspSubscribeQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIQuoteWhole *info);
		virtual void TAP_CDECL OnRspUnSubscribeQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIContract *info);
		virtual void TAP_CDECL OnRtnQuote(const TapAPIQuoteWhole *info);

		inline
		static std::string getErrorString(TAPIINT32 errorCode)
		{
			std::ostringstream os;
			os << "[ErrorCode: ] " << errorCode;
			return os.str();
		}

		inline
		static bool isEmpty(long _value)
		{
			return abs(_value - LONG_MIN) < 2;
		}

	private:
		TAPSimpleEvent m_Event;
		bool		m_bIsAPIReady;

		ITapQuoteAPI* mdApi;

		TAPMarketDataMessageEncoderBinding* encBinding;
		StatisticsMessageEncoderBinding* encStatBinding;

		char* fieldPresence;
		int fpLength;

		char* fieldPresence_Statistics_Connection;
		int fpLength_Statistics_Connection;

		boost::unordered_set<std::string>* securities;
		boost::unordered_map<SecurityID, SecurityDefinition*>* securityCache;
		boost::mutex subscriptionMutex;

		std::string address;
		std::string brokerID;
		std::string userID;
		std::string password;
		std::string exdest;

		unsigned int reqID;

		JNIThreadManager* jniThreadManager;
		NativeThreadID mainCallbackThreadID;

		void(*logger)(std::string);
		void(*callback)(long,int);
		bool(*security)(std::string,std::string,SecurityDefinition*);

		bool login();
		int getNextRequestID();
		void notifyConnectionStatus(bool);

		#if ENABLE_RECORDING == 1
		AsyncOut* async;
		#endif
};

inline
static std::string getInstrumentIDFromTapQuoteMsg(const TapAPIQuoteWhole* _message){
	std::ostringstream os;
	os << _message->Contract.Commodity.CommodityType << _message->Contract.Commodity.CommodityNo;
	return os.str();
}

class TAPMarketDataMessageEncoderBinding : public EncoderBinding<TapAPIQuoteWhole>
{
	public:
		TAPMarketDataMessageEncoderBinding(boost::unordered_map<SecurityID, SecurityDefinition*>* _securityCache)
		{
			this->securityCache = _securityCache;
		}
		~TAPMarketDataMessageEncoderBinding(){}

		virtual char getByteValue(const ByteField* const _field, unsigned int _idx, TapAPIQuoteWhole* _message)
		{
			return 0;
		}
		virtual short getShortValue(const ByteField* const _field, unsigned int _idx, TapAPIQuoteWhole* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_PRICE_LEVELS:
				{
					return 5;
				}
				default:
				{
					return 0;
				}
			}
		}
		virtual int getIntValue(const ByteField* const _field, unsigned int _idx, TapAPIQuoteWhole* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_MARKET_DATA_ACTION:
				{
					return MarketDataAction::DEPTH_UPDATE->value;
				}
				case ByteField::Identifier::ID_PRICE_EXPONENT:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return definition->pxExponent * -1;
				}
				case ByteField::Identifier::ID_DEPTH_BID_SIZE:
				{
					return _message->QBidQty[_idx];
				}
				case ByteField::Identifier::ID_DEPTH_OFFER_SIZE:
				{
					return _message->QAskQty[_idx];
				}
				default:
				{
					return 0;
				}
			}
			return 0;
		}
		virtual long getLongValue(const ByteField* const _field, unsigned int _idx, TapAPIQuoteWhole* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_TIME:
				{
					return getTimeStamp();
				}
				case ByteField::Identifier::ID_DEPTH_BID_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QBidPrice[_idx], definition->pxExponent);
					break;
				}
				case ByteField::Identifier::ID_DEPTH_OFFER_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QAskPrice[_idx], definition->pxExponent);
					break;
				}
				case ByteField::Identifier::ID_LAST_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QLastPrice, definition->pxExponent);
				}
				case ByteField::Identifier::ID_HIGH_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QHighPrice, definition->pxExponent);
				}
				case ByteField::Identifier::ID_LOW_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QLowPrice, definition->pxExponent);
				}
				case ByteField::Identifier::ID_UPPER_LIMIT:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QLimitUpPrice, definition->pxExponent);
				}
				case ByteField::Identifier::ID_LOWER_LIMIT:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QLimitDownPrice, definition->pxExponent);
				}
				case ByteField::Identifier::ID_OPEN_INTEREST:
				{
					return (long)_message->QPositionQty;
				}
				case ByteField::Identifier::ID_TURNOVER:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QTotalTurnover, definition->pxExponent);
				}
				case ByteField::Identifier::ID_VOLUME:
				{
					return (long)_message->QTotalQty;
				}
				case ByteField::Identifier::ID_OPEN_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QOpeningPrice, definition->pxExponent);
				}
				case ByteField::Identifier::ID_CLOSE_PRICE:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return getMantissa(_message->QClosingPrice, definition->pxExponent);
				}
				default:
				{
					return 0;
				}
			}
			return 0;
		}
		virtual std::string getStringValue(const ByteField* const _field, unsigned int _idx, TapAPIQuoteWhole* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_SHORTNAME:
				{
					SecurityDefinition* definition = this->securityCache->at(getInstrumentIDFromTapQuoteMsg(_message));
					return definition->shortname;
				}

				default:
				{
					return "";
				}
			}
		}
		virtual short getVarDataLength(const ByteField* const _field, unsigned int _idx, TapAPIQuoteWhole* _message)
		{
			switch(_field->ID)
			{
				case ByteField::Identifier::ID_DEPTH_BID_PRICE_LIST:
				{
					return getDepthCount(_message, Side::Identifier::ID_BUY);
				}
				case ByteField::Identifier::ID_DEPTH_BID_SIZE_LIST:
				{
					return getDepthCount(_message, Side::Identifier::ID_BUY);
				}
				case ByteField::Identifier::ID_DEPTH_OFFER_PRICE_LIST:
				{
					return getDepthCount(_message, Side::Identifier::ID_SELL);
				}
				case ByteField::Identifier::ID_DEPTH_OFFER_SIZE_LIST:
				{
					return getDepthCount(_message, Side::Identifier::ID_SELL);
				}
				default:
				{
					return 0;
				}
			}
		}

	private:
		inline
		int getDepthCount(TapAPIQuoteWhole* _message, Side::Identifier _side)
		{
			switch(_side)
			{
				case Side::Identifier::ID_BUY:
				{
					if(TAPMarketDataImpl::isEmpty(_message->QBidPrice[0]))
					{
						return 0;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QBidPrice[1]))
					{
						return 1;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QBidPrice[2]))
					{
						return 2;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QBidPrice[3]))
					{
						return 3;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QBidPrice[4]))
					{
						return 4;
					}
					return 5;
				}
				case Side::Identifier::ID_SELL:
				{
					if(TAPMarketDataImpl::isEmpty(_message->QAskPrice[0]))
					{
						return 0;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QAskPrice[1]))
					{
						return 1;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QAskPrice[2]))
					{
						return 2;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QAskPrice[3]))
					{
						return 3;
					}
					else if(TAPMarketDataImpl::isEmpty(_message->QAskPrice[4]))
					{
						return 4;
					}
					return 5;
				}
				default:
				{
				}
			}
			return 0;
		}

	private:
		boost::unordered_map<SecurityID, SecurityDefinition*>* securityCache;

		static long getMantissa(double _raw, int _exponent)
		{
			return (long)round(_raw * pow(10, _exponent));
		}

		static unsigned long getTimeStamp()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}
};

#endif /* TAPMARKETDATAIMPL_H */

