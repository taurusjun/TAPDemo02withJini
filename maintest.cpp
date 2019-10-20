#include <iostream>
#include <boost/algorithm/string.hpp>
#include "jni/JNIThreadManager.h"
#include "impl/TAPOrderEntryImpl.h"
#include "impl/TAPSimpleEvent.h"
#include "collections/ConcurrentQueue.hpp"

using namespace std;

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
////--------------------------------------------------------------------------------------

int main()
{
  cout <<"-------- main test --------" << endl;
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

  threadManager = new JNIThreadManager(jvm);
  JavaThreadID threadID = boost::this_thread::get_id();
 	tap_orderentry = new TAPOrderEntryImpl(threadManager, &order_callback, &retrieve_security_by_id_exdest, &retrieve_security_by_shortname, &log_main);


  return 0;
}
