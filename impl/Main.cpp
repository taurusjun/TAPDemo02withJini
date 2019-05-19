#include "TAPMarketDataImpl.h"

#include <iostream>
#include <string.h>
#include <stdio.h>
using namespace std;

void ShowBuf(char* buf,int len)
{
    printf("===================\n");
        for(int i=0; i < len; ++i)
    {
        if(i%32==0){
            printf("\n");
        }
        printf("0x%x(%c) ", buf[i], buf[i]);
    }
    printf("===================\n");
}

inline
void log_main(std::string _line)
{
	cout << _line << endl;
}

int main(int argc, char* argv[])
{
    TAPMarketDataImpl* tapmd = new TAPMarketDataImpl(NULL,NULL,NULL,&log_main);
    tapmd->configure("","","","","");
    bool rslt = tapmd->connect();

    if(rslt){
        tapmd->subscribe("cuuuu");
    }

    while(true){

    }
    
    return 0;
}