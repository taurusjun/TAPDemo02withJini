#include "TAPOrderEntryImpl.h"

#define NUM_FIELDS_ORDER_ACK				2
#define NUM_FIELDS_ORDER_REJ				4
#define NUM_FIELDS_ORDER_FILL				7
#define NUM_FIELDS_STATISTICS_POSITION 		5
#define NUM_FIELDS_STATISTICS_ACCOUNT	 	5
#define NUM_FIELDS_STATISTICS_CONNECTION 	2
#define MSG_BUFFER_SIZE 					512

using namespace std;
using namespace boost;
TAPOrderEntryImpl::TAPOrderEntryImpl(JNIThreadManager* _manager, CallbackFunc _callbackFunc, SecurityIDLookupFunc _securityFuncByID, ShortnameLookupFunc _securityFuncByShortname, LoggerFunc _loggerFunc)
{
	this->jniThreadManager = _manager;

	this->traderApi = 0;

	this->securityCacheByShortname = new boost::unordered_map<Shortname, SecurityDefinition*>(8);
	this->securityCacheBySecurityID = new boost::unordered_map<SecurityID, SecurityDefinition*>(8);

	this->arbitrageProducts = new boost::unordered_set<Shortname>();

	this->orderIDToRefIDQueue = new boost::lockfree::spsc_queue<TAPOrderIDMapping>(1024);
	this->orderIDToRefIDMap = new boost::unordered_map<std::string, int>();

	this->refIDToOrderIDQueue = new boost::lockfree::spsc_queue<TAPOrderIDMapping>(1024);
	this->refIDToOrderIDMap = new boost::unordered_map<int, std::string>();

	this->refIDToOrderStatusQueue = new boost::lockfree::spsc_queue<TAPOrderStatusMapping>(1024);
	this->refIDToOrderStatusMap = new boost::unordered_map<int, TAPOrderStatus>();

	this->refIDToOrderNoMap = new boost::unordered_map<int, std::string>();
	this->refIDToOrderActionMap = new boost::unordered_map<int, TAPOrderAction>();

	this->replaceQueue = new boost::lockfree::spsc_queue<TAPReplaceCompletion*>(1024);
	this->replaceFunctionMap = new boost::unordered_map<int, std::function<void()>>();

	this->encOrderBinding = new TAPOrderMessageEncoderBinding(this);
	this->decOrderBinding = new TAPOrderMessageDecoderBinding(this, this->arbitrageProducts);
	this->encStatBinding = new StatisticsMessageEncoderBinding();
	this->decStatBinding = new StatisticsMessageDecoderBinding();

	this->positionStaticMap = new boost::unordered_map<std::string, unsigned int>();

	const ByteField* fields_order_ack[NUM_FIELDS_ORDER_ACK] =
	{
		ByteField::ORDER_ACTION,
		ByteField::ORDER_ID
	};
	this->fieldPresence_Order_Ack = CoreMessageTemplate::buildPresence(this->fpLength_Order_Ack, CoreMessageType::ORDER_ENTRY_MESSAGE->mTemplate, fields_order_ack, NUM_FIELDS_ORDER_ACK);

	const ByteField* fields_order_rej[NUM_FIELDS_ORDER_REJ] =
	{
		ByteField::ORDER_ACTION,
		ByteField::ORDER_ID,
		ByteField::ORDER_REJECT_TYPE,
		ByteField::ORDER_REJECT_REASON
	};
	this->fieldPresence_Order_Rej = CoreMessageTemplate::buildPresence(this->fpLength_Order_Rej, CoreMessageType::ORDER_ENTRY_MESSAGE->mTemplate, fields_order_rej, NUM_FIELDS_ORDER_REJ);

	const ByteField* fields_order_fill[NUM_FIELDS_ORDER_FILL] =
	{
		ByteField::ORDER_ACTION,
		ByteField::ORDER_ID,
		ByteField::ORDER_EXECUTION_ID,
		ByteField::SHORTNAME,
		ByteField::ORDER_EXECUTED_QUANTITY,
		ByteField::PRICE_EXPONENT,
		ByteField::ORDER_FILL_PRICE
	};
	this->fieldPresence_Order_Fill = CoreMessageTemplate::buildPresence(this->fpLength_Order_Fill, CoreMessageType::ORDER_ENTRY_MESSAGE->mTemplate, fields_order_fill, NUM_FIELDS_ORDER_FILL);

	const ByteField* fields_statistics_position[NUM_FIELDS_STATISTICS_POSITION] =
	{
		ByteField::STATISTICS_ACTION,
		ByteField::SHORTNAME,
		ByteField::ORDER_POSITION_TYPE,
		ByteField::ORDER_SIDE,
		ByteField::ORDER_SIZE
	};
	this->fieldPresence_Statistics_Position = CoreMessageTemplate::buildPresence(this->fpLength_Statistics_Position, CoreMessageType::STATISTICS_MESSAGE->mTemplate, fields_statistics_position, NUM_FIELDS_STATISTICS_POSITION);

	const ByteField* fields_statistics_account[NUM_FIELDS_STATISTICS_POSITION] =
	{
		ByteField::STATISTICS_ACTION,
		ByteField::PRE_BALANCE,
		ByteField::CURRENT_BALANCE,
		ByteField::CURRENT_MARGIN,
		ByteField::AVAILABLE_MARGIN
	};
	this->fieldPresence_Statistics_Account = CoreMessageTemplate::buildPresence(this->fpLength_Statistics_Account, CoreMessageType::STATISTICS_MESSAGE->mTemplate, fields_statistics_account, NUM_FIELDS_STATISTICS_ACCOUNT);


	const ByteField* fields_statistics_connection[NUM_FIELDS_STATISTICS_CONNECTION] =
	{
		ByteField::STATISTICS_ACTION,
		ByteField::CONNECTION_STATUS
	};
	this->fieldPresence_Statistics_Connection = CoreMessageTemplate::buildPresence(this->fpLength_Statistics_Connection, CoreMessageType::STATISTICS_MESSAGE->mTemplate, fields_statistics_connection, NUM_FIELDS_STATISTICS_CONNECTION);

	this->callback = _callbackFunc;
	this->securityByID = _securityFuncByID;
	this->securityByShortname = _securityFuncByShortname;
	this->logger = _loggerFunc;

	this->sessionID = 0;
	this->frontID = 0;
	this->reqID = 1;
	this->refID = 0;
}

TAPOrderEntryImpl::~TAPOrderEntryImpl()
{
	this->traderApi->SetAPINotify(NULL);
	FreeTapTradeAPI(this->traderApi);	
	this->traderApi = 0;
}

void TAPOrderEntryImpl::configure(std::string _address, std::string _port, std::string _brokerID, std::string _userID, std::string _password, std::string _exdest, std::string _authcode)
{
	this->address = _address;
	this->port = (unsigned short) strtoul(_port.c_str(), NULL, 0);;
	this->brokerID = _brokerID;
	this->userID = _userID;
	this->password = _password;
	this->exdest = _exdest;
	this->authcode=_authcode;

	std::string exchangeNo = _exdest;
	boost::replace_first(exchangeNo, "TAP-","");
	this->exchangeNo = exchangeNo;

	this->loadSecurityCache("conf/TAP/tap-security-cache.txt");
}

bool TAPOrderEntryImpl::connect()
{
	if(this->traderApi != 0)
	{
		FreeTapTradeAPI(this->traderApi);	
		this->traderApi = 0;
	}

	this->logger("[TAP-TRADE] Connecting to TAP Trader API...");

    TAPIINT32 iResult = TAPIERROR_SUCCEED;
    TapAPIApplicationInfo stAppInfo;
    strcpy(stAppInfo.AuthCode, this->authcode.c_str());
	strcpy(stAppInfo.KeyOperationLogPath, "log");
	//create API
	this->traderApi = CreateTapTradeAPI(&stAppInfo, iResult);
	if(NULL == traderApi) {
		std::ostringstream os1;
		os1 << "Create API failed!" <<endl;
		this->logger(os1.str());
		return false;
	}
	//set notify
	this->traderApi->SetAPINotify(this);

	TAPIINT32 iErr = TAPIERROR_SUCCEED;
	bool rslt = true;
	iErr = this->traderApi->SetHostAddress(this->address.c_str(), this->port);
	if(TAPIERROR_SUCCEED != iErr) {
		std::ostringstream os1;
		os1 << "SetHostAddress Error:" << iErr <<endl;
		this->logger(os1.str());
		return false;
	}else{
        rslt = this->login();
	}

	if (!rslt)
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Error: login failed!" ;
		this->logger(os.str());
		return false;
	}	

	std::ostringstream os;
	os << "[TAP-TRADE] TradeAPI Info: " << GetTapTradeAPIVersion();
	this->logger(os.str());

	//TODO replace this with a latch
	boost::this_thread::sleep(boost::posix_time::milliseconds(5000));

	//QryPosition
	TAPIUINT32 m_uiSessionID = this->getNextRequestID();
	std::ostringstream os2;
	os2 << "[TAP-TRADE] Retrieving position [reqID=" << reqID << "] begin";
	this->logger(os2.str());

	TapAPIPositionQryReq stQryPosReq;
	memset(&stQryPosReq, 0, sizeof(stQryPosReq));
	iErr = this->traderApi->QryPosition(&m_uiSessionID,&stQryPosReq);
	if(TAPIERROR_SUCCEED != iErr) {
		os << "QryPosition Error:" << iErr <<endl;
		this->logger(os.str());
		return false;
	}

	//comment out the following sleep line and use event notification 
	// boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
	this->m_Event.WaitEvent();
	std::ostringstream os3;
	os3 << "[TAP-TRADE] Retrieving position [reqID=" << reqID << "] success!";
	this->logger(os3.str());
	
	return true;
}

void TAPOrderEntryImpl::disconnect()
{
	this->logger("[TAP-TRADE] Disconnecting from TAP Trader API...");
	FreeTapTradeAPI(this->traderApi);
	this->traderApi = 0;

	boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
}

bool TAPOrderEntryImpl::login()
{
	this->logger("[TAP-TRADE] Logging in to TAP Trader API...");

	TapAPITradeLoginAuth stLoginAuth;
	memset(&stLoginAuth, 0, sizeof(stLoginAuth));
	strcpy(stLoginAuth.UserNo, this->userID.c_str());
	strcpy(stLoginAuth.Password, this->password.c_str());
	stLoginAuth.ISModifyPassword = APIYNFLAG_NO;
	stLoginAuth.ISDDA = APIYNFLAG_NO;
	TAPIINT32 iErr = TAPIERROR_SUCCEED;
	iErr = traderApi->Login(&stLoginAuth);
	if(TAPIERROR_SUCCEED != iErr) {
        ostringstream os;
		os << "[TAP-TRADER] Error logging: " << iErr;
		this->logger(os.str());
		return false;
	}

	// //Waiting for APIReady
	this->m_Event.WaitEvent();
	if (!this->m_bIsAPIReady){
		this->logger("[TAP-TRADER] TradeAPI not ready, abort!");
		return false;
	}

	this->logger("[TAP-TRADER] TradeAPI is ready.");
	return true;
}

void TAPOrderEntryImpl::retrievePositions()
{
	for(auto iter = this->securityCacheBySecurityID->begin(); iter != this->securityCacheBySecurityID->end(); ++iter)
	{
		std::string securityID = iter->first;

		this->resetPosition(securityID, Side::BUY, PositionType::OPEN_TODAY, 0);
		this->resetPosition(securityID, Side::BUY, PositionType::OPEN_OVERNIGHT, 0);

		this->resetPosition(securityID, Side::SELL, PositionType::OPEN_TODAY, 0);
		this->resetPosition(securityID, Side::SELL, PositionType::OPEN_OVERNIGHT, 0);
	}
}

//update postions from history data
void TAPOrderEntryImpl::resetPosition(std::string _securityID, const Side* _side, const PositionType* _type, int _size)
{
	if(CollectionsHelper::containsKey(this->securityCacheBySecurityID, _securityID))
	{
		StatisticsMessage update;
		update.statisticsAction = StatisticsAction::POSITION_RESET;
		update.shortname = (*this->securityCacheBySecurityID)[_securityID]->shortname;
		update.positionSide = _side;
		update.positionType = _type;
		update.positionSize = _size;
		this->updatePosition(&update);
	}	
}

//Query account fund info
void TAPOrderEntryImpl::retrieveAccountDetails()
{
	if(this->traderApi != 0)
	{
		TapAPIFundReq stFundReq;
		memset(&stFundReq, 0, sizeof(stFundReq));
		strcpy(stFundReq.AccountNo, this->userID.c_str());
		TAPIUINT32 m_uiSessionID = this->getNextRequestID();
		this->traderApi->QryFund(&m_uiSessionID, &stFundReq);
	}
}

//update account fund info
void TAPOrderEntryImpl::resetAccountDetails(double _preBalance, double _currBalance, double _currMargin, double _availableMargin)
{
	StatisticsMessage update;
	update.statisticsAction = StatisticsAction::TRADING_ACCOUNT_INFO_UPDATE;
	update.preBalance = _preBalance;
	update.currBalance = _currBalance;
	update.currMargin = _currMargin;
	update.availableMargin = _availableMargin;
	this->updateAccountDetails(&update);
}

void TAPOrderEntryImpl::notifyConnectionStatus(bool _connected)
{
	StatisticsMessage update;
	update.statisticsAction = StatisticsAction::CONNECTION_STATUS_UPDATE;
	update.connected = _connected;
	this->updateConnectionStatus(&update);
}

void TAPOrderEntryImpl::loadSecurityCache(std::string _cacheFilePath)
{
	boost::mutex::scoped_lock lock(this->securityCacheFileMutex);

	boost::filesystem::path full_path(boost::filesystem::current_path());
	std::ostringstream os0;
	os0 << "Reading security cache file from:"<< full_path.c_str()<< "/"<< _cacheFilePath;
	this->logger(os0.str());

	std::ifstream securityCacheFile;
	securityCacheFile.open(_cacheFilePath);

	if(!securityCacheFile.good())
	{
		std::ostringstream os;
		os << "File "<< _cacheFilePath << " not exist" << endl;;
		this->logger(os.str());
		return;
	}

	std::string line;
	while(!securityCacheFile.eof())
	{
		getline(securityCacheFile, line);
		if(line.length() > 0)
		{
			bool isArb = false;
			std::string securityID;
			std::string::size_type indexOf = line.find(':');
			if(indexOf != std::string::npos)
			{
				securityID = line.substr(0, indexOf);
				std::string spec = line.substr(indexOf);
				if(spec.compare(":A") == 0)
				{
					isArb = true;
				}
			}
			else
			{
				securityID = line;
			}

			SecurityDefinition* definition = new SecurityDefinition();
			if(this->securityByID(securityID, this->exdest, definition))
			{
				std::ostringstream os;
				os << "[TAP-TRADE] Caching security for [id=" << securityID << "][shortname=" << definition->shortname << "][isArb=" << isArb << "]";
				this->logger(os.str());

				(*this->securityCacheBySecurityID)[definition->securityID] = definition;
				(*this->securityCacheByShortname)[definition->shortname] = definition;

				if(isArb)
				{
					this->arbitrageProducts->insert(definition->shortname);
				}
			}
			else
			{
				std::ostringstream os;
				os << "[TAP-TRADE] Unable to retrieve security for [id=" << securityID << "]";
				this->logger(os.str());
			}
		}
	}
}

void TAPOrderEntryImpl::request(char* _msg, int _length)
{
	int msgTypeValue = ByteCodec::getInt(_msg, CoreMessageTemplate::MESSAGE_TYPE_OFFSET);
	const CoreMessageType* msgType = CoreMessageType::fromValue(msgTypeValue);

	if(msgType->value == CoreMessageType::ORDER_ENTRY_MESSAGE->value)
	{
		TAPOrderRequest req;
		TapAPINewOrder order;
		TapAPIOrderCancelReq cancel;

		memset(&order, 0, sizeof(order));
		memset(&cancel, 0, sizeof(cancel));

		req.reqNew = &order;
		req.reqCancel = &cancel;

		CoreMessageDecoder::decode(_msg, _length, this->decOrderBinding, &req);

		switch(req.orderAction->ID)
		{
			case OrderAction::Identifier::ID_NEW:
			{
				int refID = this->getNextRefID();

				TAPOrderIDMapping idMapping;
				idMapping.refID = refID;
				idMapping.orderID = req.orderID;
				this->refIDToOrderIDQueue->push(idMapping);

				TAPOrderStatusMapping statusMapping;
				statusMapping.refID = refID;
				statusMapping.status = TAPOrderStatus::PENDING_NEW;
				this->refIDToOrderStatusQueue->push(statusMapping);

				this->enrichNew(req.reqNew, refID);
				this->printNewOrderRequest(req.reqNew);
				TAPIUINT32 newReqID = this->getNextRequestID();
				this->traderApi->InsertOrder(&newReqID, req.reqNew);
				break;
			}
			case OrderAction::Identifier::ID_REPLACE:
			{
				this->updateOrderIDToRefIDMapping();
				if(!CollectionsHelper::containsKey(this->orderIDToRefIDMap, req.orderID))
				{
					std::ostringstream os;
					os << "[TAP-TRADE] Error: Unable to retrieve refID in OrderActionMap [orderID=" << req.orderID << "][OrderAction=REPLACE]"<<endl;
					this->logger(os.str());
					this->actionCallback(OrderAction::INTERNAL_REJECT, req.orderID, RejectType::CANCEL, "No refered cancel order");
					return;
				}
				int cancelRefID = this->orderIDToRefIDMap->at(req.orderID);

				TAPIUINT32 cancelReqID = this->getNextRequestID();

				TAPReplaceCompletion* completion = new TAPReplaceCompletion();
				completion->refID = cancelRefID;

				TapAPINewOrder* reqNew = new TapAPINewOrder();
				copyNewRequest(reqNew, req.reqNew);

				completion->func = [=]()
				{
					std::string orderID = req.orderID;
					TapAPINewOrder* replaceReqNew = reqNew;
					TAPIUINT32 newReqID = this->getNextRequestID();
					int refID = this->getNextRefID();

					(*this->refIDToOrderIDMap)[refID] = orderID;
					(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::PENDING_REPLACE_PHASE_2;

					this->enrichNew(replaceReqNew, refID);
					this->printNewOrderRequest(replaceReqNew);
					this->traderApi->InsertOrder(&newReqID, replaceReqNew);

					delete replaceReqNew;
				};
				this->replaceQueue->push(completion);

				TAPOrderStatusMapping statusMapping;
				statusMapping.refID = cancelRefID;
				statusMapping.status = TAPOrderStatus::PENDING_REPLACE_PHASE_1;
				this->refIDToOrderStatusQueue->push(statusMapping);

				this->enrichCancel(req.reqCancel, cancelRefID);
				this->printCancelOrderRequest(req.reqCancel);
				this->traderApi->CancelOrder(&cancelReqID, req.reqCancel);
				break;
			}
			case OrderAction::Identifier::ID_CANCEL:
			{
				this->updateOrderIDToRefIDMapping();

				TAPIUINT32 cancelReqID = this->getNextRequestID();

				if(!CollectionsHelper::containsKey(this->orderIDToRefIDMap, req.orderID))
				{
					std::ostringstream os;
					os << "[TAP-TRADE] Error: Unable to retrieve refID in OrderActionMap [orderID=" << req.orderID << "][OrderAction=CANCEL]"<<endl;
					this->logger(os.str());
					this->actionCallback(OrderAction::INTERNAL_REJECT, req.orderID, RejectType::CANCEL, "No refered cancel order");
					return;
				}
				int refID = this->orderIDToRefIDMap->at(req.orderID);

				TAPOrderStatusMapping statusMapping;
				statusMapping.refID = refID;
				statusMapping.status = TAPOrderStatus::PENDING_CANCEL;
				this->refIDToOrderStatusQueue->push(statusMapping);

				this->enrichCancel(req.reqCancel, refID);
				this->printCancelOrderRequest(req.reqCancel);
				this->traderApi->CancelOrder(&cancelReqID, req.reqCancel);
				break;
			}
			default:
			{
				std::ostringstream os;
				os << "[TAP-TRADE] Error: Unable to handle request with action ID: " << req.orderAction->value;
				this->logger(os.str());
				break;
			}
		}
	}
	else if(msgType->value == CoreMessageType::STATISTICS_MESSAGE->value)
	{
		StatisticsMessage update;
		CoreMessageDecoder::decode(_msg, _length, this->decStatBinding, &update);

		switch(update.statisticsAction->ID)
		{
			case StatisticsAction::Identifier::ID_TRADING_ACCOUNT_INFO_REQUEST:
			{
				this->retrieveAccountDetails();
				break;
			}
			default:
			{
				break;
			}
		}
	}
}

void TAPOrderEntryImpl::enrichNew(TapAPINewOrder* _new, int _refID)
{
	strcpy(_new->AccountNo, this->userID.c_str());
	_new->RefInt = _refID;

	_new->OrderMinQty = 1;
	_new->TacticsType = TAPI_TACTICS_TYPE_NONE;//Immediately

	//------------- fill info ----------------
	strcpy(_new->ExchangeNo, this->exchangeNo.c_str());
	_new->CommodityType = TAPI_COMMODITY_TYPE_FUTURES;
	strcpy(_new->StrikePrice, "");		
	_new->CallOrPutFlag = TAPI_CALLPUT_FLAG_NONE;		
	strcpy(_new->ContractNo2, "");		
	strcpy(_new->StrikePrice2, "");		
	_new->CallOrPutFlag2 = TAPI_CALLPUT_FLAG_NONE;	
	_new->OrderType = TAPI_ORDER_TYPE_MARKET;//			
	// _new->OrderType = TAPI_ORDER_TYPE_LIMIT;//			
	_new->OrderSource = TAPI_ORDER_SOURCE_ESUNNY_API;		
	_new->TimeInForce = TAPI_ORDER_TIMEINFORCE_GFD;		
	strcpy(_new->ExpireTime, "");		
	_new->IsRiskOrder = APIYNFLAG_NO;		
				
	_new->PositionEffect2 = TAPI_PositionEffect_NONE;	
	strcpy(_new->InquiryNo,"");			

	_new->OrderPrice2;		
	_new->StopPrice;			
	_new->MinClipSize;		
	_new->MaxClipSize;		
	_new->RefInt;				
	strcpy(_new->RefString, "");//TODO			

	_new->TriggerCondition = TAPI_TRIGGER_CONDITION_NONE;	
	_new->TriggerPriceType = TAPI_TRIGGER_PRICE_NONE;	

	_new->AddOneIsValid = APIYNFLAG_NO;	
	_new->OrderQty2;
	_new->HedgeFlag2 = TAPI_HEDGEFLAG_NONE;
	_new->MarketLevel = TAPI_MARKET_LEVEL_0;
	_new->FutureAutoCloseFlag = APIYNFLAG_NO; // V9.0.2.0 20150520

	(*this->refIDToOrderActionMap)[_refID]=TAPOrderAction::INSERT;
}

void TAPOrderEntryImpl::enrichCancel(TapAPIOrderCancelReq* _cancel, int _refID)
{
	_cancel->RefInt = _refID;
	string orderNo = (*this->refIDToOrderNoMap)[_refID];
	strcpy(_cancel->OrderNo, orderNo.c_str());
	_cancel->ServerFlag='C';
	_cancel->RefString;//TODO

	(*this->refIDToOrderActionMap)[_refID]=TAPOrderAction::CANCEL;
}

int TAPOrderEntryImpl::getNextRequestID()
{
	return this->reqID++;
}

int TAPOrderEntryImpl::getNextRefID()
{
	return this->refID++;
}

void TAP_CDECL TAPOrderEntryImpl::OnRspLogin( TAPIINT32 errorCode, const TapAPITradeLoginRspInfo *loginRspInfo )
{
	if(TAPIERROR_SUCCEED == errorCode) {
		this->logger("[TAP-TRADE] Login success, waiting for API ready...");

		//register current thread as call back thread
		NativeThreadID id = boost::this_thread::get_id();
		this->jniThreadManager->registerJNIThreadName(id, "tap-trade-callback");
		this->jniThreadManager->getNativeThreadEnv(id);
		this->mainCallbackThreadID = id;
	} else {
		this->logger("[TAP-TRADE] Login failed!");
		this->m_Event.SignalEvent();	
	}
}

void TAP_CDECL TAPOrderEntryImpl::OnAPIReady()
{	
	//requesting positions too soon after startup can result in an error
	boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
	this->retrievePositions();
	this->retrieveAccountDetails();

	m_bIsAPIReady = true;
	this->m_Event.SignalEvent();	
	this->notifyConnectionStatus(true);
}

void TAP_CDECL TAPOrderEntryImpl::OnDisconnect( TAPIINT32 reasonCode )
{
	std::ostringstream os;
	os << "[TAP-TRADE] Disconnected from TAP TRADER API: " << reasonCode;
	this->logger(os.str());

	std::string threadName = "tap-trade-terminate";
	NativeThreadID id = boost::this_thread::get_id();
	this->jniThreadManager->registerJNIThreadName(id, threadName);
	this->jniThreadManager->getNativeThreadEnv(id);
	this->notifyConnectionStatus(false);

	if(!this->jniThreadManager->releaseNativeThreadEnv(id))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Error releasing native thread [name=" << threadName << "]";
		this->logger(os.str());
	}

	if(!this->jniThreadManager->releaseNativeThreadEnv(this->mainCallbackThreadID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Error releasing native thread [name=" << this->jniThreadManager->getJNIThreadName(this->mainCallbackThreadID) << "]";
		this->logger(os.str());
	}
}

void TAP_CDECL TAPOrderEntryImpl::OnRspQryPosition( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIPositionInfo *info )
{
	std::ostringstream os;
	if(NULL == info)
	{
		os << "[TAP-TRADE] Received flat position [reqID=" << sessionID << "]";
		this->logger(os.str());
	}
	else 
	{
		if(info->IsHistory == APIYNFLAG_YES) //History
		{	
			string c1=info->CommodityNo;
			string c2=info->ContractNo;
			string instrumentID=c1+c2;
			os << "[TAP-TRADE] Received historical position [securityID=" << instrumentID << "][date=" << info->MatchDate << "]";
			switch(info->MatchSide)
			{
				case TAPI_SIDE_BUY:
				{
					os << "[side=buy][PositionNo=" << info->PositionNo << "][overnight=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
					// this->calcPositions(instrumentID, Side::BUY, PositionType::OPEN_OVERNIGHT, info->PositionQty);
					this->resetPosition(instrumentID, Side::BUY, PositionType::OPEN_OVERNIGHT, info->PositionQty);
					break;
				}
				case TAPI_SIDE_SELL:
				{
					os << "[side=sell][PositionNo=" << info->PositionNo << "][overnight=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
					// this->calcPositions(instrumentID, Side::SELL, PositionType::OPEN_OVERNIGHT, info->PositionQty);
					this->resetPosition(instrumentID, Side::SELL, PositionType::OPEN_OVERNIGHT, info->PositionQty);
					break;
				}
				default:
				{
					break;
				}
			}
			this->logger(os.str());
		}
		else if(info->IsHistory == APIYNFLAG_NO) //Today
		{
			string c1(info->CommodityNo);
			string c2(info->ContractNo);
			string instrumentID=c1+c2;
			os << "[TAP-TRADE] Received current day position [securityID=" << instrumentID << "][date=" << info->MatchDate << "]";
			switch(info->MatchSide)
			{
				case TAPI_SIDE_BUY:
				{
					os << "[side=buy][PositionNo=" << info->PositionNo << "][today=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
					// this->calcPositions(instrumentID, Side::BUY, PositionType::OPEN_TODAY, info->PositionQty);
					this->resetPosition(instrumentID, Side::BUY, PositionType::OPEN_TODAY, info->PositionQty);
					break;
				}
				case TAPI_SIDE_SELL:
				{
					os << "[side=sell][PositionNo=" << info->PositionNo << "][today=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
					// this->calcPositions(instrumentID, Side::SELL, PositionType::OPEN_TODAY, info->PositionQty);
					this->resetPosition(instrumentID, Side::SELL, PositionType::OPEN_TODAY, info->PositionQty);
					break;
				}
				case TAPI_SIDE_NONE:
				default:
				{
				}
			}
			this->logger(os.str());
		}

		//all position info recieved
		if(isLast == APIYNFLAG_YES)
		{
			std::ostringstream os1;
			os1 << "****** Last position reveived *****";
			this->logger(os1.str());
			this->m_Event.SignalEvent();	
		}
	}
}

void TAPOrderEntryImpl::OnRspQryFund(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIFundData *info)
{
	if(info != NULL)
	{
		double preBalance = info->PreBalance;
		double balance = info->Balance;

		double currMargin = info->AccountIntialMargin;
		double availableMargin = info->Available;

		std::ostringstream os;
		os.precision(10);
		os << "[TAP-TRADE] Received trading account details [sessionID=" << sessionID << "][preBalance=" << preBalance << "][balance=" << balance << "][currMargin=" << currMargin << "][availableMargin=" << availableMargin << "]";
		this->logger(os.str());

		this->resetAccountDetails(preBalance, balance, currMargin, availableMargin);
	}
}

void TAP_CDECL TAPOrderEntryImpl::OnRtnOrder( const TapAPIOrderInfoNotice *info )
{
	if(NULL == info){
		return;
	}

	TapAPIOrderInfo* _order = info->OrderInfo;
	int refID = _order->RefInt;
	string orderNo = _order->OrderNo;
	(*this->refIDToOrderNoMap)[refID] = orderNo;

	TAPIOrderStateType orderStatus = _order->OrderState;

	if(!CollectionsHelper::containsKey(this->refIDToOrderActionMap, refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve action in OrderActionMap][refID=" << refID << "]"<<endl;
		this->logger(os.str());
		return;
	}
	TAPOrderAction tapOrderAction = (*this->refIDToOrderActionMap)[refID];

	if (info->ErrorCode != 0) //system error handling
	{
		std::ostringstream os0;
		os0 << "Order action error：" << info->ErrorCode << endl;
		this->logger(os0.str());
		switch(tapOrderAction){
			case TAPOrderAction::INSERT: //insert action
			{
				OnOrderInsertError(refID, info->ErrorCode);
			}
			case TAPOrderAction::CANCEL: // cancel action
			{
				OnOrderCancelError(refID, info->ErrorCode);
			}
		}		
		return;
	} 

	std::ostringstream os;
	os << "[TAP-TRADE] Received order status update [refID=" << refID << "][orderStatus=" << orderStatus << "]";
	this->logger(os.str());

	this->updateRefIDToOrderIDMapping();
	this->updateRefIDToOrderStatusMapping();
	this->updateRefIDToReplaceFunctionMapping();

	string errorMsg = "unknown";
	if(info->OrderInfo->ErrorText!=NULL || info->OrderInfo->ErrorText !=""){
		errorMsg = info->OrderInfo->ErrorText;
	}
	std::ostringstream oso1;
	oso1 <<"[SessionID=" <<info->SessionID<<"]";
	switch(tapOrderAction){
		case TAPOrderAction::INSERT: //insert action
		{
			oso1 << "[TAP-TRADE] Order insert returned [Order status="<<orderStatus<<"][Error code=" << info->OrderInfo->ErrorCode << "][Error message=" << errorMsg << "]";
			switch(orderStatus)
			{
				case TAPI_ORDER_STATE_QUEUED:
				case TAPI_ORDER_STATE_PARTFINISHED:
				case TAPI_ORDER_STATE_FINISHED: 
				{
					oso1 << "[action= ackNew]";
					this->ackNew(refID);
					break;
				}
				case TAPI_ORDER_STATE_FAIL:
				{
					if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
					{
						std::ostringstream os;
						os << "[TAP-TRADE] Unable to retrieve order status [OnRtnOrder()][refID=" << refID << "]";
						this->logger(os.str());
						return;
					}

					TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];
					if(status == TAPOrderStatus::PENDING_REPLACE_PHASE_2)
					{
						oso1 << "[action= ackCancel]";
						this->ackCancel(refID);
					}
					else
					{
						oso1 << "[action= reject][rejectType= new]";
						(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
						this->reject(refID, RejectType::NEW, errorMsg);
					}
					break;
				}
				case TAPI_ORDER_STATE_SUBMIT: 
				case TAPI_ORDER_STATE_ACCEPT: 
				case TAPI_ORDER_STATE_TRIGGERING: 
				case TAPI_ORDER_STATE_EXCTRIGGERING:
				case TAPI_ORDER_STATE_CANCELING:
				case TAPI_ORDER_STATE_MODIFYING:
				case TAPI_ORDER_STATE_CANCELED:
				case TAPI_ORDER_STATE_LEFTDELETED:
				case TAPI_ORDER_STATE_DELETED:
				case TAPI_ORDER_STATE_SUPPENDED:
				case TAPI_ORDER_STATE_DELETEDFOREXPIRE:
				case TAPI_ORDER_STATE_EFFECT:
				case TAPI_ORDER_STATE_APPLY:
					break;
			}
			break;//TAPOrderAction::INSERT
		}
		case TAPOrderAction::CANCEL://cancel action
		{
			oso1 << "[TAP-TRADE] Order cancel returned [Order status="<<orderStatus<<"][Error code=" << info->OrderInfo->ErrorCode << "]";
			if (0!= info->OrderInfo->ErrorCode)
			{
				if(info->OrderInfo->ErrorCode == TAPIERROR_ORDERDELETE_NOT_STATE)
				{
					string errorMsg=boost::locale::conv::from_utf<char>("此状态不允许撤单", "GB2312");
					oso1 << "[Error message=" << errorMsg << "][action= reject][rejectType= cancel]";
					(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
					this->reject(refID, RejectType::CANCEL, errorMsg);
				}else{
					oso1 << "[Error message=" << errorMsg << "][No action]";
				}
			}else
			{
				switch(orderStatus)
				{
					case TAPI_ORDER_STATE_CANCELED:
					case TAPI_ORDER_STATE_LEFTDELETED:
					// case TAPI_ORDER_STATE_DELETED: 
					case TAPI_ORDER_STATE_DELETEDFOREXPIRE:
					{
						oso1 << "[Error message=" << errorMsg << "][action= ackCancel]";
						this->ackCancel(refID);
						break;
					}
					case TAPI_ORDER_STATE_FAIL:
					{
						string errorMsg = "unknown";
						oso1 << "[Error message=" << errorMsg << "][action= reject][rejectType= cancel]";
						(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
						this->reject(refID, RejectType::CANCEL, errorMsg);
						break;
					}
					case TAPI_ORDER_STATE_SUBMIT: 
					case TAPI_ORDER_STATE_ACCEPT: 
					case TAPI_ORDER_STATE_TRIGGERING: 
					case TAPI_ORDER_STATE_EXCTRIGGERING:
					case TAPI_ORDER_STATE_QUEUED:
					case TAPI_ORDER_STATE_PARTFINISHED:
					case TAPI_ORDER_STATE_FINISHED:
					case TAPI_ORDER_STATE_CANCELING:
					case TAPI_ORDER_STATE_MODIFYING:
					case TAPI_ORDER_STATE_DELETED:
					case TAPI_ORDER_STATE_SUPPENDED:
					case TAPI_ORDER_STATE_EFFECT:
					case TAPI_ORDER_STATE_APPLY:
					{
						break;
					}
				}
			}
			break;//TAPOrderAction::CANCEL
		}
	}

	this->logger(oso1.str());
}

void TAP_CDECL TAPOrderEntryImpl::OnRtnFill( const TapAPIFillInfo *info )
// void TAPOrderEntryImpl::OnRtnTrade(CThostFtdcTradeField* _trade)
{
	int refID = atoi(info->OrderNo);

	std::ostringstream os;
	os << "[TAP-TRADE] Received trade update [refID=" << refID << "]";
	this->logger(os.str());

	this->updateRefIDToOrderIDMapping();
	this->updateRefIDToOrderStatusMapping();

	if(!CollectionsHelper::containsKey(this->refIDToOrderIDMap, refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order ID [OnRtnTrade()][refID=" << refID << "]";
		this->logger(os.str());
		return;
	}
	std::string orderID = (*this->refIDToOrderIDMap)[refID];

	TAPOrderUpdate update;
	update.orderAction = OrderAction::FILL;
	update.orderID = orderID;
	update.tradeUpdate = info;

	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::ORDER_ENTRY_MESSAGE, data, this->encOrderBinding, this->fieldPresence_Order_Fill, this->fpLength_Order_Fill, &update);
	long addr = (long)data;

	this->callback(addr, msgLen);
}

void TAPOrderEntryImpl::OnOrderInsertError(int refID, int errorCode ){
	std::string rejReason = getErrorString(errorCode);

	std::ostringstream os;
	os << "Error response (order insert): [OnOrderInsertError()][refID=" << refID << "][rejRsn=" << rejReason << "]";

	this->updateRefIDToOrderIDMapping();
	this->updateRefIDToOrderStatusMapping();

	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order status [OnOrderInsertError()][refID=" << refID << "]";
		this->logger(os.str());
		return;
	}

	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];

	switch(status)
	{
		case TAPOrderStatus::PENDING_NEW:
		{
			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
			os << "[action= reject][rejectType= new]";
			this->reject(refID, RejectType::NEW, rejReason);
			break;
		}
		case TAPOrderStatus::PENDING_REPLACE_PHASE_2:
		{
			os << "[action= cancel]";
			this->ackCancel(refID);
			break;
		}
		default:
		{
		}
	}
	this->logger(os.str());
}

void TAPOrderEntryImpl::OnOrderCancelError(int refID, int errorCode ){
	std::string rejReason = getErrorString(errorCode);

	std::ostringstream os;
	os << "[TAP-TRADE] Error response (order cancel): [OnOrderCancelError()][refID=" << refID << "][rejRsn=" << rejReason << "]";

	this->updateRefIDToOrderIDMapping();
	this->updateRefIDToOrderStatusMapping();

	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order status [OnOrderCancelError()][refID=" << refID << "]";
		this->logger(os.str());
		return;
	}

	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];

	switch(status)
	{
		case TAPOrderStatus::PENDING_REPLACE_PHASE_1:
		{
			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
			os << "[action= reject][rejectType= replace]";
			this->reject(refID, RejectType::REPLACE, rejReason);
			break;
		}
		case TAPOrderStatus::PENDING_CANCEL:
		{
			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
			os << "[action= reject][rejectType= cancel]";
			this->reject(refID, RejectType::CANCEL, rejReason);
			break;
		}
		default:
		{
		}
	}
	this->logger(os.str());
}

//------------------unimplemented functions ---------------
void TAP_CDECL TAPOrderEntryImpl::OnConnect()
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspChangePassword(TAPIUINT32 sessionID, TAPIINT32 errorCode)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspSetReservedInfo(TAPIUINT32 sessionID, TAPIINT32 errorCode, const TAPISTR_50 info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryAccount(TAPIUINT32 sessionID, TAPIUINT32 errorCode, TAPIYNFLAG isLast, const TapAPIAccountInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnFund(const TapAPIFundData *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryExchange(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIExchangeInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryCommodity(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPICommodityInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryContract(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPITradeContractInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnContract(const TapAPITradeContractInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspOrderAction(TAPIUINT32 sessionID, TAPIUINT32 errorCode, const TapAPIOrderActionRsp *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryOrder(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIOrderInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryOrderProcess(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIOrderInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryFill(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIFillInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnPosition(const TapAPIPositionInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryClose(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPICloseInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnClose(const TapAPICloseInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnPositionProfit(const TapAPIPositionProfitNotice *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryDeepQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIDeepQuoteQryRsp *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspQryExchangeStateInfo(TAPIUINT32 sessionID,TAPIINT32 errorCode, TAPIYNFLAG isLast,const TapAPIExchangeStateInfo * info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnExchangeStateInfo(const TapAPIExchangeStateInfoNotice * info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRtnReqQuoteNotice(const TapAPIReqQuoteNotice *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspUpperChannelInfo(TAPIUINT32 sessionID,TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIUpperChannelInfo * info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
void TAP_CDECL TAPOrderEntryImpl::OnRspAccountRentInfo(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIAccountRentInfo * info)
{
	// cout << __FUNCTION__ << " is called." << endl;
}
//---------------------------------------------------------

void TAPOrderEntryImpl::ackNew(int _refID)
{
	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, _refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order status [ackNew()][refID=" << _refID << "]";
		this->logger(os.str());
		return;
	}
	if(!CollectionsHelper::containsKey(this->refIDToOrderIDMap, _refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order ID [ackNew()][refID=" << _refID << "]";
		this->logger(os.str());
		return;
	}

	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[_refID];
	std::string orderID = (*this->refIDToOrderIDMap)[_refID];

	std::ostringstream os3;
	switch(status)
	{
		case TAPOrderStatus::PENDING_NEW:
		{
			os3 << "[TAP-TRADE] [ackNew()][status=pending]";
			(*this->refIDToOrderStatusMap)[_refID] = TAPOrderStatus::WORKING;

			TAPOrderIDMapping mapping;
			mapping.orderID = orderID;
			mapping.refID = _refID;
			this->orderIDToRefIDQueue->push(mapping);

			TAPOrderUpdate update;
			update.orderAction = OrderAction::NEW_ACK;
			update.orderID = orderID;

			char data[MSG_BUFFER_SIZE];
			int msgLen = CoreMessageEncoder::encode(CoreMessageType::ORDER_ENTRY_MESSAGE, data, this->encOrderBinding, this->fieldPresence_Order_Ack, this->fpLength_Order_Ack, &update);
			long addr = (long)data;

			this->callback(addr, msgLen);
			break;
		}
		case TAPOrderStatus::PENDING_REPLACE_PHASE_2:
		{
			os3 << "[TAP-TRADE] [ackNew()][status=PENDING_REPLACE_PHASE_2]";
			(*this->refIDToOrderStatusMap)[_refID] = TAPOrderStatus::WORKING;

			TAPOrderIDMapping mapping;
			mapping.orderID = orderID;
			mapping.refID = _refID;
			this->orderIDToRefIDQueue->push(mapping);

			TAPOrderUpdate update;
			update.orderAction = OrderAction::REPLACE_ACK;
			update.orderID = orderID;

			char data[MSG_BUFFER_SIZE];
			int msgLen = CoreMessageEncoder::encode(CoreMessageType::ORDER_ENTRY_MESSAGE, data, this->encOrderBinding, this->fieldPresence_Order_Ack, this->fpLength_Order_Ack, &update);
			long addr = (long)data;

			this->callback(addr, msgLen);
			break;
		}
		case TAPOrderStatus::PENDING_REPLACE_PHASE_1:
		case TAPOrderStatus::PENDING_CANCEL:
		case TAPOrderStatus::WORKING:
		case TAPOrderStatus::CANCELLED:
		case TAPOrderStatus::REJECTED:
		default:
		{
			break;
		}
	}
	this->logger(os3.str());

}

void TAPOrderEntryImpl::ackCancel(int _refID)
{
	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, _refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order status [ackCancel()][refID=" << _refID << "]";
		this->logger(os.str());
		return;
	}
	if(!CollectionsHelper::containsKey(this->refIDToOrderIDMap, _refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order ID [ackCancel()][refID=" << _refID << "]";
		this->logger(os.str());
		return;
	}

	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[_refID];
	std::string orderID = (*this->refIDToOrderIDMap)[_refID];

	switch(status)
	{
		case TAPOrderStatus::PENDING_REPLACE_PHASE_1:
		{
			std::function<void()> replacePhase2 = this->replaceFunctionMap->at(_refID);
			this->replaceFunctionMap->erase(_refID);
			replacePhase2();
			break;
		}
		case TAPOrderStatus::PENDING_REPLACE_PHASE_2: //produce unsolicited cancel from replace phase 2 rejection
		case TAPOrderStatus::PENDING_CANCEL:
		case TAPOrderStatus::PENDING_NEW: //cancel due to FAK
		case TAPOrderStatus::WORKING: //unsolicited cancel
		{
			(*this->refIDToOrderStatusMap)[_refID] = TAPOrderStatus::CANCELLED;

			TAPOrderUpdate update;
			update.orderAction = OrderAction::CANCEL_ACK;
			update.orderID = orderID;

			char data[MSG_BUFFER_SIZE];
			int msgLen = CoreMessageEncoder::encode(CoreMessageType::ORDER_ENTRY_MESSAGE, data, this->encOrderBinding, this->fieldPresence_Order_Ack, this->fpLength_Order_Ack, &update);
			long addr = (long)data;

			this->callback(addr, msgLen);
			break;
		}
		case TAPOrderStatus::CANCELLED:
		case TAPOrderStatus::REJECTED:
		default:
		{
			break;
		}
	}
}

void TAPOrderEntryImpl::reject(int _refID, const RejectType* _rejType, std::string _rejectReason)
{
	if(!CollectionsHelper::containsKey(this->refIDToOrderIDMap, _refID))
	{
		std::ostringstream os;
		os << "[TAP-TRADE] Unable to retrieve order ID [reject()][refID=" << _refID << "]";
		this->logger(os.str());
		return;
	}

	TAPOrderUpdate update;
	update.orderAction = OrderAction::EXTERNAL_REJECT;
	update.orderID = (*this->refIDToOrderIDMap)[_refID];
	update.rejectType = _rejType;
	update.rejectReason = _rejectReason;

	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::ORDER_ENTRY_MESSAGE, data, this->encOrderBinding, this->fieldPresence_Order_Rej, this->fpLength_Order_Rej, &update);
	long addr = (long)data;

	this->callback(addr, msgLen);
}

void TAPOrderEntryImpl::actionCallback(const OrderAction* _orderAction, string _orderID, const RejectType* _rejType, std::string _rejectReason)
{
	TAPOrderUpdate update;
	update.orderAction = _orderAction;
	update.orderID = _orderID;
	update.rejectType = _rejType;
	update.rejectReason = _rejectReason;

	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::ORDER_ENTRY_MESSAGE, data, this->encOrderBinding, this->fieldPresence_Order_Rej, this->fpLength_Order_Rej, &update);
	long addr = (long)data;

	this->callback(addr, msgLen);

}

void TAPOrderEntryImpl::updatePosition(StatisticsMessage* _update)
{
	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::STATISTICS_MESSAGE, data, this->encStatBinding, this->fieldPresence_Statistics_Position, this->fpLength_Statistics_Position, _update);
	long addr = (long)data;

	this->callback(addr, msgLen);
}

void TAPOrderEntryImpl::updateAccountDetails(StatisticsMessage* _update)
{
	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::STATISTICS_MESSAGE, data, this->encStatBinding, this->fieldPresence_Statistics_Account, this->fpLength_Statistics_Account, _update);
	long addr = (long)data;

	this->callback(addr, msgLen);
}

void TAPOrderEntryImpl::updateConnectionStatus(StatisticsMessage* _update)
{
	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::STATISTICS_MESSAGE, data, this->encStatBinding, this->fieldPresence_Statistics_Connection, this->fpLength_Statistics_Connection, _update);
	long addr = (long)data;

	this->callback(addr, msgLen);
}

bool TAPOrderEntryImpl::getSecurityDefinitionBySecurityID(std::string _securityID, SecurityDefinition* &_bucket)
{
	if(!CollectionsHelper::containsKey(this->securityCacheBySecurityID, _securityID))
	{
		SecurityDefinition* cachedDef = new SecurityDefinition();
		if(this->securityByID(_securityID, this->exdest, cachedDef))
		{
			(*this->securityCacheBySecurityID)[_securityID] = cachedDef;
		}
		else
		{
			delete cachedDef;
			return false;
		}
	}
	_bucket = (*this->securityCacheBySecurityID)[_securityID];
	return true;
}

bool TAPOrderEntryImpl::getSecurityDefinitionByShortname(std::string _shortname, SecurityDefinition* &_bucket)
{
	if(!CollectionsHelper::containsKey(this->securityCacheByShortname, _shortname))
	{
		SecurityDefinition* cachedDef = new SecurityDefinition();
		if(this->securityByShortname(_shortname, cachedDef))
		{
			(*this->securityCacheByShortname)[_shortname] = cachedDef;
		}
		else
		{
			delete cachedDef;
			return false;
		}
	}
	_bucket = (*this->securityCacheByShortname)[_shortname];
	return true;
}