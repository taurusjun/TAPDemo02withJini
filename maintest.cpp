#include <iostream>
#include <boost/algorithm/string.hpp>
#include "jni/JNIThreadManager.h"
#include "impl/TAPOrderEntryImpl.h"
#include "impl/TAPSimpleEvent.h"
#include "collections/ConcurrentQueue.hpp"
#include <cctype>

using namespace std;

#define DEFAULT_AUTHCODE	("67EA896065459BECDFDB924B29CB7DF1946CED\
32E26C1EAC946CED32E26C1EAC946CED32E26C1EAC946CED32E26C1EAC5211AF9FEE\
541DDE41BCBAB68D525B0D111A0884D847D57163FF7F329FA574E7946CED32E26C1E\
AC946CED32E26C1EAC733827B0CE853869ABD9B8F170E14F8847D3EA0BF4E191F5D9\
7B3DFE4CCB1F01842DD2B3EA2F4B20CAD19B8347719B7E20EA1FA7A3D1BFEFF22290\
F4B5C43E6C520ED5A40EC1D50ACDF342F46A92CCF87AEE6D73542C42EC17818349C7\
DEDAB0E4DB16977714F873D505029E27B3D57EB92D5BEDA0A710197EB67F94BB1892\
B30F58A3F211D9C3B3839BE2D73FD08DD776B9188654853DDA57675EBB7D6FBBFC")

//--------------------------------------------------------
static const char* STRING_SIGNATURE = "Ljava/lang/String;";
static const char* LEVEL_SIGNATURE = "Lorg/apache/logging/log4j/Level;";

JNIThreadManager* threadManager;

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

////---------------------------------- inline functions ----------------------------------
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
		std::string name = "ctp-native";

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
void order_callback_mock(long _addr, int _length)
{
	cout << "call back called: " << "[addr=" << _addr << "],[length="<<_length<<"]"<<endl;
}

inline
void log_main(std::string _line)
{
	loggingQueue.push(_line);
	cout << _line <<endl;
}

inline
bool retrieve_security_by_id_exdest(std::string _secID, std::string _exdest, SecurityDefinition* _bucket)
{
	JNIEnv* environment = threadManager->getEnv(boost::this_thread::get_id());
	jstring securityID = environment->NewStringUTF(_secID.c_str());
	jstring exdestination = environment->NewStringUTF(_exdest.c_str());
	_bucket->securityID = _secID;

	jclass moduleClass = environment->FindClass("com/unown/ctp/jni/NativeCTP");
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
bool retrieve_security_by_id_exdest_mock(std::string _secID, std::string _exdest, SecurityDefinition* _bucket)
{
	_bucket->securityID = _secID;


  _bucket->shortname = "mockname";
  _bucket->pxExponent = 1;

	return true;
}

inline
bool retrieve_security_by_shortname(std::string _shortname, SecurityDefinition* _bucket)
{
	JNIEnv* environment = threadManager->getEnv(boost::this_thread::get_id());

	jstring shortname = environment->NewStringUTF(_shortname.c_str());
	_bucket->shortname = _shortname;

	jclass moduleClass = environment->FindClass("com/unown/ctp/jni/NativeCTP");
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

inline
bool retrieve_security_by_shortname_mock(std::string _shortname, SecurityDefinition* _bucket)
{
	_bucket->shortname = _shortname;
	_bucket->securityID = "IF1901";
	_bucket->pxExponent = 1;

	return true;
}

////--------------------------------------------------------------------------------------
void genKey(string& key, const string& instrumentID, const int& sideValue, const int& positionTypeValue)
{
	key=instrumentID+"|"+std::to_string(sideValue)+"|"+std::to_string(positionTypeValue);
}

void deGenKey(const string& key, string& instrumentID, int& sideValue, int& positionTypeValue)
{
	std::vector<std::string> strs;
	boost::split(strs, key, boost::is_any_of("|"));
	instrumentID = strs.at(0);
	sideValue = atoi(strs.at(1).c_str());
	positionTypeValue = atoi(strs.at(2).c_str());
}

int main()
{
  cout <<"-------- main test --------" << endl;
//   std::string s1="CF001";
//   size_t len = s1.size();
//   string p="";
//   for (size_t i = 0; i < len; i++)
//   {
// 	  char c = s1.at(i);
// 	  if(isdigit(c)){
// 	//   if(isalpha(c)){
// 		  p=p+c;
// 	  }
//   }
  
//   cout << p <<endl;
// 	std::string s2 = "TAP-ZCE";
// 	std::string s3=s2;
// 	std::string m="TAP-";
// 	boost::replace_first(s3,m,"");
//    cout << s2 <<endl;
//    cout << s3 <<endl;


//   TAPISTR_10 test1="001";
//   string c2=test1;
//   cout << c2 <<endl;
  // string line = "CFFEX:F:IF:1910:A"; 
  // std::vector<std::string> strs;
  // boost::split(strs, line, boost::is_any_of(":"));
  // for(int i=0;i<strs.size();i++){
  //   cout << strs.at(i) << endl;
  // }
  // cout <<"concation: "<< strs[2]+strs[3]<<endl;
  // cout << strs[4].compare("A")<<endl;
  // cout << strs[4].compare("a")<<endl;
  // cout << strs[4].compare("b")<<endl;
//

//   threadManager = new JNIThreadManager(jvm);
//   JavaThreadID threadID = boost::this_thread::get_id();
//   tap_orderentry = new TAPOrderEntryImpl(threadManager, &order_callback_mock, &retrieve_security_by_id_exdest_mock, &retrieve_security_by_shortname_mock, &log_main);
//   tap_orderentry->configure("123.161.206.213","6060","","Q48753284","335236","",DEFAULT_AUTHCODE);
//   tap_orderentry->connect();

  string key="";
  string insId="cu001";
  const Side* side=Side::BUY;
  const PositionType* posType=PositionType::OPEN_TODAY;

  genKey(key,insId,side->value,posType->value);
  cout << key<<endl;	

  int sideValue=0;
  int posTypeValue=0;

  deGenKey(key,insId,sideValue,posTypeValue);
  cout << insId <<" " << Side::getSide(sideValue)->value << " " << PositionType::getPositionType(posTypeValue)->value << endl;

  return 0;
}
