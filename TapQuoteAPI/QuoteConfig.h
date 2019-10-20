#include "string.h"

#ifndef QUOTE_CONFIG_H
#define QUOTE_CONFIG_H

//行情IP地址与端口 联通
#define DEFAULT_IP1		("61.163.243.173")
#define DEFAULT_PORT1	(6161)
//行情IP地址与端口 电信
#define DEFAULT_IP		("123.161.206.213")
#define DEFAULT_PORT	(6161)

extern bool isdisconnect ;

//授权码
#define DEFAULT_AUTHCODE1	("67EA896065459BECDFDB924B29CB7DF1946CED\
32E26C1EAC946CED32E26C1EAC946CED32E26C1EAC946CED32E26C1EAC5211AF9FEE\
541DDE41BCBAB68D525B0D111A0884D847D57163FF7F329FA574E7946CED32E26C1E\
AC946CED32E26C1EAC733827B0CE853869ABD9B8F170E14F8847D3EA0BF4E191F5D9\
7B3DFE4CCB1F01842DD2B3EA2F4B20CAD19B8347719B7E20EA1FA7A3D1BFEFF22290\
F4B5C43E6C520ED5A40EC1D50ACDF342F46A92CCF87AEE6D73542C42EC17818349C7\
DEDAB0E4DB16977714F873D505029E27B3D57EB92D5BEDA0A710197EB67F94BB1892\
B30F58A3F211D9C3B3839BE2D73FD08DD776B9188654853DDA57675EBB7D6FBBFC")

#define DEFAULT_AUTHCODE      ("B112F916FE7D27BCE7B97EB620206457946CED32E26C1EAC946CED32E26C1EAC946CED32E26C1EAC946CED32E26C1EAC5211AF9FEE541DDE9D6F622F72E25D5DEF7F47AA93A738EF5A51B81D8526AB6A9D19E65B41F59D6A946CED32E26C1EACCAF8D4C61E28E2B1ABD9B8F170E14F8847D3EA0BF4E191F5DCB1B791E63DC196D1576DEAF5EC563CA3E560313C0C3411B45076795F550EB050A62C4F74D5892D2D14892E812723FAC858DEBD8D4AF9410729FB849D5D8D6EA48A1B8DC67E037381A279CE9426070929D5DA085659772E24A6F5EA52CF92A4D403F9E46083F27B19A88AD99812DADA44100324759F9FD1964EBD4F2F0FB50B51CD31C0B02BB437")

//用户名密码
// #define DEFAULT_USERNAME	("Q48753284")
// #define DEFAULT_PASSWORD	("335236")
#define DEFAULT_USERNAME	("")
#define DEFAULT_PASSWORD	("")

//订阅的行情
///------------- 日盘 9:00~15:00 -------------
#define DEFAULT_EXCHANGE_NO		("CFFEX")
#define DEFAULT_COMMODITY_TYPE	(TAPI_COMMODITY_TYPE_FUTURES)
#define DEFAULT_COMMODITY_NO	("IF")
#define DEFAULT_CONTRACT_NO		("1906")
///-------------------------------

///------------- 夜盘 21:00~1:00 -------------
// #define DEFAULT_EXCHANGE_NO		("SHFE")
// #define DEFAULT_COMMODITY_TYPE	(TAPI_COMMODITY_TYPE_FUTURES)
// #define DEFAULT_COMMODITY_NO	("AU")
// #define DEFAULT_CONTRACT_NO		("1908")
///-------------------------------

template<size_t size> inline void APIStrncpy(char (&Dst)[size], const char* source)
{
#ifdef WIN32
    strncpy_s(Dst, source, sizeof(Dst) - 1);
#endif
    
#ifdef linux
    strncpy(Dst, source, sizeof(Dst));
    //Dst[size] = '\0';
#endif
}

#endif // QUOTE_CONFIG_H
