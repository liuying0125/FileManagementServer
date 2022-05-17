#include <stdio.h>
#include <cstring>
#include "md5.h"



int main ()
{


    MD5_CTX md5;
    MD5Init(&md5);                         //初始化用于md5加密的结构

    unsigned char encrypt[200] = {"hello"};     //存放于加密的信息
    unsigned char decrypt[17];       //存放加密后的结果


    MD5Update(&md5,encrypt,strlen((char *)encrypt));   //对欲加密的字符进行加密
    MD5Final(decrypt,&md5);                                            //获得最终结果

    printf("加密前:%s\n加密后:",encrypt);

    for(int i=0;i<16;i++)
        printf("%2x",decrypt[i]);  
    return 0;
}






