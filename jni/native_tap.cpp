/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include "native_tap.h"
#include "jni/JNIThreadManager.h"
#include "TAPMarketDataImpl.h"
#include "TAPOrderEntryImpl.h"
#include "collections/ConcurrentQueue.hpp"

#include <iostream>
#include <boost/unordered_map.hpp>
#include <boost/thread.hpp>
#include <boost/locale.hpp>

#define CHECK_JNI_EXCEPTION(JNIenv)\
	if (JNIenv->ExceptionCheck())\
	{\
		JNIenv->ExceptionClear();\
		return JNI_FALSE;\
	}\
\

static const char* STRING_SIGNATURE = "Ljava/lang/String;";
static const char* LEVEL_SIGNATURE = "Lorg/apache/logging/log4j/Level;";

JNIThreadManager* threadManager;

TAPMarketDataImpl* tap_marketdata;
TAPOrderEntryImpl* tap_orderentry;

jmethodID md_callback_method_id;
jmethodID order_callback_method_id;

ConcurrentQueue<std::string> loggingQueue;

JavaVM *jvm;
jobject md_callbackObj;
jobject order_callbackObj;

bool md_registered = 0;
bool order_registered = 0;
bool order_req_registered = 0;

boost::mutex registration_mutex;

volatile bool logging = 0;

inline
void getFieldAsString(JNIEnv* _env, jobject _obj, const char* _field, std::string &_buffer)
{
	jclass objCls = _env->GetObjectClass(_obj);
	jfieldID fid = _env->GetFieldID(objCls, _field, STRING_SIGNATURE);
	jstring value = (jstring) _env->GetObjectField(_obj, fid);
	const char* str = _env->GetStringUTFChars(value, 0);
	_buffer.append(str);
	_env->ReleaseStringUTFChars(value, str); //due to being called by java thread
}

inline
std::string getFieldAsString(JNIEnv* _env, jobject _obj, const char* _field)
{
	jclass objCls = _env->GetObjectClass(_obj);
	jfieldID fid = _env->GetFieldID(objCls, _field, STRING_SIGNATURE);
	jstring value = (jstring) _env->GetObjectField(_obj, fid);
	const char* str = _env->GetStringUTFChars(value, 0);
	std::string retVal(str);
	_env->ReleaseStringUTFChars(value, str); //due to being called by java thread
	return retVal;
}

inline
int getFieldAsInt(JNIEnv* _env, jobject _obj, const char* _field)
{
	jclass objCls = _env->GetObjectClass(_obj);
	jfieldID fid = _env->GetFieldID(objCls, _field, "I");
	jint value = _env->GetIntField(_obj, fid);
	return value;
}

inline
void init_logger()
{
	if(logging)
	{
		return;
	}
	auto process = []()
	{
		std::string name = "tap-native";

		logging = true;

		JNIEnv* environment;

		JavaVMAttachArgs args;
		args.version = JNI_VERSION_1_8;
		char threadName[name.length()+1];
		strcpy(threadName, name.c_str());
		args.name = threadName;
		args.group = NULL;

		jint rs = jvm->AttachCurrentThread((void**)&environment, &args);
		assert (rs == JNI_OK);

		jclass loggerClass = environment->FindClass("com/unown/core/commons/log/CoreLogger");
		jclass levelClass = environment->FindClass("org/apache/logging/log4j/Level");
		jmethodID logMethodID = environment->GetStaticMethodID(loggerClass, "log", "(Lorg/apache/logging/log4j/Level;Ljava/lang/String;)V");
		jfieldID infoFieldID = environment->GetStaticFieldID(levelClass, "INFO", LEVEL_SIGNATURE);

		jobject level = environment->GetStaticObjectField(levelClass, infoFieldID);

		std::string _line;
		while(logging)
		{
			if(loggingQueue.wait_and_pop(_line))
			{
				std::string encoded = boost::locale::conv::to_utf<char>(_line, "GB2312");

				jstring statement = environment->NewStringUTF(encoded.c_str());
				environment->CallStaticVoidMethod(loggerClass, logMethodID, level, statement);
				environment->DeleteLocalRef(statement); //due to being called by native thread
			}
		}

		jvm->DetachCurrentThread();
	};
	boost::thread logger_thread{process};
}

inline
void destroy_logger()
{
	if(!logging)
	{
		return;
	}
	logging = false;
	loggingQueue.time_out();
}

inline
void md_callback(long _addr, int _length)
{
	JNIEnv* environment = threadManager->getNativeThreadEnv(boost::this_thread::get_id());
	environment->CallVoidMethod(md_callbackObj, md_callback_method_id, _addr, _length);
}

inline
void order_callback(long _addr, int _length)
{
	JNIEnv* environment = threadManager->getNativeThreadEnv(boost::this_thread::get_id());
	environment->CallVoidMethod(order_callbackObj, order_callback_method_id, _addr, _length);
}

inline
void log_main(std::string _line)
{
	loggingQueue.push(_line);
}

inline
bool retrieve_security_by_id_exdest(std::string _secID, std::string _exdest, SecurityDefinition* _bucket)
{
	JNIEnv* environment = threadManager->getEnv(boost::this_thread::get_id());
	jstring securityID = environment->NewStringUTF(_secID.c_str());
	jstring exdestination = environment->NewStringUTF(_exdest.c_str());
	_bucket->securityID = _secID;

	jclass moduleClass = environment->FindClass("com/unown/tap/jni/NativeTAP");
	jmethodID securityMethod = environment->GetStaticMethodID(moduleClass, "getSecurityDefinition", "(Ljava/lang/String;Ljava/lang/String;)Lcom/unown/core/commons/types/instrument/SecurityDefinition;");
	jobject securityDefinition = environment->CallStaticObjectMethod(moduleClass, securityMethod, securityID, exdestination);

	bool success = securityDefinition != NULL;

	if(success)
	{
		_bucket->shortname = getFieldAsString(environment, securityDefinition, "shortName");
		_bucket->pxExponent = getFieldAsInt(environment, securityDefinition, "decimalLength");
	}

	environment->DeleteLocalRef(securityID); //due to being called by native thread
	environment->DeleteLocalRef(exdestination); //due to being called by native thread

	return success;
}

inline
bool retrieve_security_by_shortname(std::string _shortname, SecurityDefinition* _bucket)
{
	JNIEnv* environment = threadManager->getEnv(boost::this_thread::get_id());

	jstring shortname = environment->NewStringUTF(_shortname.c_str());
	_bucket->shortname = _shortname;

	jclass moduleClass = environment->FindClass("com/unown/tap/jni/NativeTAP");
	jmethodID securityMethod = environment->GetStaticMethodID(moduleClass, "getSecurityDefinition", "(Ljava/lang/String;)Lcom/unown/core/commons/types/instrument/SecurityDefinition;");
	jobject securityDefinition = environment->CallStaticObjectMethod(moduleClass, securityMethod, shortname);

	bool success = securityDefinition != NULL;

	if(success)
	{
		_bucket->securityID = getFieldAsString(environment, securityDefinition, "securityId");
		_bucket->pxExponent = getFieldAsInt(environment, securityDefinition, "decimalLength");
	}
	environment->DeleteLocalRef(shortname); //due to being called by native thread
	return success;
}

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024OrderEntry_register(JNIEnv* _env, jobject _this, jobject _callback, jobject _config)
{
	boost::mutex::scoped_lock lock(registration_mutex);

	if(!order_registered)
	{
		jint rs = _env->GetJavaVM(&jvm);
		assert (rs == JNI_OK);

		threadManager = new JNIThreadManager(jvm);
		JavaThreadID threadID = boost::this_thread::get_id();
		threadManager->cacheJavaThreadEnv(threadID, _env);

		tap_orderentry = new TAPOrderEntryImpl(threadManager, &order_callback, &retrieve_security_by_id_exdest, &retrieve_security_by_shortname, &log_main);

		std::string address = getFieldAsString(_env, _config, "address");
		std::string port = getFieldAsString(_env, _config, "port");
		std::string brokerID = getFieldAsString(_env, _config, "brokerID");
		std::string userID = getFieldAsString(_env, _config, "userID");
		std::string password = getFieldAsString(_env, _config, "password");
		std::string exdest = getFieldAsString(_env, _config, "exdest");
		std::string authcode = getFieldAsString(_env, _config, "authcode");

		jclass callback_class = _env->GetObjectClass(_callback);
		order_callback_method_id = _env->GetMethodID(callback_class, "onCallback", "(JI)V");

		order_callbackObj = _env->NewGlobalRef(_callback);

		init_logger();

		tap_orderentry->configure(address, port, brokerID, userID, password, exdest,authcode);

		order_registered = 1;

		threadManager->releaseJavaThreadEnv(threadID);
	}
}

// JNIEXPORT void JNICALL Java_com_unown_ctp_jni_NativeCTP_00024OrderEntry_deregister(JNIEnv* _env, jobject _this)
// {
// 	destroy_logger();
// }

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024OrderEntry_connect(JNIEnv* _env, jobject _this)
{
	tap_orderentry->connect();
}

// JNIEXPORT void JNICALL Java_com_unown_ctp_jni_NativeCTP_00024OrderEntry_disconnect(JNIEnv* _env, jobject _this)
// {
// 	ctp_orderentry->disconnect();
// }

// JNIEXPORT void JNICALL Java_com_unown_ctp_jni_NativeCTP_00024OrderEntry_request(JNIEnv* _env, jobject _this, jbyteArray _msg, jint _length)
// {
// 	if(!order_req_registered)
// 	{
// 		threadManager->cacheJavaThreadEnv(boost::this_thread::get_id(), _env);
// 		order_req_registered = 1;
// 	}

// 	char msg[_length];
// 	_env->GetByteArrayRegion(_msg, 0, _length, reinterpret_cast<jbyte*>(msg));

// 	ctp_orderentry->request(msg, _length);
// }
///---------------------------------------------------------------------------------------------------
JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024MarketData_register(JNIEnv* _env, jobject _this, jobject _callback, jobject _config)
{
	boost::mutex::scoped_lock lock(registration_mutex);

	if(!md_registered)
	{
		jint rs = _env->GetJavaVM(&jvm);
		assert (rs == JNI_OK);

		threadManager = new JNIThreadManager(jvm);
		JavaThreadID threadID = boost::this_thread::get_id();

		tap_marketdata = new TAPMarketDataImpl(threadManager, &md_callback, &retrieve_security_by_id_exdest, &log_main);

		std::string address = getFieldAsString(_env, _config, "address");
		std::string port = getFieldAsString(_env, _config, "port");
		std::string brokerID = getFieldAsString(_env, _config, "brokerID");
		std::string userID = getFieldAsString(_env, _config, "userID");
		std::string password = getFieldAsString(_env, _config, "password");
		std::string exdest = getFieldAsString(_env, _config, "exdest");
		std::string authcode = getFieldAsString(_env, _config, "authcode");

		jclass callback_class = _env->GetObjectClass(_callback);
		md_callback_method_id = _env->GetMethodID(callback_class, "onCallback", "(JI)V");

		md_callbackObj = _env->NewGlobalRef(_callback);

		init_logger();

		tap_marketdata->configure(address, port, brokerID, userID, password, exdest,authcode);

		md_registered = 1;

		threadManager->releaseJavaThreadEnv(threadID);
	}
}

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024MarketData_deregister(JNIEnv* _env, jobject _this)
{
	destroy_logger();
}

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024MarketData_connect(JNIEnv* _env, jobject _this)
{
	tap_marketdata->connect();
}

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024MarketData_disconnect(JNIEnv* _env, jobject _this)
{
	tap_marketdata->disconnect();
}

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024MarketData_subscribe(JNIEnv* _env, jobject _this, jobject _contract)
{
	JavaThreadID id = boost::this_thread::get_id();
	threadManager->cacheJavaThreadEnv(id, _env);
	std::string exchangeNo = getFieldAsString(_env, _contract, "exchangeNo");
	std::string commodityType = getFieldAsString(_env, _contract, "commodityType");
	std::string commodityNo = getFieldAsString(_env, _contract, "commodityNo");
	std::string contractNo = getFieldAsString(_env, _contract, "contractNo");

	TapAPIContract contract;
	memset(&contract, 0, sizeof(contract));
	strcpy(contract.Commodity.ExchangeNo, exchangeNo.c_str());
	contract.Commodity.CommodityType = commodityType.c_str()[0];
	strcpy(contract.Commodity.CommodityNo, commodityNo.c_str());
	strcpy(contract.ContractNo1, contractNo.c_str());
	contract.CallOrPutFlag1 = TAPI_CALLPUT_FLAG_NONE;
	contract.CallOrPutFlag2 = TAPI_CALLPUT_FLAG_NONE;
	TAPIUINT32 m_uiSessionID = 0;

	tap_marketdata->subscribe(&contract);
	threadManager->releaseJavaThreadEnv(id);
}

JNIEXPORT void JNICALL Java_com_unown_tap_jni_NativeTAP_00024MarketData_unsubscribe(JNIEnv* _env, jobject _this, jobject _contract)
{
	JavaThreadID id = boost::this_thread::get_id();
	threadManager->cacheJavaThreadEnv(id, _env);
	std::string exchangeNo = getFieldAsString(_env, _contract, "exchangeNo");
	std::string commodityType = getFieldAsString(_env, _contract, "commodityType");
	std::string commodityNo = getFieldAsString(_env, _contract, "commodityNo");
	std::string contractNo = getFieldAsString(_env, _contract, "contractNo");

	TapAPIContract contract;
	memset(&contract, 0, sizeof(contract));
	strcpy(contract.Commodity.ExchangeNo, exchangeNo.c_str());
	contract.Commodity.CommodityType = commodityType.c_str()[0];
	strcpy(contract.Commodity.CommodityNo, commodityNo.c_str());
	strcpy(contract.ContractNo1, contractNo.c_str());
	contract.CallOrPutFlag1 = TAPI_CALLPUT_FLAG_NONE;
	contract.CallOrPutFlag2 = TAPI_CALLPUT_FLAG_NONE;
	TAPIUINT32 m_uiSessionID = 0;

	tap_marketdata->unsubscribe(&contract);
	threadManager->releaseJavaThreadEnv(id);
}
