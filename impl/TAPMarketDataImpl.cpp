#include "TAPMarketDataImpl.h"

#define NUM_FIELDS                              18
#define MSG_BUFFER_SIZE                         512

#define NUM_FIELDS_STATISTICS_CONNECTION 	2

using namespace std;
TAPMarketDataImpl::TAPMarketDataImpl(JNIThreadManager* _manager, CallbackFunc _callbackFunc, SecurityIDLookupFunc _securityFunc, LoggerFunc _loggerFunc)
{
	this->jniThreadManager = _manager;

	this->mdApi = 0;

	this->securities = new boost::unordered_set<std::string>();
	this->securityCache = new boost::unordered_map<SecurityID, SecurityDefinition*>();

	this->encBinding = new TAPMarketDataMessageEncoderBinding(this->securityCache);
	this->encStatBinding = new StatisticsMessageEncoderBinding();

	const ByteField* fields[NUM_FIELDS] =
	{
		ByteField::SHORTNAME,
		ByteField::TIME,
		ByteField::TIME_PRECISION,
		ByteField::PRICE_EXPONENT,
		ByteField::LAST_PRICE,
		ByteField::HIGH_PRICE,
		ByteField::LOW_PRICE,
		ByteField::UPPER_LIMIT,
		ByteField::LOWER_LIMIT,
		ByteField::VOLUME,
		ByteField::TURNOVER,
		ByteField::OPEN_INTEREST,
		ByteField::OPEN_PRICE,
		ByteField::CLOSE_PRICE,
		ByteField::DEPTH_BID_PRICE_LIST,
		ByteField::DEPTH_BID_SIZE_LIST,
		ByteField::DEPTH_OFFER_PRICE_LIST,
		ByteField::DEPTH_OFFER_SIZE_LIST
	};
	this->fieldPresence = CoreMessageTemplate::buildPresence(this->fpLength, CoreMessageType::MARKET_DATA_MESSAGE->mTemplate, fields, NUM_FIELDS);

	const ByteField* fields_statistics_connection[NUM_FIELDS_STATISTICS_CONNECTION] =
	{
		ByteField::STATISTICS_ACTION,
		ByteField::CONNECTION_STATUS
	};
	this->fieldPresence_Statistics_Connection = CoreMessageTemplate::buildPresence(this->fpLength_Statistics_Connection, CoreMessageType::STATISTICS_MESSAGE->mTemplate, fields_statistics_connection, NUM_FIELDS_STATISTICS_CONNECTION);

	this->callback = _callbackFunc;
	this->security = _securityFunc;
	this->logger = _loggerFunc;
	this->reqID = 1;

	#if ENABLE_RECORDING == 1
	this->async = 0;
	#endif
}

TAPMarketDataImpl::~TAPMarketDataImpl()
{
	this->mdApi->SetAPINotify(NULL);
	this->mdApi = 0;

	delete(this->encBinding);
	delete(this->securities);
	delete(this->securityCache);
}

void TAPMarketDataImpl::configure(std::string _address, std::string _port, std::string _brokerID, std::string _userID, std::string _password, std::string _exdest, std::string _authcode)
{
	this->address = _address;
	this->port = (unsigned short) strtoul(_port.c_str(), NULL, 0);;
	this->brokerID = _brokerID;
	this->userID = _userID;
	this->password = _password;
	this->exdest = _exdest;
	this->authcode=_authcode;

	#if ENABLE_RECORDING == 1
	auto range = [](int _lower, int _upper, int _cmp)
	{
		return _cmp >= _lower && _cmp <= _upper;
	};
	std::ostringstream os;
	os << "logs/record/";
	for(unsigned int i = 0; i < _address.length(); i++)
	{
		char c = _address[i];
		if(range(48, 57, c) || range(65, 90, c) || range(97, 122, c))
		{
			os << c;
		}
		else if(c == '.' || c == ':')
		{
			os << '_';
		}
	}
	os << '_' << AsyncOut::getTimestamp() << ".log";
	this->async = new AsyncOut(os.str());
	#endif
}

bool TAPMarketDataImpl::connect()
{
	if(this->mdApi != 0)
	{
		this->mdApi = 0;
	}

	this->logger("[TAP-MD] Connecting to TAP MD API...");

    TAPIINT32 iResult = TAPIERROR_SUCCEED;
    TapAPIApplicationInfo stAppInfo;
    strcpy(stAppInfo.AuthCode, this->authcode.c_str());
	strcpy(stAppInfo.KeyOperationLogPath, "log");
	this->mdApi = CreateTapQuoteAPI(&stAppInfo, iResult);
	this->mdApi->SetAPINotify(this);

	TAPIINT32 iErr = TAPIERROR_SUCCEED;
	bool rslt = true;
	iErr = this->mdApi->SetHostAddress(this->address.c_str(), this->port);
	if(TAPIERROR_SUCCEED != iErr) {
		cout << "SetHostAddress Error:" << iErr <<endl;
		return false;
	}else{
        rslt = this->login();
    }


	std::ostringstream os;
	os << "[TAP-MD] QuoteAPI Info: " << GetTapQuoteAPIVersion();
	this->logger(os.str());
	if (!rslt)
	{
		return false;
	}
	
	#if ENABLE_RECORDING == 1
	this->async->start();
	#endif

	return true;
}

void TAPMarketDataImpl::disconnect()
{
	this->logger("[TAP-MD] Disconnecting from TAP MD API...");
	this->mdApi->Disconnect();
	this->mdApi = 0;

	boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
}

bool TAPMarketDataImpl::login()
{
	this->logger("[TAP-MD] Logging in to TAP MD API...");

	TapAPIQuoteLoginAuth stLoginAuth;
	memset(&stLoginAuth, 0, sizeof(stLoginAuth));
	strcpy(stLoginAuth.UserNo, this->userID.c_str());
	strcpy(stLoginAuth.Password, this->password.c_str());
	stLoginAuth.ISModifyPassword = APIYNFLAG_NO;
	stLoginAuth.ISDDA = APIYNFLAG_NO;
	TAPIINT32 iErr = TAPIERROR_SUCCEED;
	iErr = this->mdApi->Login(&stLoginAuth);
	if(TAPIERROR_SUCCEED != iErr) {
        ostringstream os;
		os << "[TAP-MD] Error logging: " << iErr;
		this->logger(os.str());
		return false;
	}

	// //Waiting for APIReady
	this->m_Event.WaitEvent();
	if (!this->m_bIsAPIReady){
		this->logger("[TAP-MD] QuoteAPI not ready, abort!");
		return false;
	}

	this->logger("[TAP-MD] QuoteAPI is ready.");
	return true;
}

void TAPMarketDataImpl::subscribe(TapAPIContract* contract)
{
	boost::mutex::scoped_lock lock(this->subscriptionMutex);

	std::ostringstream os1;
	os1 << contract->Commodity.CommodityNo << contract->ContractNo1 ;
	std::string strContract = os1.str();
	
	std::ostringstream os;
	os << "[TAP-MD] Subscribing to security[" << strContract<< "]";
	this->logger(os.str());

	TAPIUINT32 m_uiSessionID = this->getNextRequestID();
    TAPIINT32 iErr = TAPIERROR_SUCCEED;
	iErr = this->mdApi->SubscribeQuote(&m_uiSessionID, contract);
	if(TAPIERROR_SUCCEED != iErr) {
		std::ostringstream os;
		os << "[TAP-MD] Error subscribing to security[" << strContract << "]";
		this->logger(os.str());
	}
    else
	{
		this->securities->insert(strContract);
	}

	lock.unlock();
}

void TAPMarketDataImpl::unsubscribe(TapAPIContract* contract)
{
	boost::mutex::scoped_lock lock(this->subscriptionMutex);

	std::ostringstream os1;
	os1 << contract->Commodity.CommodityNo << contract->ContractNo1 ;
	std::string strContract = os1.str();

	std::ostringstream os;
	os << "[TAP-MD] Unsubscribing from security[" << strContract << "]";
	this->logger(os.str());

	TAPIUINT32 m_uiSessionID = this->getNextRequestID();
    TAPIINT32 iErr = TAPIERROR_SUCCEED;
	iErr = this->mdApi->UnSubscribeQuote(&m_uiSessionID, contract);
	if(TAPIERROR_SUCCEED != iErr) 
	{
		std::ostringstream os;
		os << "[TAP-MD] Error unsubscribing to security[" << strContract << "]";
		this->logger(os.str());
	}
	else
	{
		this->securities->erase(strContract);
	}

	lock.unlock();
}

int TAPMarketDataImpl::getNextRequestID()
{
	return this->reqID++;
}

////-------------------------------------------------------------
void TAP_CDECL TAPMarketDataImpl::OnRspLogin(TAPIINT32 errorCode, const TapAPIQuotLoginRspInfo *info)
{
	if(TAPIERROR_SUCCEED == errorCode) {
		this->logger("[TAP-MD] Login success, waiting for API ready...");

		NativeThreadID id = boost::this_thread::get_id();
		this->jniThreadManager->registerJNIThreadName(id, "tap-md-callback");
		this->jniThreadManager->getNativeThreadEnv(id);
		this->mainCallbackThreadID = id;

		m_bIsAPIReady = true;
	} else {
		this->logger("[TAP-MD] Login failed!");
		this->m_Event.SignalEvent();	
	}
}

void TAP_CDECL TAPMarketDataImpl::OnAPIReady()
{
	cout << "API初始化完成" << endl;
	this->m_Event.SignalEvent();	
	this->notifyConnectionStatus(true);
}

void TAP_CDECL TAPMarketDataImpl::OnDisconnect(TAPIINT32 reasonCode)
{
	std::ostringstream os;
	os << "[TAP-MD] Disconnected from TAP MD API: " << reasonCode;
	this->logger(os.str());

	this->notifyConnectionStatus(false);

	if(!this->jniThreadManager->releaseNativeThreadEnv(this->mainCallbackThreadID))
	{
		std::ostringstream os;
		os << "[TAP-Trader] Error releasing native thread [name=" << this->jniThreadManager->getJNIThreadName(this->mainCallbackThreadID) << "]";
		this->logger(os.str());
	}
}

void TAP_CDECL TAPMarketDataImpl::OnRspChangePassword(TAPIUINT32 sessionID, TAPIINT32 errorCode)
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL TAPMarketDataImpl::OnRspQryExchange(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIExchangeInfo *info)
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL TAPMarketDataImpl::OnRspQryCommodity(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIQuoteCommodityInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
	if (NULL != info)
	{
		cout<< info->Commodity.ExchangeNo << " "
			<< info->Commodity.CommodityType << " "
			<< info->Commodity.CommodityNo << " "
			<< info->CommodityTickSize << " "
			// ...		
			<<endl;
	}
}

void TAP_CDECL TAPMarketDataImpl::OnRspQryContract(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIQuoteContractInfo *info)
{
	// cout << __FUNCTION__ << " is called." << endl;
	if (NULL != info)
	{
		cout << info->Contract.Commodity.ExchangeNo << " "
			<< info->Contract.Commodity.CommodityType << " "
			<< info->Contract.Commodity.CommodityNo << " "
			<< info->Contract.ContractNo1 << " "
			// ...		
			<<endl;
	}
}

void TAP_CDECL TAPMarketDataImpl::OnRtnContract(const TapAPIQuoteContractInfo *info)
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL TAPMarketDataImpl::OnRspSubscribeQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIQuoteWhole *info)
{
	std::string instrumentID = getInstrumentIDFromTapQuoteMsg(info);
	std::ostringstream os;
	os << "[TAP-MD] Security[" << instrumentID << "] subscription response: " << getErrorString(errorCode);
	this->logger(os.str());

	if (TAPIERROR_SUCCEED == errorCode)
	{
		SecurityDefinition* definition = new SecurityDefinition();
		std::string secID(instrumentID);
		if(this->security(secID, this->exdest, definition))
		{
			bool contains = this->securityCache->find(secID) != this->securityCache->end();
			if(!contains)
			{
				std::ostringstream os;
				os << "[TAP-MD] Caching security[" << instrumentID<< "]";
				this->logger(os.str());

				this->securityCache->emplace(secID, definition);
			}
		}
		else
		{
			delete(definition);
		}
	}
}

void TAP_CDECL TAPMarketDataImpl::OnRspUnSubscribeQuote(TAPIUINT32 sessionID, TAPIINT32 errorCode, TAPIYNFLAG isLast, const TapAPIContract *info)
{
	cout << __FUNCTION__ << " is called." << endl;
}

void TAP_CDECL TAPMarketDataImpl::OnRtnQuote(const TapAPIQuoteWhole *info)
{
	TapAPIQuoteWhole *_message = const_cast<TapAPIQuoteWhole *>(info);
	#if ENABLE_RECORDING == 1
	std::ostringstream os;
	os << _depthMarketData->InstrumentID << ',';
	os << _depthMarketData->BidPrice1 << ',' << _depthMarketData->BidVolume1 << ',';
	os << _depthMarketData->AskPrice1 << ',' << _depthMarketData->AskVolume1;
	this->async->out(os.str());
	#endif

	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::MARKET_DATA_MESSAGE, data, this->encBinding, this->fieldPresence, this->fpLength, _message);
	long addr = (long)data;

	this->callback(addr, msgLen);
}

void TAPMarketDataImpl::notifyConnectionStatus(bool _connected)
{
	StatisticsMessage update;
	update.statisticsAction = StatisticsAction::CONNECTION_STATUS_UPDATE;
	update.connected = _connected;

	char data[MSG_BUFFER_SIZE];
	int msgLen = CoreMessageEncoder::encode(CoreMessageType::STATISTICS_MESSAGE, data, this->encStatBinding, this->fieldPresence_Statistics_Connection, this->fpLength_Statistics_Connection, &update);
	long addr = (long)data;

	this->callback(addr, msgLen);
}