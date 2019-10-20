#include "TAPOrderEntryImpl.h"

#define NUM_FIELDS_ORDER_ACK				2
#define NUM_FIELDS_ORDER_REJ				4
#define NUM_FIELDS_ORDER_FILL				7
#define NUM_FIELDS_STATISTICS_POSITION 		5
#define NUM_FIELDS_STATISTICS_ACCOUNT	 	5
#define NUM_FIELDS_STATISTICS_CONNECTION 	2
#define MSG_BUFFER_SIZE 					512

using namespace std;
TAPOrderEntryImpl::TAPOrderEntryImpl(JNIThreadManager* _manager, CallbackFunc _callbackFunc, SecurityIDLookupFunc _securityFuncByID, ShortnameLookupFunc _securityFuncByShortname, LoggerFunc _loggerFunc)
{
	this->jniThreadManager = _manager;

	this->traderApi = 0;

	this->securityCacheByShortname = new boost::unordered_map<Shortname, SecurityDefinition*>(8);
	this->securityCacheBySecurityID = new boost::unordered_map<SecurityID, SecurityDefinition*>(8);
	this->contractCacheBySecurityID = new boost::unordered_map<SecurityID, TapContractDefinition*>(8);

	this->arbitrageProducts = new boost::unordered_set<Shortname>();

	this->orderIDToRefIDQueue = new boost::lockfree::spsc_queue<TAPOrderIDMapping>(1024);
	this->orderIDToRefIDMap = new boost::unordered_map<std::string, unsigned int>();

	this->refIDToOrderIDQueue = new boost::lockfree::spsc_queue<TAPOrderIDMapping>(1024);
	this->refIDToOrderIDMap = new boost::unordered_map<unsigned int, std::string>();

	this->refIDToOrderStatusQueue = new boost::lockfree::spsc_queue<TAPOrderStatusMapping>(1024);
	this->refIDToOrderStatusMap = new boost::unordered_map<unsigned int, TAPOrderStatus>();

	this->replaceQueue = new boost::lockfree::spsc_queue<TAPReplaceCompletion*>(1024);
	this->replaceFunctionMap = new boost::unordered_map<unsigned int, std::function<void()>>();

	this->encOrderBinding = new TAPOrderMessageEncoderBinding(this);
	this->decOrderBinding = new TAPOrderMessageDecoderBinding(this, this->arbitrageProducts);
	this->encStatBinding = new StatisticsMessageEncoderBinding();
	this->decStatBinding = new StatisticsMessageDecoderBinding();

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

	this->loadSecurityCache("conf/TAP/ctp-security-cache.txt");
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

	std::ostringstream os;
	os << "[TAP-MD] TradeAPI Info: " << GetTapTradeAPIVersion();
	this->logger(os.str());
	if (!rslt)
	{
		return false;
	}
	
	// this->traderApi = CThostFtdcTraderApi::CreateFtdcTraderApi();
	// this->traderApi->RegisterSpi(this);
	// char front_addr[100];
	// strcpy(front_addr, this->address.c_str());
	// this->traderApi->RegisterFront(front_addr);
	// this->traderApi->SubscribePrivateTopic(THOST_TERT_QUICK);
	// this->traderApi->SubscribePublicTopic(THOST_TERT_QUICK);
	// this->traderApi->Init();

	//TODO replace this with a latch
	boost::this_thread::sleep(boost::posix_time::milliseconds(5000));

	//TODO: 查询单个合约的持仓，似乎没有对应的API
	for(auto iter = this->securityCacheBySecurityID->begin(); iter != this->securityCacheBySecurityID->end(); ++iter)
	{
		std::string securityID = iter->first;

		TAPIUINT32 m_uiSessionID = this->getNextRequestID();
		std::ostringstream os;
		os << "[TAP-TRADE] Retrieving position [securityID=" << securityID << "][reqID=" << reqID << "]";
		this->logger(os.str());

		TapAPIPositionQryReq stQryPosReq;
		memset(&stQryPosReq, 0, sizeof(stQryPosReq));
		iErr = this->traderApi->QryPosition(&m_uiSessionID,&stQryPosReq);
		if(TAPIERROR_SUCCEED != iErr) {
			cout << "QryPosition Error:" << iErr <<endl;
			return;
		}
		// CThostFtdcQryInvestorPositionField position;
		// strcpy(position.BrokerID, this->brokerID.c_str());
		// strcpy(position.InvestorID, this->userID.c_str());
		// strcpy(position.InstrumentID, securityID.c_str());
		// this->traderApi->ReqQryInvestorPosition(&position, reqID);

		boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
	}
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
		os << "[TAP-MD] Error logging: " << iErr;
		this->logger(os.str());
		return false;
	}

	// //Waiting for APIReady
	this->m_Event.WaitEvent();
	if (!this->m_bIsAPIReady){
		this->logger("[TAP-MD] TradeAPI not ready, abort!");
		return false;
	}

	this->logger("[TAP-MD] TradeAPI is ready.");
	return true;
}

//无需进行投资者结算
// void TAPOrderEntryImpl::confirmLogin(CThostFtdcRspUserLoginField* _response)
// {
// 	this->frontID = _response->FrontID;
// 	this->sessionID = _response->SessionID;

// 	CThostFtdcSettlementInfoConfirmField confirmField;

// 	strcpy(confirmField.BrokerID, this->brokerID.c_str());
// 	strcpy(confirmField.InvestorID, this->userID.c_str());

// 	this->traderApi->ReqSettlementInfoConfirm(&confirmField, this->getNextRequestID());
// }

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

void TAPOrderEntryImpl::resetPosition(std::string _securityID, const Side* _side, const PositionType* _type, int _size)
{
	StatisticsMessage update;
	update.statisticsAction = StatisticsAction::POSITION_RESET;
	update.shortname = (*this->securityCacheBySecurityID)[_securityID]->shortname;
	update.positionSide = _side;
	update.positionType = _type;
	update.positionSize = _size;
	this->updatePosition(&update);
}

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
	std::ifstream securityCacheFile;
	securityCacheFile.open(_cacheFilePath);

	if(!securityCacheFile.good())
	{
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
			//try to spliit line like: "CFFEX:F:IF:1910:A"
			std::vector<std::string> strs;
			boost::split(strs, line, boost::is_any_of(":"));
			TapContractDefinition* contractDef = new TapContractDefinition();
			strcpy(contractDef->ExchangeNo, strs[0].c_str());
			contractDef->CommodityType = strs[1][0];
			strcpy(contractDef->CommodityNo, strs[2].c_str());
			strcpy(contractDef->ContractNo, strs[3].c_str());

			securityID = strs[2]+strs[3];
			(*this->contractCacheBySecurityID)[securityID]=contractDef;
			if(strs.size>4 && strs[4].compare("A")==0){
					isArb = true;
			}

			SecurityDefinition* definition = new SecurityDefinition();
			if(this->securityByID(securityID, this->exdest, definition))
			{
				std::ostringstream os;
				os << "[TAP-TRADE] Caching security for [id=" << securityID << "][shortname=" << definition->shortname << "]";
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
				unsigned int refID = this->getNextRefID();

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
				unsigned int cancelRefID = this->orderIDToRefIDMap->at(req.orderID);

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
					unsigned int refID = this->getNextRefID();

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
				unsigned int refID = this->orderIDToRefIDMap->at(req.orderID);

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
				os << "[TAP-TRADE] Unable to handle request with action ID: " << req.orderAction->value;
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

void TAPOrderEntryImpl::enrichNew(TapAPINewOrder* _new, unsigned int _refID)
{
	strcpy(_new->ExchangeNo, this->brokerID.c_str());
	strcpy(_new->AccountNo, this->userID.c_str());
	sprintf(_new->RefString, "%d", _refID);

	_new->OrderMinQty = 1;
	//QA: no auto suspend
	// _new->IsAutoSuspend = 0;
	//QA: UserForceClose
	// _new->UserForceClose = 0;
	//QA: ForceCloseReason
	// _new->ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
	//QA: no ContingentCondition
	// _new->ContingentCondition = THOST_FTDC_CC_Immediately;
}

void TAPOrderEntryImpl::enrichCancel(TapAPIOrderCancelReq* _cancel, unsigned int _refID)
{
	sprintf(_cancel->RefString, "%d", _refID);
}

int TAPOrderEntryImpl::getNextRequestID()
{
	return this->reqID++;
}

unsigned int TAPOrderEntryImpl::getNextRefID()
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

		m_bIsAPIReady = true;
	} else {
		this->logger("[TAP-TRADE] Login failed!");
		this->m_Event.SignalEvent();	
	}
}

void TAP_CDECL TAPOrderEntryImpl::OnAPIReady()
{	
	// 无需进行投资者结算
	// this->confirmLogin(_rspUserLogin);

	//requesting positions too soon after startup can result in an error
	boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
	this->retrievePositions();
	this->retrieveAccountDetails();

	this->m_Event.SignalEvent();	
	this->notifyConnectionStatus(true);
}

// void TAPOrderEntryImpl::OnFrontConnected()
// {
// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Connected to TAP Trader API";
// 	this->logger(os.str());

// 	NativeThreadID id = boost::this_thread::get_id();
// 	this->jniThreadManager->registerJNIThreadName(id, "ctp-trader-callback");
// 	this->jniThreadManager->getNativeThreadEnv(id);
// 	this->mainCallbackThreadID = id;

// 	this->login();
// }

void TAP_CDECL TAPOrderEntryImpl::OnDisconnect( TAPIINT32 reasonCode )
{
	std::ostringstream os;
	os << "[TAP-TRADE] Disconnected from TAP MD API: " << reasonCode;
	this->logger(os.str());

	std::string threadName = "ctp-trade-terminate";
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
// void TAPOrderEntryImpl::OnFrontDisconnected(int _reason)
// {
// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Disconnected from TAP Trader API: " << _reason;
// 	this->logger(os.str());

// 	std::string threadName = "ctp-trader-terminate";
// 	NativeThreadID id = boost::this_thread::get_id();
// 	this->jniThreadManager->registerJNIThreadName(id, threadName);
// 	this->jniThreadManager->getNativeThreadEnv(id);

// 	this->notifyConnectionStatus(false);

// 	if(!this->jniThreadManager->releaseNativeThreadEnv(id))
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Error releasing native thread [name=" << threadName << "]";
// 		this->logger(os.str());
// 	}

// 	if(!this->jniThreadManager->releaseNativeThreadEnv(this->mainCallbackThreadID))
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Error releasing native thread [name=" << this->jniThreadManager->getJNIThreadName(this->mainCallbackThreadID) << "]";
// 		this->logger(os.str());
// 	}

// 	/*
// 	int remaining = this->jniThreadManager->releaseAll();

// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Released additional threads [count=" << remaining << "]";
// 	this->logger(os.str());
// 	*/
// }

// void TAPOrderEntryImpl::OnHeartBeatWarning(int _timeLapse)
// {
// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Heartbeat: " << _timeLapse;
// 	this->logger(os.str());
// }

// void TAPOrderEntryImpl::OnRspUserLogin(CThostFtdcRspUserLoginField* _rspUserLogin, CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
// {
// 	//only initialize refID from cold start
// 	if(this->refID == 0)
// 	{
// 		this->refID = atoi(_rspUserLogin->MaxOrderRef);
// 	}

// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Logged in to TAP Trader API [user=" << _rspUserLogin->UserID << "][maxOrderRef=" << _rspUserLogin->MaxOrderRef << "][reqID=" << _requestID << "] login response: " << getErrorString(_rspInfo);
// 	this->logger(os.str());

// 	this->confirmLogin(_rspUserLogin);

// 	//requesting positions too soon after startup can result in an error
// 	boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
// 	this->retrievePositions();
// 	this->retrieveAccountDetails();
// 	this->notifyConnectionStatus(true);
// }

// void TAPOrderEntryImpl::OnRspUserLogout(CThostFtdcUserLogoutField* _rspUserLogout, CThostFtdcRspInfoField* _rspInfo, int _nRequestID, bool _isLast)
// {
// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Logged out of TAP Trader API [user=" << _rspUserLogout->UserID << "] logout response: " << getErrorString(_rspInfo);
// 	this->logger(os.str());
// }

// void TAPOrderEntryImpl::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* _settlementInfoConfirm, CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
// {
// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Received settlement confirmation [brokerID=" << _settlementInfoConfirm->BrokerID << "][investorID=" << _settlementInfoConfirm->InvestorID << "] settlement response: " << getErrorString(_rspInfo);
// 	this->logger(os.str());
// }

void TAP_CDECL TAPOrderEntryImpl::OnRspQryPosition( TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIPositionInfo *info )
{
	std::ostringstream os;
	if(NULL == info)
	{
		os << "[TAP-TRADE] Received flat position [reqID=" << sessionID << "]";
		this->logger(os.str());
	}
	//History
	//TODO: 是否直接计算总持仓？
	else if(info->IsHistory == APIYNFLAG_YES)
	{	
		string instrumentID=getInstrumentIDFromCommodityNoAndContractNo(info->CommodityNo,info->ContractNo);
		os << "[TAP-TRADE] Received historical position [securityID=" << instrumentID << "][date=" << info->MatchDate << "]";
		switch(info->MatchSide)
		{
			case TAPI_SIDE_BUY:
			{
				os << "[side=buy][PositionNo=" << info->PositionNo << "][overnight=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
				this->resetPosition(instrumentID, Side::BUY, PositionType::OPEN_OVERNIGHT, info->PositionQty);
				break;
			}
			case TAPI_SIDE_SELL:
			{
				os << "[side=sell][PositionNo=" << info->PositionNo << "][today=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
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
	//Today
	//TODO: 是否直接计算总持仓？
	else if(info->IsHistory == APIYNFLAG_NO)
	{
		string instrumentID=getInstrumentIDFromCommodityNoAndContractNo(info->CommodityNo,info->ContractNo);
		os << "[TAP-TRADE] Received current day position [securityID=" << instrumentID << "][date=" << info->MatchDate << "]";
		switch(info->MatchSide)
		{
			case TAPI_SIDE_BUY:
			{
				os << "[side=buy][PositionNo=" << info->PositionNo << "][today=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
				this->resetPosition(instrumentID, Side::BUY, PositionType::OPEN_TODAY, info->PositionQty);
				break;
			}
			case TAPI_SIDE_SELL:
			{
				os << "[side=sell][PositionNo=" << info->PositionNo << "][today=" << info->PositionQty << "][price=" << info->PositionPrice << "]";
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
}

// void TAPOrderEntryImpl::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* _position, CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
// {
// 	std::ostringstream os;
// 	if(_position == 0)
// 	{
// 		os << "[TAP-TRADE] Received flat position [reqID=" << _requestID << "]";
// 		this->logger(os.str());
// 	}
// 	else if(_position->PositionDate == THOST_FTDC_PSD_History)
// 	{
// 		os << "[TAP-TRADE] Received historical position [securityID=" << _position->InstrumentID << "][date=" << _position->TradingDay << "]";
// 		switch(_position->PosiDirection)
// 		{
// 			case THOST_FTDC_PD_Long:
// 			{
// 				os << "[side=buy][today=" << _position->TodayPosition << "][overnight=" << _position->YdPosition << "][current=" << _position->Position << "][closed=" << _position->CloseVolume << "]";
// 				this->resetPosition(std::string(_position->InstrumentID), Side::BUY, PositionType::OPEN_OVERNIGHT, _position->Position);
// 				break;
// 			}
// 			case THOST_FTDC_PD_Short:
// 			{
// 				os << "[side=sell][today=" << _position->TodayPosition << "][overnight=" << _position->YdPosition << "][current=" << _position->Position << "][closed=" << _position->CloseVolume << "]";
// 				this->resetPosition(std::string(_position->InstrumentID), Side::SELL, PositionType::OPEN_OVERNIGHT, _position->Position);
// 				break;
// 			}
// 			default:
// 			{
// 				break;
// 			}
// 		}
// 		this->logger(os.str());
// 	}
// 	else if(_position->PositionDate == THOST_FTDC_PSD_Today)
// 	{
// 		os << "[TAP-TRADE] Received current day position [securityID=" << _position->InstrumentID << "][date=" << _position->TradingDay << "]";
// 		switch(_position->PosiDirection)
// 		{
// 			case THOST_FTDC_PD_Long:
// 			{
// 				os << "[side=buy][today=" << _position->TodayPosition << "][overnight=" << _position->YdPosition << "][current=" << _position->Position << "][closed=" << _position->CloseVolume << "]";
// 				this->resetPosition(std::string(_position->InstrumentID), Side::BUY, PositionType::OPEN_TODAY, _position->Position);
// 				break;
// 			}
// 			case THOST_FTDC_PD_Short:
// 			{
// 				os << "[side=sell][today=" << _position->TodayPosition << "][overnight=" << _position->YdPosition << "][current=" << _position->Position << "][closed=" << _position->CloseVolume << "]";
// 				this->resetPosition(std::string(_position->InstrumentID), Side::SELL, PositionType::OPEN_TODAY, _position->Position);
// 				break;
// 			}
// 			case THOST_FTDC_PD_Net:
// 			default:
// 			{
// 			}
// 		}
// 		this->logger(os.str());
// 	}
// }

// void TAPOrderEntryImpl::OnRspQryTradingAccount(CThostFtdcTradingAccountField* _account, CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
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

	if (info->ErrorCode != 0) {
		cout << "服务器返回了一个关于委托信息的错误：" << info->ErrorCode << endl;
		return;
	} 

	TapAPIOrderInfo* _order = info->OrderInfo;
	unsigned int refID = atoi(_order->OrderNo);

	TAPIOrderStateType orderStatus = _order->OrderState;

	std::ostringstream os;
	os << "[TAP-TRADE] Received order status update [refID=" << refID << "][orderStatus=" << orderStatus << "]";
	this->logger(os.str());

	this->updateRefIDToOrderIDMapping();
	this->updateRefIDToOrderStatusMapping();
	this->updateRefIDToReplaceFunctionMapping();

	//TODO: when to invoke ackNew/ackCancel/ackReject?
	switch(orderStatus)
	{
		case TAPI_ORDER_STATE_SUBMIT: 
		case TAPI_ORDER_STATE_ACCEPT: 
		case TAPI_ORDER_STATE_TRIGGERING: 
		case TAPI_ORDER_STATE_EXCTRIGGERING:
		case TAPI_ORDER_STATE_QUEUED:
		case TAPI_ORDER_STATE_PARTFINISHED:
		case TAPI_ORDER_STATE_FINISHED:
		{
			this->ackNew(refID);
			break;
		}
		case TAPI_ORDER_STATE_CANCELING:
		case TAPI_ORDER_STATE_MODIFYING:
		case TAPI_ORDER_STATE_CANCELED:
		{
			this->ackCancel(refID);
			break;
		}
		case TAPI_ORDER_STATE_LEFTDELETED:
		case TAPI_ORDER_STATE_FAIL:
		case TAPI_ORDER_STATE_DELETED:
		{
			this->reject(refID, RejectType::CANCEL,"unknown");
			break;
		}
	}
}
// void TAPOrderEntryImpl::OnRtnOrder(CThostFtdcOrderField* _order)
// {
// 	unsigned int refID = atoi(_order->OrderRef);

// 	TThostFtdcOrderStatusType orderStatus = _order->OrderStatus;
// 	TThostFtdcOrderSubmitStatusType submitStatus = _order->OrderSubmitStatus;

// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Received order status update [refID=" << refID << "][orderStatus=" << orderStatus << "][submitStatus=" << submitStatus << "]";
// 	this->logger(os.str());

// 	this->updateRefIDToOrderIDMapping();
// 	this->updateRefIDToOrderStatusMapping();
// 	this->updateRefIDToReplaceFunctionMapping();

// 	switch(submitStatus)
// 	{
// 		case THOST_FTDC_OSS_InsertSubmitted:
// 		{
// 			unsigned int refID = atoi(_order->OrderRef);
// 			switch(orderStatus)
// 			{
// 				case THOST_FTDC_OST_AllTraded: //Order submitted, entire quantity crosses book and is filled
// 				case THOST_FTDC_OST_PartTradedQueueing: //Initial part fill, remaining order quantity is on the book
// 				case THOST_FTDC_OST_NoTradeQueueing: //No initial trade, entire order quantity is on the book
// 				{
// 					this->ackNew(refID);
// 					break;
// 				}
// 				case THOST_FTDC_OST_Canceled:
// 				{
// 					this->ackCancel(refID);
// 					break;
// 				}
// 			}
// 			break;
// 		}
// 		case THOST_FTDC_OSS_Accepted:
// 		{
// 			unsigned int refID = atoi(_order->OrderRef);
// 			switch(orderStatus)
// 			{
// 				case THOST_FTDC_OST_PartTradedQueueing: //Initial part fill, remaining order quantity is on the book
// 				case THOST_FTDC_OST_NoTradeQueueing: //No initial trade, entire order quantity is on the book
// 				{
// 					this->ackNew(refID);
// 					break;
// 				}
// 				case THOST_FTDC_OST_Canceled:
// 				{
// 					this->ackCancel(refID);
// 					break;
// 				}
// 			}
// 			break;
// 		}
// 		case THOST_FTDC_OSS_InsertRejected:
// 		{
// 			unsigned int refID = atoi(_order->OrderRef);

// 			if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
// 			{
// 				std::ostringstream os;
// 				os << "[TAP-TRADE] Unable to retrieve order status [OnRtnOrder()][refID=" << refID << "]";
// 				this->logger(os.str());
// 				return;
// 			}

// 			TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];
// 			if(status == TAPOrderStatus::PENDING_REPLACE_PHASE_2)
// 			{
// 				this->ackCancel(refID);
// 			}
// 			else
// 			{
// 				(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
// 				this->reject(refID, RejectType::NEW, "unknown");
// 			}
// 			break;
// 		}
// 		case THOST_FTDC_OSS_CancelRejected:
// 		{
// 			unsigned int refID = atoi(_order->OrderRef);
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
// 			this->reject(refID, RejectType::CANCEL, "unknown");
// 			break;
// 		}
// 		case THOST_FTDC_OSS_CancelSubmitted:
// 		case THOST_FTDC_OSS_ModifySubmitted:
// 		case THOST_FTDC_OSS_ModifyRejected:
// 		{
// 			break;
// 		}
// 	}
// }

void TAP_CDECL TAPOrderEntryImpl::OnRtnFill( const TapAPIFillInfo *info )
// void TAPOrderEntryImpl::OnRtnTrade(CThostFtdcTradeField* _trade)
{
	unsigned int refID = atoi(info->OrderNo);

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

// void TAPOrderEntryImpl::OnRspError(CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
// {
// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Error response: [reqID=" << _requestID << "]" << getErrorString(_rspInfo);
// 	this->logger(os.str());

// 	std::cout << os.str() << std::endl;

// 	this->updateRefIDToOrderIDMapping();
// 	this->updateRefIDToOrderStatusMapping();
// }

//TODO: 
void TAPOrderEntryImpl::OnOrderInsertError( const TapAPIOrderInfoNotice *info ){
	unsigned int refID = atoi(info->OrderInfo->RefString);
	std::string rejReason = getErrorString(info->ErrorCode);

	std::ostringstream os;
	os << "Error response (order insert @ broker): [OnOrderInsertError()][refID=" << refID << "][rejRsn=" << rejReason << "]";
	this->logger(os.str());

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
			this->reject(refID, RejectType::NEW, rejReason);
			break;
		}
		case TAPOrderStatus::PENDING_REPLACE_PHASE_2:
		{
			this->ackCancel(refID);
			break;
		}
		default:
		{
		}
	}
}

//TODO: 
void TAPOrderEntryImpl::OnOrderCancelError( const TapAPIOrderInfoNotice *info ){
	std::string rejReason = getErrorString(info->ErrorCode);
	// if(_orderAction == 0)
	// {
	// 	std::ostringstream os;
	// 	os << "[TAP-TRADE] Missing referenced order action: [OnRspOrderAction()][rejRsn=" << rejReason << "]";
	// 	this->logger(os.str());
	// 	return;
	// }
	unsigned int refID = atoi(info->OrderInfo->RefString);

	std::ostringstream os;
	os << "[TAP-TRADE] Error response (order cancel @ broker): [OnOrderCancelError()][refID=" << refID << "][rejRsn=" << rejReason << "]";
	this->logger(os.str());

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
			this->reject(refID, RejectType::REPLACE, rejReason);
			break;
		}
		case TAPOrderStatus::PENDING_CANCEL:
		{
			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
			this->reject(refID, RejectType::CANCEL, rejReason);
			break;
		}
		default:
		{
		}
	}
}

// void TAPOrderEntryImpl::OnRspOrderInsert(CThostFtdcInputOrderField* _inputOrder, CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
// {
// 	unsigned int refID = atoi(_inputOrder->OrderRef);
// 	std::string rejReason = getErrorString(_rspInfo);

// 	std::ostringstream os;
// 	os << "Error response (order insert @ broker): [OnRspOrderInsert()][refID=" << refID << "][rejRsn=" << rejReason << "]";
// 	this->logger(os.str());

// 	this->updateRefIDToOrderIDMapping();
// 	this->updateRefIDToOrderStatusMapping();

// 	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Unable to retrieve order status [OnRspOrderInsert()][refID=" << refID << "]";
// 		this->logger(os.str());
// 		return;
// 	}

// 	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];

// 	switch(status)
// 	{
// 		case TAPOrderStatus::PENDING_NEW:
// 		{
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
// 			this->reject(refID, RejectType::NEW, rejReason);
// 			break;
// 		}
// 		case TAPOrderStatus::PENDING_REPLACE_PHASE_2:
// 		{
// 			this->ackCancel(refID);
// 			break;
// 		}
// 		default:
// 		{
// 		}
// 	}
// }

// void TAPOrderEntryImpl::OnRspOrderAction(CThostFtdcInputOrderActionField* _orderAction, CThostFtdcRspInfoField* _rspInfo, int _requestID, bool _isLast)
// {
// 	std::string rejReason = getErrorString(_rspInfo);
// 	if(_orderAction == 0)
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Missing referenced order action: [OnRspOrderAction()][rejRsn=" << rejReason << "]";
// 		this->logger(os.str());
// 		return;
// 	}
// 	unsigned int refID = atoi(_orderAction->OrderRef);

// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Error response (order cancel @ broker): [OnRspOrderAction()][refID=" << refID << "][rejRsn=" << rejReason << "]";
// 	this->logger(os.str());

// 	this->updateRefIDToOrderIDMapping();
// 	this->updateRefIDToOrderStatusMapping();

// 	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Unable to retrieve order status [OnRspOrderAction()][refID=" << refID << "]";
// 		this->logger(os.str());
// 		return;
// 	}

// 	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];

// 	switch(status)
// 	{
// 		case TAPOrderStatus::PENDING_REPLACE_PHASE_1:
// 		{
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
// 			this->reject(refID, RejectType::REPLACE, rejReason);
// 			break;
// 		}
// 		case TAPOrderStatus::PENDING_CANCEL:
// 		{
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
// 			this->reject(refID, RejectType::CANCEL, rejReason);
// 			break;
// 		}
// 		default:
// 		{
// 		}
// 	}
// }

// void TAPOrderEntryImpl::OnErrRtnOrderInsert(CThostFtdcInputOrderField* _inputOrder, CThostFtdcRspInfoField* _rspInfo)
// {
// 	unsigned int refID = atoi(_inputOrder->OrderRef);
// 	std::string rejReason = getErrorString(_rspInfo);

// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Error response (order insert @ exchange) [OnErrRtnOrderInsert()][refID=" << refID << "][rejRsn=" << rejReason << "]";
// 	this->logger(os.str());

// 	this->updateRefIDToOrderIDMapping();
// 	this->updateRefIDToOrderStatusMapping();

// 	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Unable to retrieve order status [OnErrRtnOrderInsert()][refID=" << refID << "]";
// 		this->logger(os.str());
// 		return;
// 	}

// 	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];

// 	switch(status)
// 	{
// 		case TAPOrderStatus::PENDING_NEW:
// 		{
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::REJECTED;
// 			this->reject(refID, RejectType::NEW, rejReason);
// 			break;
// 		}
// 		case TAPOrderStatus::PENDING_REPLACE_PHASE_2:
// 		{
// 			this->ackCancel(refID);
// 			break;
// 		}
// 		default:
// 		{
// 		}
// 	}
// }

// void TAPOrderEntryImpl::OnErrRtnOrderAction(CThostFtdcOrderActionField* _orderAction, CThostFtdcRspInfoField* _rspInfo)
// {
// 	unsigned int refID = atoi(_orderAction->OrderRef);
// 	std::string rejReason = getErrorString(_rspInfo);

// 	std::ostringstream os;
// 	os << "[TAP-TRADE] Error response (order cancel @ exchange) [OnErrRtnOrderAction()][refID=" << refID << "][rejRsn=" << rejReason << "]";
// 	this->logger(os.str());

// 	this->updateRefIDToOrderIDMapping();
// 	this->updateRefIDToOrderStatusMapping();

// 	if(!CollectionsHelper::containsKey(this->refIDToOrderStatusMap, refID))
// 	{
// 		std::ostringstream os;
// 		os << "[TAP-TRADE] Unable to retrieve order status [OnErrRtnOrderAction()][refID=" << refID << "]";
// 		this->logger(os.str());
// 		return;
// 	}

// 	TAPOrderStatus status = (*this->refIDToOrderStatusMap)[refID];

// 	switch(status)
// 	{
// 		case TAPOrderStatus::PENDING_REPLACE_PHASE_1:
// 		{
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
// 			this->reject(refID, RejectType::REPLACE, rejReason);
// 			break;
// 		}
// 		case TAPOrderStatus::PENDING_CANCEL:
// 		{
// 			(*this->refIDToOrderStatusMap)[refID] = TAPOrderStatus::WORKING;
// 			this->reject(refID, RejectType::CANCEL, rejReason);
// 			break;
// 		}
// 		default:
// 		{
// 		}
// 	}
// }

void TAPOrderEntryImpl::ackNew(unsigned int _refID)
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

	switch(status)
	{
		case TAPOrderStatus::PENDING_NEW:
		{
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
}

void TAPOrderEntryImpl::ackCancel(unsigned int _refID)
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

void TAPOrderEntryImpl::reject(unsigned int _refID, const RejectType* _rejType, std::string _rejectReason)
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

bool TAPOrderEntryImpl::getContractDefinitionBySecurityID(std::string _securityID,TapContractDefinition* &_bucket)
{
	if(!CollectionsHelper::containsKey(this->contractCacheBySecurityID, _securityID))
	{
		return false;
	}
	_bucket = (*this->contractCacheBySecurityID)[_securityID];
	return true;
}