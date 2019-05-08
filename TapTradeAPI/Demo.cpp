#include "TapTradeAPI.h"
#include "TapAPIError.h"
#include "Trade.h"
#include "TradeConfig.h"

#include <iostream>
#include <string.h>
using namespace std;

int main(int argc, char* argv[])
{
	//取得API的版本信息
	cout << GetTapTradeAPIVersion() << endl;

	//创建API实例
	TAPIINT32 iResult = TAPIERROR_SUCCEED;
	TapAPIApplicationInfo stAppInfo;
	strcpy(stAppInfo.AuthCode, DEFAULT_AUTHCODE);
	strcpy(stAppInfo.KeyOperationLogPath, "log");
	ITapTradeAPI *pAPI = CreateTapTradeAPI(&stAppInfo, iResult);
	if (NULL == pAPI){
		cout << "创建API实例失败，错误码：" << iResult <<endl;
		return -1;
	}

	//设定ITapTradeAPINotify的实现类，用于异步消息的接收
	Trade objTrade;
	pAPI->SetAPINotify(&objTrade);
	

	//启动测试，下单
	objTrade.SetAPI(pAPI);
	objTrade.RunTest();


	return 0;
}