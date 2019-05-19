#include "TAPMarketDataImpl.h"
#include "TAPMarketDataConfig.h"

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

void TAPMarketDataImpl::configure(std::string _address, std::string _brokerID, std::string _userID, std::string _password, std::string _exdest)
{
	this->address = _address;
	this->brokerID = _brokerID;
	this->userID = _userID;
	this->password = _password;
	this->exdest = _exdest;

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

	this->logger("[TAP-MD] Connecting to CTP MD API...");

    TAPIINT32 iResult = TAPIERROR_SUCCEED;
    TapAPIApplicationInfo stAppInfo;
    strcpy(stAppInfo.AuthCode, DEFAULT_AUTHCODE);
	strcpy(stAppInfo.KeyOperationLogPath, "log");
	this->mdApi = CreateTapQuoteAPI(&stAppInfo, iResult);
	this->mdApi->SetAPINotify(this);

	TAPIINT32 iErr = TAPIERROR_SUCCEED;
	bool rslt = true;
	iErr = this->mdApi->SetHostAddress(DEFAULT_IP, DEFAULT_PORT);
	if(TAPIERROR_SUCCEED != iErr) {
		cout << "SetHostAddress Error:" << iErr <<endl;
		return false;
	}else{
        rslt = this->login();
    }


	std::ostringstream os;
	// os << "date:" << this->mdApi->GetTradingDay() << "\n";
	// os << "version:" << this->mdApi->GetApiVersion() << "\n";
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
	this->logger("[TAP-MD] Disconnecting from CTP MD API...");
	this->mdApi->Disconnect();
	this->mdApi = 0;

	boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
}

bool TAPMarketDataImpl::login()
{
	this->logger("[TAP-MD] Logging in to CTP MD API...");

	TapAPIQuoteLoginAuth stLoginAuth;
	memset(&stLoginAuth, 0, sizeof(stLoginAuth));
	APIStrncpy(stLoginAuth.UserNo, DEFAULT_USERNAME);
	APIStrncpy(stLoginAuth.Password, DEFAULT_PASSWORD);
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
		return false;
	}

	return true;
}

void TAPMarketDataImpl::subscribe(std::string _securityID)
{
	boost::mutex::scoped_lock lock(this->subscriptionMutex);

	std::ostringstream os;
	os << "[TAP-MD] Subscribing to security[" << _securityID << "]";
	this->logger(os.str());

	TapAPIContract stContract;
	memset(&stContract, 0, sizeof(stContract));
	// APIStrncpy(stContract.Commodity.ExchangeNo, DEFAULT_EXCHANGE_NO);
	strcpy(stContract.Commodity.ExchangeNo, DEFAULT_EXCHANGE_NO);
	stContract.Commodity.CommodityType = DEFAULT_COMMODITY_TYPE;
	// APIStrncpy(stContract.Commodity.CommodityNo, DEFAULT_COMMODITY_NO);
	// APIStrncpy(stContract.ContractNo1, DEFAULT_CONTRACT_NO);
	strcpy(stContract.Commodity.CommodityNo, DEFAULT_COMMODITY_NO);
	strcpy(stContract.ContractNo1, DEFAULT_CONTRACT_NO);
	stContract.CallOrPutFlag1 = TAPI_CALLPUT_FLAG_NONE;
	stContract.CallOrPutFlag2 = TAPI_CALLPUT_FLAG_NONE;
	TAPIUINT32 m_uiSessionID = 0;
    TAPIINT32 iErr = TAPIERROR_SUCCEED;
	iErr = this->mdApi->SubscribeQuote(&m_uiSessionID, &stContract);
	if(TAPIERROR_SUCCEED != iErr) {
		std::ostringstream os;
		os << "[TAP-MD] Error subscribing to security[" << _securityID << "]";
		this->logger(os.str());
	}
    else
	{
		this->securities->insert(_securityID);
	}

	lock.unlock();
}

void TAPMarketDataImpl::unsubscribe(std::string _securityID)
{
	boost::mutex::scoped_lock lock(this->subscriptionMutex);

	std::ostringstream os;
	os << "[TAP-MD] Unsubscribing from security[" << _securityID << "]";
	this->logger(os.str());

	TapAPIContract stContract;
	memset(&stContract, 0, sizeof(stContract));
	APIStrncpy(stContract.Commodity.ExchangeNo, DEFAULT_EXCHANGE_NO);
	stContract.Commodity.CommodityType = DEFAULT_COMMODITY_TYPE;
	APIStrncpy(stContract.Commodity.CommodityNo, DEFAULT_COMMODITY_NO);
	APIStrncpy(stContract.ContractNo1, DEFAULT_CONTRACT_NO);
	stContract.CallOrPutFlag1 = TAPI_CALLPUT_FLAG_NONE;
	stContract.CallOrPutFlag2 = TAPI_CALLPUT_FLAG_NONE;
	TAPIUINT32 m_uiSessionID = 0;
    TAPIINT32 iErr = TAPIERROR_SUCCEED;
	iErr = this->mdApi->UnSubscribeQuote(&m_uiSessionID, &stContract);
	if(TAPIERROR_SUCCEED != iErr) 
	{
		std::ostringstream os;
		os << "[TAP-MD] Error unsubscribing to security[" << _securityID << "]";
		this->logger(os.str());
	}
	else
	{
		this->securities->erase(_securityID);
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
		cout << "登录成功，等待API初始化..." << endl;
		NativeThreadID id = boost::this_thread::get_id();
		this->jniThreadManager->registerJNIThreadName(id, "tap-md-callback");
		this->jniThreadManager->getNativeThreadEnv(id);
		this->mainCallbackThreadID = id;
		m_bIsAPIReady = true;
	} else {
		cout << "登录失败，错误码:" << errorCode << endl;
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
	cout << "API断开,断开原因:"<<reasonCode << endl;
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
	// cout << __FUNCTION__ << " is called." << endl;
	// if (TAPIERROR_SUCCEED == errorCode)
	// {
	// 	cout << "行情订阅成功 ";
	// 	if (NULL != info)
	// 	{
	// 		cout << info->DateTimeStamp << " "
	// 			<< info->Contract.Commodity.ExchangeNo << " "
	// 			<< info->Contract.Commodity.CommodityType << " "
	// 			<< info->Contract.Commodity.CommodityNo << " "
	// 			<< info->Contract.ContractNo1 << " "
	// 			<< info->QLastPrice
	// 			// ...		
	// 			<<endl;
	// 	}

	// } else{
	// 	cout << "行情订阅失败，错误码：" << errorCode <<endl;
	// }

	if (TAPIERROR_SUCCEED == errorCode)
	{
		std::string instrumentID = getInstrumentIDFromTapQuoteMsg(info);
		std::ostringstream os;
		os << "[TAP-MD] Security[" << instrumentID << "] subscription response: " << getErrorString(errorCode);
		this->logger(os.str());

		SecurityDefinition* definition = new SecurityDefinition();
		std::string secID(instrumentID);
		if(this->security(secID, this->exdest, definition))
		{
			bool contains = this->securityCache->find(secID) != this->securityCache->end();
			if(!contains)
			{
				std::ostringstream os;
				os << "[CTP-MD] Caching security[" << instrumentID<< "]";
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
	cout << __FUNCTION__ << " is called." << endl;
	if (NULL != info)
	{
		cout << "行情更新:" 
			<< info->DateTimeStamp << " "
			<< info->Contract.Commodity.ExchangeNo << " "
			<< info->Contract.Commodity.CommodityType << " "
			<< info->Contract.Commodity.CommodityNo << " "
			<< info->Contract.ContractNo1 << " "
			<< info->QLastPrice
			// ...		
			<<endl;
	}
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